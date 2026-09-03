// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/OpfsFileReader.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/Threading.h"

#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <cstring>
#include <mutex>
#include <thread>

static constexpr size_t CHUNK_SIZE = 128 * 1024;

// Filled in by pcsx2_web_opfs_open (web/host/pcsx2_web_opfs.js); the layout is shared with the
// JS side, so keep the field order and sizes in sync.
struct OpfsOpenResult
{
	s32 handle;
	s32 error;
	double size;
	char mode[16];
	char message[256];
};
static_assert(sizeof(OpfsOpenResult) == 288);
static_assert(offsetof(OpfsOpenResult, size) == 8);
static_assert(offsetof(OpfsOpenResult, mode) == 16);
static_assert(offsetof(OpfsOpenResult, message) == 32);

extern "C" {
// Implemented in web/host/pcsx2_web_opfs.js. All three run on the OPFS thread: open completes
// asynchronously and signals ctx, read and close are synchronous.
void pcsx2_web_opfs_open(em_proxying_ctx* ctx, const char* path, OpfsOpenResult* result);
double pcsx2_web_opfs_read(int handle, void* dst, double offset, int length);
void pcsx2_web_opfs_close(int handle);
}

namespace
{
	// The pthread that owns every sync access handle. It leaves its entry point through
	// emscripten_exit_with_live_runtime(), so its worker sits in the event loop where promises
	// resolve and proxied work is executed as it arrives. Created on first use and never joined.
	class OpfsThread
	{
	public:
		static OpfsThread& Get()
		{
			static OpfsThread* instance = new OpfsThread();
			return *instance;
		}

		bool Sync(const std::function<void()>& func)
		{
			return m_queue.proxySync(m_thread.native_handle(), func);
		}

		bool SyncWithCtx(const std::function<void(emscripten::ProxyingQueue::ProxyingCtx)>& func)
		{
			return m_queue.proxySyncWithCtx(m_thread.native_handle(), func);
		}

	private:
		OpfsThread()
		{
			m_thread = std::thread([this]() {
				Threading::SetNameOfCurrentThread("OPFS");
				{
					std::lock_guard lock(m_mutex);
					m_started = true;
				}
				m_condition.notify_all();
				emscripten_exit_with_live_runtime();
			});

			std::unique_lock lock(m_mutex);
			m_condition.wait(lock, [this]() { return m_started; });
		}

		emscripten::ProxyingQueue m_queue;
		std::mutex m_mutex;
		std::condition_variable m_condition;
		bool m_started = false;
		std::thread m_thread;
	};

	std::mutex s_last_mode_mutex;
	std::string s_last_mode;
} // namespace

bool Opfs::IsOpfsPath(std::string_view path)
{
	return path.size() > MOUNT_ROOT.size() && path.substr(0, MOUNT_ROOT.size()) == MOUNT_ROOT;
}

std::string_view Opfs::RelativePath(std::string_view path)
{
	return IsOpfsPath(path) ? path.substr(MOUNT_ROOT.size()) : path;
}

std::string Opfs::GetLastHandleMode()
{
	std::lock_guard lock(s_last_mode_mutex);
	return s_last_mode;
}

Opfs::File::File() = default;

Opfs::File::~File()
{
	Close();
}

bool Opfs::File::Open(std::string_view relative_path, Error* error)
{
	Close();

	const std::string path(relative_path);
	OpfsOpenResult result = {};
	result.handle = -1;
	const bool proxied = OpfsThread::Get().SyncWithCtx([&](emscripten::ProxyingQueue::ProxyingCtx ctx) {
		pcsx2_web_opfs_open(ctx.ctx, path.c_str(), &result);
	});
	if (!proxied)
	{
		Error::SetStringView(error, "The OPFS thread did not accept the open request.");
		return false;
	}
	if (result.error != 0 || result.handle < 0)
	{
		result.message[sizeof(result.message) - 1] = '\0';
		Error::SetStringFmt(error, "Failed to open '{}' in origin-private storage: {}", path, result.message);
		return false;
	}

	result.mode[sizeof(result.mode) - 1] = '\0';
	m_handle = result.handle;
	m_size = static_cast<u64>(result.size);
	m_mode = result.mode;
	{
		std::lock_guard lock(s_last_mode_mutex);
		s_last_mode = m_mode;
	}
	Console.WriteLnFmt("OPFS: opened '{}' ({} bytes, {} handle)", path, m_size, m_mode);
	return true;
}

void Opfs::File::Close()
{
	if (m_handle < 0)
		return;

	const int handle = m_handle;
	m_handle = -1;
	m_size = 0;
	m_mode.clear();
	OpfsThread::Get().Sync([handle]() { pcsx2_web_opfs_close(handle); });
}

s64 Opfs::File::Read(void* dst, u64 offset, u32 length)
{
	if (m_handle < 0)
		return -1;
	if (length == 0)
		return 0;

	double got = -1.0;
	const int handle = m_handle;
	if (!OpfsThread::Get().Sync([&]() { got = pcsx2_web_opfs_read(handle, dst, static_cast<double>(offset), static_cast<int>(length)); }))
		return -1;

	return (got < 0.0) ? -1 : static_cast<s64>(got);
}

OpfsFileReader::OpfsFileReader() = default;

OpfsFileReader::~OpfsFileReader()
{
	pxAssert(!m_file.IsOpen());
}

bool OpfsFileReader::Open2(std::string filename, Error* error)
{
	m_filename = std::move(filename);
	if (!m_file.Open(Opfs::RelativePath(m_filename), error))
		return false;

	if (m_file.GetSize() == 0)
	{
		Error::SetStringView(error, "Failed to determine file size.");
		Close2();
		return false;
	}

	return true;
}

ThreadedFileReader::Chunk OpfsFileReader::ChunkForOffset(u64 offset)
{
	ThreadedFileReader::Chunk chunk = {};
	const u64 file_size = m_file.GetSize();
	if (offset >= file_size)
	{
		chunk.chunkID = -1;
	}
	else
	{
		chunk.chunkID = offset / CHUNK_SIZE;
		chunk.length = static_cast<u32>(std::min<u64>(file_size - offset, CHUNK_SIZE));
		chunk.offset = static_cast<u64>(chunk.chunkID) * CHUNK_SIZE;
	}

	return chunk;
}

int OpfsFileReader::ReadChunk(void* dst, s64 blockID)
{
	if (blockID < 0)
		return -1;

	const u64 file_size = m_file.GetSize();
	const u64 file_offset = static_cast<u64>(blockID) * CHUNK_SIZE;
	if (file_offset >= file_size)
		return -1;

	const u32 read_size = static_cast<u32>(std::min<u64>(file_size - file_offset, CHUNK_SIZE));
	const s64 got = m_file.Read(dst, file_offset, read_size);
	return (got == static_cast<s64>(read_size)) ? static_cast<int>(read_size) : 0;
}

void OpfsFileReader::Close2()
{
	m_file.Close();
}

u32 OpfsFileReader::GetBlockCount() const
{
	return static_cast<u32>(m_file.GetSize() / m_blocksize);
}
