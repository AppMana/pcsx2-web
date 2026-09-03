// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "CDVD/ThreadedFileReader.h"

#include <string>
#include <string_view>

// Disc images in the browser's origin-private file system. A path of the form
// "/opfs/<relative path>" names a stored file; the same mount root is what the
// page's storage module reports for imported files.
namespace Opfs
{
	static constexpr std::string_view MOUNT_ROOT = "/opfs/";

	bool IsOpfsPath(std::string_view path);
	std::string_view RelativePath(std::string_view path);

	// The sync access handle mode the browser granted for the most recently opened file
	// ("read-only" on Chrome, "readwrite" where the mode option is not understood), empty
	// when nothing has been opened.
	std::string GetLastHandleMode();

	// A read-only file in origin-private storage. The FileSystemSyncAccessHandle is owned by a
	// dedicated OPFS thread that sits in its event loop, so the asynchronous open can complete
	// there; every read is proxied to that thread and lands directly in the caller's buffer.
	// Reads may be issued from any pthread.
	class File
	{
	public:
		File();
		~File();

		File(const File&) = delete;
		File& operator=(const File&) = delete;

		bool Open(std::string_view relative_path, Error* error);
		void Close();

		bool IsOpen() const { return m_handle >= 0; }
		u64 GetSize() const { return m_size; }
		const std::string& GetHandleMode() const { return m_mode; }

		// Reads up to length bytes at offset into dst. Returns the number of bytes read (short at
		// the end of the file, zero past it) or -1 on error.
		s64 Read(void* dst, u64 offset, u32 length);

	private:
		int m_handle = -1;
		u64 m_size = 0;
		std::string m_mode;
	};
} // namespace Opfs

class OpfsFileReader final : public ThreadedFileReader
{
	DeclareNoncopyableObject(OpfsFileReader);

	Opfs::File m_file;

public:
	OpfsFileReader();
	~OpfsFileReader() override;

	bool Open2(std::string filename, Error* error) override;

	Chunk ChunkForOffset(u64 offset) override;
	int ReadChunk(void* dst, s64 blockID) override;

	void Close2() override;

	u32 GetBlockCount() const override;
};
