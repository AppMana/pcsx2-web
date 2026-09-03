// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Browser host for the PCSX2 core.  Modeled on pcsx2-gsrunner/Main.cpp: settings live in
// memory, the window is surfaceless, audio is the null stream, and the emulator runs on a
// dedicated CPU pthread that is driven through the pcsx2_web_* exports.

#include <emscripten.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fmt/format.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"
#include "common/Threading.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/Counters.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GSXXH.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GS/Renderers/Common/GSRenderer.h"
#include "pcsx2/GSDumpReplayer.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/Host/AudioStream.h"
#include "pcsx2/Host/WebAudioStream.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/Recording/PadData.h"
#include "pcsx2/TraceHash.h"
#include "pcsx2/VMManager.h"

#include "svnrev.h"

namespace WebHost
{
	enum Status : int
	{
		Uninitialized = -1,
		Idle = 0,
		Initializing = 1,
		Running = 2,
		Paused = 3,
		Stopping = 4,
		BootFailed = 5,
		CPUThreadFailed = 6,
	};

	static bool InitializeConfig();
	static void SettingsOverride();
	static void CPUThreadMain();
	static void ProcessCPUThreadTasks();
	static void LogCallback(LOGLEVEL level, ConsoleColors color, std::string_view message);

	static MemorySettingsInterface s_settings_interface;

	static Threading::Thread s_cpu_thread;
	static std::atomic<bool> s_cpu_thread_ready{false};
	static std::atomic<bool> s_cpu_thread_failed{false};
	static std::atomic<bool> s_boot_failed{false};
	static std::atomic<bool> s_initialized{false};

	static std::mutex s_cpu_task_mutex;
	static std::deque<std::function<void()>> s_cpu_tasks;
	static Threading::KernelSemaphore s_cpu_task_sema;
	static std::optional<VMBootParameters> s_pending_boot;

	static std::mutex s_host_task_mutex;
	static std::deque<std::function<void()>> s_host_tasks;

	// EE/IOP console lines in the tracerunner's tty.txt format.
	static std::mutex s_tty_mutex;
	static std::string s_tty_buffer;

	enum TraceMask : u32
	{
		TraceCPU = 1u << 0,
		TraceAudio = 1u << 1,
	};

	// cpu.jsonl and audio.jsonl records, appended at every vsync and drained by pcsx2_web_trace_read.
	static std::atomic<u32> s_trace_mask{0};
	static std::atomic<u32> s_trace_ram_every{0};

	// Stops the VM at this many vsyncs, the way pcsx2-tracerunner's -frames does (0 = never).
	static std::atomic<u32> s_frame_limit{0};
	static std::mutex s_trace_mutex;
	static std::string s_trace_buffer;

	alignas(4) static std::atomic<u32> s_frame_count{0};

	// Pad states in the .p2m2 per-port layout (PadData's 18 byte array): two active-low button
	// groups, right and left analogs, twelve pressures. A live state set through
	// pcsx2_web_set_pad takes effect at the next vsync; the schedule holds frame-indexed states
	// that take effect at the vsync whose g_FrameCount equals their frame, at the point where
	// the input recording replay would override the pad, so a replayed schedule and a .p2m2
	// recording drive the guest identically.
	using PadBytes = std::array<u8, 18>;
	static constexpr u32 PAD_PORTS = 2;

	struct PadScheduleEntry
	{
		u32 frame;
		u32 port;
		PadBytes bytes;
	};

	static std::mutex s_pad_mutex;
	static std::array<std::optional<PadBytes>, PAD_PORTS> s_pad_pending;
	static std::array<std::optional<PadBytes>, PAD_PORTS> s_pad_current;
	static std::vector<PadScheduleEntry> s_pad_schedule;
	static size_t s_pad_schedule_next = 0;
	static std::atomic<u32> s_pad_applied_count{0};

	static PadBytes PadBytesFromState(u32 buttons, u32 lx, u32 ly, u32 rx, u32 ry, const u8* pressures);
	static void ApplyPadOnCPUThread();

	// GS presentation: the canvas the page transferred (empty = surfaceless), set before boot.
	static std::string s_canvas_selector;
	static u32 s_canvas_width = 640;
	static u32 s_canvas_height = 480;

	// Readback mode: 0 = none (no readbacks at all), 1 = async (frame captures through the GS
	// thread's event loop). Synchronous readbacks need the PCSX2_WEB_SYNC_READBACK=asyncify build.
	static std::atomic<int> s_readback_mode{0};

	// Frame capture: every Nth presented frame (0 = off) and the listed oracle frame numbers are read
	// back and queued for pcsx2_web_frame_read. GS dumps loop this many times, like pcsx2-gsrunner -loop.
	static std::atomic<u32> s_frame_capture_every{0};
	static std::vector<u32> s_frame_capture_list;
	static std::atomic<int> s_dump_loop_count{1};

	struct CapturedFrame
	{
		u32 frame;
		u32 width;
		u32 height;
		u32 dump_frame;
		s32 dump_loop;
		u32 oracle_frame;
		u64 render_ns;
		u32 changed_pixels;
		u64 hash;
		std::vector<u32> pixels;
	};
	static constexpr size_t MAX_QUEUED_FRAMES = 64;
	static std::mutex s_frame_mutex;
	static std::deque<CapturedFrame> s_frames;

	// GS thread state: previous capture for the changed pixel count, present timing, and the dump
	// replayer's position as pcsx2-gsrunner mirrors it (posted from the CPU thread at every vsync).
	static std::vector<u32> s_last_capture;
	static u32 s_last_capture_width = 0;
	static u32 s_last_capture_height = 0;
	static u64 s_last_present_ticks = 0;
	static u32 s_dump_frame_number = 0;
	static s32 s_dump_loop_number = 0;
	// g_FrameCount + 1 as pcsx2-tracerunner mirrors it to its GS thread, the number its PNGs carry.
	static u32 s_oracle_frame_number = 0;

	static std::mutex s_gpu_info_mutex;
	static std::string s_gpu_driver_info;

	static void CaptureCompleted(u32 frame, u32 dump_frame, s32 dump_loop, u32 oracle_frame, u64 render_ns, u32 width, u32 height, std::vector<u32> pixels);
} // namespace WebHost

WebHost::PadBytes WebHost::PadBytesFromState(u32 buttons, u32 lx, u32 ly, u32 rx, u32 ry, const u8* pressures)
{
	// buttons: digital1 in bits 0-7 (select, l3, r3, start, up, right, down, left) and digital2 in
	// bits 8-15 (l2, r2, l1, r1, triangle, circle, cross, square), pressed bits set, the same
	// layout as the pad's own digital bytes and the kit's DIGITAL1/DIGITAL2 constants.
	const u8 digital1 = static_cast<u8>(buttons & 0xffu);
	const u8 digital2 = static_cast<u8>((buttons >> 8) & 0xffu);

	PadBytes bytes = {};
	bytes[0] = static_cast<u8>(~digital1);
	bytes[1] = static_cast<u8>(~digital2);
	bytes[2] = static_cast<u8>(std::min<u32>(rx, 255));
	bytes[3] = static_cast<u8>(std::min<u32>(ry, 255));
	bytes[4] = static_cast<u8>(std::min<u32>(lx, 255));
	bytes[5] = static_cast<u8>(std::min<u32>(ly, 255));

	if (pressures)
	{
		std::memcpy(&bytes[6], pressures, 12);
	}
	else
	{
		// right, left, up, down, triangle, circle, cross, square, l1, r1, l2, r2: full pressure when
		// pressed, the same rule the kit's input recording writer applies.
		static constexpr std::array<std::pair<u8, u8>, 12> pressure_bits = {{
			{1, 0x20}, {1, 0x80}, {1, 0x10}, {1, 0x40},
			{2, 0x10}, {2, 0x20}, {2, 0x40}, {2, 0x80},
			{2, 0x04}, {2, 0x08}, {2, 0x01}, {2, 0x02},
		}};
		for (size_t i = 0; i < pressure_bits.size(); i++)
		{
			const u8 group = (pressure_bits[i].first == 1) ? digital1 : digital2;
			bytes[6 + i] = (group & pressure_bits[i].second) ? 255 : 0;
		}
	}

	return bytes;
}

void WebHost::ApplyPadOnCPUThread()
{
	if (!VMManager::HasValidVM())
		return;

	const u32 frame = g_FrameCount;
	std::array<std::optional<PadBytes>, PAD_PORTS> states;
	{
		std::lock_guard lock(s_pad_mutex);
		for (u32 port = 0; port < PAD_PORTS; port++)
		{
			if (s_pad_pending[port].has_value())
			{
				s_pad_current[port] = s_pad_pending[port];
				s_pad_pending[port].reset();
			}
		}
		while (s_pad_schedule_next < s_pad_schedule.size() && s_pad_schedule[s_pad_schedule_next].frame <= frame)
		{
			const PadScheduleEntry& entry = s_pad_schedule[s_pad_schedule_next++];
			s_pad_current[entry.port] = entry.bytes;
		}
		states = s_pad_current;
	}

	for (u32 port = 0; port < PAD_PORTS; port++)
	{
		if (!states[port].has_value())
			continue;

		PadData(static_cast<int>(port), 0, states[port].value()).OverrideActualController();
		s_pad_applied_count.fetch_add(1, std::memory_order_relaxed);
	}
}

void WebHost::LogCallback(LOGLEVEL level, ConsoleColors color, std::string_view message)
{
	const char* prefix;
	if (color == Color_Cyan)
		prefix = "EE: ";
	else if (color == Color_Yellow)
		prefix = "IOP: ";
	else
		return;

	std::lock_guard lock(s_tty_mutex);
	s_tty_buffer.append(prefix);
	s_tty_buffer.append(message);
	s_tty_buffer.push_back('\n');
}

bool WebHost::InitializeConfig()
{
	EmuFolders::SetAppRoot();
	if (!EmuFolders::SetResourcesDirectory() || !EmuFolders::SetDataDirectory(nullptr))
		return false;

	const char* error;
	if (!VMManager::PerformEarlyHardwareChecks(&error))
	{
		Console.ErrorFmt("PerformEarlyHardwareChecks failed: {}", error);
		return false;
	}

	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
			Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
			return false;
		}

		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo fi{};
		fi.data = roboto_data;
		fi.exclude_ranges = {};
		fi.face_name = nullptr;
		fi.is_emoji_font = false;
		fonts.push_back(fi);

		ImGuiManager::SetFonts(std::move(fonts));
	}

	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);

	VMManager::SetDefaultSettings(si, true, true, true, true, true);
	SettingsOverride();

	VMManager::Internal::LoadStartupSettings();
	return true;
}

void WebHost::SettingsOverride()
{
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
	s_settings_interface.SetIntValue("EmuCore/GS", "VsyncEnable", false);
	s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::SW));
	s_settings_interface.SetIntValue("EmuCore/GS", "SWExtraThreads", 0);

	s_settings_interface.SetBoolValue("InputSources", "SDL", false);

	// Audio reaches the page through the Web Audio backend when it attaches an AudioContext;
	// without one the stream fills and drops chunks silently. Time stretching is off so that
	// what the SPU2 wrote is what gets hashed and played.
	s_settings_interface.SetStringValue("SPU2/Output", "Backend", "WebAudio");
	s_settings_interface.SetStringValue("SPU2/Output", "SyncMode", "Disabled");

#ifndef PCSX2_WEB_SYNC_READBACK
	// GSDownloadTexture::Map() cannot block on the GS thread's event loop, so the texture cache and
	// GS local memory reads never wait on the GPU; frame captures use the asynchronous map instead.
	s_settings_interface.SetIntValue("EmuCore/GS", "HWDownloadMode", static_cast<int>(GSHardwareDownloadMode::NoReadbacks));
#endif

	s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "vuThread", false);
	s_settings_interface.SetBoolValue("EmuCore", "EnableDiscordPresence", false);
	s_settings_interface.SetBoolValue("Achievements", "Enabled", false);

	Pad::ClearPortBindings(s_settings_interface, 0);
	s_settings_interface.ClearSection("Hotkeys");

	s_settings_interface.SetBoolValue("Logging", "EnableSystemConsole", true);
	s_settings_interface.SetBoolValue("Logging", "EnableTimestamps", false);
	s_settings_interface.SetBoolValue("Logging", "EnableVerbose", true);
	s_settings_interface.SetBoolValue("Logging", "EnableEEConsole", true);
	s_settings_interface.SetBoolValue("Logging", "EnableIOPConsole", true);

	for (u32 i = 0; i < 2; i++)
	{
		s_settings_interface.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);
		s_settings_interface.SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", i + 1).c_str(), "");
	}
}

void WebHost::ProcessCPUThreadTasks()
{
	for (;;)
	{
		std::function<void()> task;
		{
			std::lock_guard lock(s_cpu_task_mutex);
			if (s_cpu_tasks.empty())
				return;
			task = std::move(s_cpu_tasks.front());
			s_cpu_tasks.pop_front();
		}
		task();
	}
}

void WebHost::CPUThreadMain()
{
	if (!VMManager::Internal::CPUThreadInitialize())
	{
		s_cpu_thread_failed.store(true, std::memory_order_release);
		VMManager::Internal::CPUThreadShutdown();
		return;
	}

	VMManager::ApplySettings();
	s_cpu_thread_ready.store(true, std::memory_order_release);

	for (;;)
	{
		s_cpu_task_sema.Wait();
		ProcessCPUThreadTasks();

		std::optional<VMBootParameters> boot;
		{
			std::lock_guard lock(s_cpu_task_mutex);
			boot = std::move(s_pending_boot);
			s_pending_boot.reset();
		}
		if (!boot.has_value())
			continue;

		s_boot_failed.store(false, std::memory_order_release);
		const bool is_dump = VMManager::IsGSDumpFileName(boot->filename);
		GSDumpReplayer::SetIsDumpRunner(is_dump);
		if (VMManager::Initialize(boot.value()) != VMBootResult::StartupSuccess)
		{
			s_boot_failed.store(true, std::memory_order_release);
			continue;
		}

		if (is_dump)
			GSDumpReplayer::SetLoopCount(s_dump_loop_count.load(std::memory_order_acquire));

		VMManager::SetState(VMState::Running);
		while (VMManager::GetState() == VMState::Running || VMManager::GetState() == VMState::Paused)
		{
			if (VMManager::GetState() == VMState::Paused)
			{
				s_cpu_task_sema.Wait();
				ProcessCPUThreadTasks();
				continue;
			}
			VMManager::Execute();
			ProcessCPUThreadTasks();
		}
		VMManager::Shutdown(false);
	}
}

extern "C" {

EMSCRIPTEN_KEEPALIVE int pcsx2_web_init()
{
	if (WebHost::s_initialized.load(std::memory_order_acquire))
		return 0;

	CrashHandler::Install();
	Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);
	Log::SetHostOutputLevel(LOGLEVEL_INFO, &WebHost::LogCallback);

	Console.WriteLnFmt("PCSX2 web host {}", GIT_REV);

	if (!WebHost::InitializeConfig())
	{
		Console.Error("Failed to initialize config.");
		return 1;
	}

	WebHost::s_cpu_thread.SetStackSize(VMManager::EMU_THREAD_STACK_SIZE);
	if (!WebHost::s_cpu_thread.Start(&WebHost::CPUThreadMain))
	{
		Console.Error("Failed to start CPU thread.");
		return 2;
	}

	WebHost::s_initialized.store(true, std::memory_order_release);
	return 0;
}

EMSCRIPTEN_KEEPALIVE int pcsx2_web_set_setting(const char* section, const char* key, const char* value)
{
	if (!section || !key)
		return 1;

	if (value)
		WebHost::s_settings_interface.SetStringValue(section, key, value);
	else
		WebHost::s_settings_interface.DeleteValue(section, key);

	if (WebHost::s_cpu_thread_ready.load(std::memory_order_acquire))
		Host::RunOnCPUThread([]() { VMManager::ApplySettings(); });
	return 0;
}

EMSCRIPTEN_KEEPALIVE int pcsx2_web_boot(const char* path, const char* elf_override)
{
	if (!WebHost::s_initialized.load(std::memory_order_acquire) || !path)
		return 1;

	VMBootParameters params;
	params.filename = path;
	if (elf_override && elf_override[0])
		params.elf_override = elf_override;

	{
		std::lock_guard lock(WebHost::s_cpu_task_mutex);
		WebHost::s_pending_boot = std::move(params);
	}
	WebHost::s_cpu_task_sema.Post();
	return 0;
}

EMSCRIPTEN_KEEPALIVE int pcsx2_web_status()
{
	if (!WebHost::s_initialized.load(std::memory_order_acquire))
		return WebHost::Uninitialized;
	if (WebHost::s_cpu_thread_failed.load(std::memory_order_acquire))
		return WebHost::CPUThreadFailed;
	if (!WebHost::s_cpu_thread_ready.load(std::memory_order_acquire))
		return WebHost::Initializing;
	if (WebHost::s_boot_failed.load(std::memory_order_acquire))
		return WebHost::BootFailed;

	switch (VMManager::GetState())
	{
		case VMState::Initializing:
			return WebHost::Initializing;
		case VMState::Running:
		case VMState::Resetting:
			return WebHost::Running;
		case VMState::Paused:
			return WebHost::Paused;
		case VMState::Stopping:
			return WebHost::Stopping;
		case VMState::Shutdown:
		default:
			return WebHost::Idle;
	}
}

EMSCRIPTEN_KEEPALIVE int pcsx2_web_stop()
{
	if (!WebHost::s_initialized.load(std::memory_order_acquire))
		return 1;

	Host::RunOnCPUThread([]() {
		if (VMManager::HasValidVM())
			VMManager::SetState(VMState::Stopping);
	});
	return 0;
}

EMSCRIPTEN_KEEPALIVE int pcsx2_web_run_host_tasks()
{
	int count = 0;
	for (;;)
	{
		std::function<void()> task;
		{
			std::lock_guard lock(WebHost::s_host_task_mutex);
			if (WebHost::s_host_tasks.empty())
				return count;
			task = std::move(WebHost::s_host_tasks.front());
			WebHost::s_host_tasks.pop_front();
		}
		task();
		count++;
	}
}

EMSCRIPTEN_KEEPALIVE const void* pcsx2_web_frame_count_address()
{
	return &WebHost::s_frame_count;
}

EMSCRIPTEN_KEEPALIVE int pcsx2_web_tty_read(char* buffer, int buffer_size)
{
	if (!buffer || buffer_size <= 0)
		return 0;

	std::lock_guard lock(WebHost::s_tty_mutex);
	const int count = static_cast<int>(std::min<size_t>(WebHost::s_tty_buffer.size(), static_cast<size_t>(buffer_size)));
	std::memcpy(buffer, WebHost::s_tty_buffer.data(), count);
	WebHost::s_tty_buffer.erase(0, count);
	return count;
}

EMSCRIPTEN_KEEPALIVE int pcsx2_web_tty_pending()
{
	std::lock_guard lock(WebHost::s_tty_mutex);
	return static_cast<int>(WebHost::s_tty_buffer.size());
}

// mask: bit 0 = cpu.jsonl records, bit 1 = audio.jsonl records. ram_every hashes memory every
// that many frames (0 disables). The audio records need the trace audio stream in place before
// the VM opens its output, so enable them before pcsx2_web_boot.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_trace_enable(int mask, int ram_every)
{
	if (mask < 0 || ram_every < 0)
		return 1;

	if ((static_cast<u32>(mask) & WebHost::TraceAudio) && VMManager::HasValidVM())
		return 2;

	AudioStream::SetNullStreamFactory((static_cast<u32>(mask) & WebHost::TraceAudio) ? TraceHash::CreateAudioStream : nullptr);
	WebHost::s_trace_ram_every.store(static_cast<u32>(ram_every), std::memory_order_release);
	WebHost::s_trace_mask.store(static_cast<u32>(mask), std::memory_order_release);
	return 0;
}

// Stops the VM once frames vsyncs have run, so that the traced records and the console output
// end exactly where a tracerunner recording with -frames <frames> ends. 0 removes the limit.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_set_frame_limit(int frames)
{
	if (frames < 0)
		return 1;

	WebHost::s_frame_limit.store(static_cast<u32>(frames), std::memory_order_release);
	return 0;
}

EMSCRIPTEN_KEEPALIVE int pcsx2_web_trace_read(char* buffer, int buffer_size)
{
	if (!buffer || buffer_size <= 0)
		return 0;

	std::lock_guard lock(WebHost::s_trace_mutex);
	const int count = static_cast<int>(std::min<size_t>(WebHost::s_trace_buffer.size(), static_cast<size_t>(buffer_size)));
	std::memcpy(buffer, WebHost::s_trace_buffer.data(), count);
	WebHost::s_trace_buffer.erase(0, count);
	return count;
}

EMSCRIPTEN_KEEPALIVE int pcsx2_web_trace_pending()
{
	std::lock_guard lock(WebHost::s_trace_mutex);
	return static_cast<int>(WebHost::s_trace_buffer.size());
}

// Sets the live state of a pad port; it is applied on the CPU thread at the next vsync.
// buttons packs digital1 (bits 0-7) and digital2 (bits 8-15) with pressed bits set; the analogs
// are 0-255 with 127 centered; pressures is null or twelve bytes in the .p2m2 order (right,
// left, up, down, triangle, circle, cross, square, l1, r1, l2, r2).
EMSCRIPTEN_KEEPALIVE int pcsx2_web_set_pad(int port, int buttons, int lx, int ly, int rx, int ry, const u8* pressures)
{
	if (port < 0 || port >= static_cast<int>(WebHost::PAD_PORTS) || buttons < 0 || buttons > 0xffff ||
		lx < 0 || ly < 0 || rx < 0 || ry < 0)
	{
		return 1;
	}

	const WebHost::PadBytes bytes = WebHost::PadBytesFromState(static_cast<u32>(buttons), static_cast<u32>(lx),
		static_cast<u32>(ly), static_cast<u32>(rx), static_cast<u32>(ry), pressures);
	std::lock_guard lock(WebHost::s_pad_mutex);
	WebHost::s_pad_pending[port] = bytes;
	return 0;
}

// Adds a frame-indexed pad state, applied at the vsync with that g_FrameCount and kept until a
// later entry for the same port. Entries are ordered by frame; equal frames keep insertion order.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_pad_schedule_add(int frame, int port, int buttons, int lx, int ly, int rx, int ry, const u8* pressures)
{
	if (frame < 0 || port < 0 || port >= static_cast<int>(WebHost::PAD_PORTS) || buttons < 0 || buttons > 0xffff ||
		lx < 0 || ly < 0 || rx < 0 || ry < 0)
	{
		return 1;
	}

	WebHost::PadScheduleEntry entry;
	entry.frame = static_cast<u32>(frame);
	entry.port = static_cast<u32>(port);
	entry.bytes = WebHost::PadBytesFromState(static_cast<u32>(buttons), static_cast<u32>(lx), static_cast<u32>(ly),
		static_cast<u32>(rx), static_cast<u32>(ry), pressures);

	std::lock_guard lock(WebHost::s_pad_mutex);
	auto pos = std::upper_bound(WebHost::s_pad_schedule.begin(), WebHost::s_pad_schedule.end(), entry.frame,
		[](u32 value, const WebHost::PadScheduleEntry& other) { return value < other.frame; });
	const size_t index = static_cast<size_t>(pos - WebHost::s_pad_schedule.begin());
	WebHost::s_pad_schedule.insert(pos, std::move(entry));
	if (index < WebHost::s_pad_schedule_next)
		WebHost::s_pad_schedule_next++;
	return 0;
}

EMSCRIPTEN_KEEPALIVE int pcsx2_web_pad_schedule_clear()
{
	std::lock_guard lock(WebHost::s_pad_mutex);
	WebHost::s_pad_schedule.clear();
	WebHost::s_pad_schedule_next = 0;
	return 0;
}

EMSCRIPTEN_KEEPALIVE int pcsx2_web_pad_schedule_size()
{
	std::lock_guard lock(WebHost::s_pad_mutex);
	return static_cast<int>(WebHost::s_pad_schedule.size());
}

// Number of pad states applied to the guest controller, one per port per vsync with a state.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_pad_applied_count()
{
	return static_cast<int>(WebHost::s_pad_applied_count.load(std::memory_order_relaxed));
}

// Starts the audio worklet on the page's AudioContext. The page must have handed its message
// port to the module (Module.pcsx2AudioPort) before this is called; see
// web/host/pcsx2_web_audio_library.js and web/public/pcsx2-web-audio.mjs.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_audio_attach(int sample_rate)
{
	if (sample_rate <= 0)
		return 1;
	return WebAudio::Attach(static_cast<u32>(sample_rate)) ? 0 : 2;
}

// WebAudio::State: -1 failed, 0 detached, 1 starting, 2 ready.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_audio_status()
{
	return static_cast<int>(WebAudio::GetState());
}

// Address of the WebAudio::StatsIndex counters (u32 each).
EMSCRIPTEN_KEEPALIVE const void* pcsx2_web_audio_stats_address()
{
	return WebAudio::StatsAddress();
}

// The canvas the GS presents to, as a CSS selector the page registered with the module's
// GL.offscreenCanvases table (its OffscreenCanvas is transferred to the GS pthread when it starts,
// or used in place when the pump runs on the main thread). An empty selector renders surfaceless.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_set_canvas(const char* selector, int width, int height)
{
	if (MTGS::IsOpen() || width < 0 || height < 0)
		return 1;

	WebHost::s_canvas_selector = selector ? selector : "";
	WebHost::s_canvas_width = static_cast<u32>(width);
	WebHost::s_canvas_height = static_cast<u32>(height);
	MTGS::SetWebCanvasSelector(MTGS::IsWebPumpOnMainThread() ? std::string() : WebHost::s_canvas_selector);
	return 0;
}

// 0: the GS runs on its own pthread (the default), 1: the GS pump runs on the module's main thread
// event loop. Must be set before the first boot.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_set_gs_host(int main_thread)
{
	if (MTGS::IsOpen())
		return 1;

	MTGS::SetWebPumpOnMainThread(main_thread != 0);
	MTGS::SetWebCanvasSelector(main_thread ? std::string() : WebHost::s_canvas_selector);
	return 0;
}

// 0: none, 1: async (frame captures are delivered through pcsx2_web_frame_read).
EMSCRIPTEN_KEEPALIVE int pcsx2_web_set_readback_mode(int mode)
{
	if (mode < 0 || mode > 1)
		return 1;

	WebHost::s_readback_mode.store(mode, std::memory_order_release);
	return 0;
}

// Reads back every Nth presented frame (0 disables) when the readback mode is async.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_set_frame_capture(int every)
{
	if (every < 0)
		return 1;

	WebHost::s_frame_capture_every.store(static_cast<u32>(every), std::memory_order_release);
	return 0;
}

// Also reads back the frame whose oracle number (g_FrameCount + 1 at the vsync, the number in the
// tracerunner's frames/frameNNNNN.png) equals frame. Set before boot.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_add_capture_frame(int frame)
{
	if (frame < 0 || VMManager::HasValidVM())
		return 1;

	WebHost::s_frame_capture_list.push_back(static_cast<u32>(frame));
	return 0;
}

// Number of times a GS dump is played (pcsx2-gsrunner -loop); 0 loops forever.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_set_dump_loop_count(int loops)
{
	if (loops < 0)
		return 1;

	WebHost::s_dump_loop_count.store(loops, std::memory_order_release);
	return 0;
}

// Size in bytes of the RGBA pixels of the oldest captured frame, 0 when none is queued.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_frame_ready()
{
	std::lock_guard lock(WebHost::s_frame_mutex);
	if (WebHost::s_frames.empty())
		return 0;

	return static_cast<int>(WebHost::s_frames.front().pixels.size() * sizeof(u32));
}

// Pops the oldest captured frame. header receives, in order: frame index, width, height, dump frame
// number, dump loop number, oracle frame number, render time in microseconds, pixels changed since
// the previous capture, XXH3 hash low word, XXH3 hash high word. Returns the RGBA bytes copied, 0
// when no frame is queued, -1 when the buffers are too small (the frame stays queued).
EMSCRIPTEN_KEEPALIVE int pcsx2_web_frame_read(u32* header, int header_words, u8* rgba, int rgba_capacity)
{
	static constexpr int HEADER_WORDS = 10;
	if (!header || header_words < HEADER_WORDS || !rgba)
		return -1;

	std::lock_guard lock(WebHost::s_frame_mutex);
	if (WebHost::s_frames.empty())
		return 0;

	WebHost::CapturedFrame& cf = WebHost::s_frames.front();
	const size_t bytes = cf.pixels.size() * sizeof(u32);
	if (bytes > static_cast<size_t>(rgba_capacity))
		return -1;

	header[0] = cf.frame;
	header[1] = cf.width;
	header[2] = cf.height;
	header[3] = cf.dump_frame;
	header[4] = static_cast<u32>(cf.dump_loop);
	header[5] = cf.oracle_frame;
	header[6] = static_cast<u32>(cf.render_ns / 1000);
	header[7] = cf.changed_pixels;
	header[8] = static_cast<u32>(cf.hash);
	header[9] = static_cast<u32>(cf.hash >> 32);
	std::memcpy(rgba, cf.pixels.data(), bytes);
	WebHost::s_frames.pop_front();
	return static_cast<int>(bytes);
}

// GSDevice::GetDriverInfo() of the open device (adapter name and backend), empty before the first present.
EMSCRIPTEN_KEEPALIVE int pcsx2_web_gs_driver_info(char* buffer, int buffer_size)
{
	std::lock_guard lock(WebHost::s_gpu_info_mutex);
	if (!buffer || buffer_size <= 0)
		return static_cast<int>(WebHost::s_gpu_driver_info.size());

	const int count = static_cast<int>(std::min<size_t>(WebHost::s_gpu_driver_info.size(), static_cast<size_t>(buffer_size - 1)));
	std::memcpy(buffer, WebHost::s_gpu_driver_info.data(), count);
	buffer[count] = 0;
	return count;
}

} // extern "C"

//////////////////////////////////////////////////////////////////////////
// Host interface
//////////////////////////////////////////////////////////////////////////

void Host::CommitBaseSettingChanges()
{
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);
}

void Host::OpenURL(const std::string_view url)
{
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return std::string();
}

void Host::BeginTextInput()
{
}

void Host::EndTextInput()
{
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	wi.surface_width = 640;
	wi.surface_height = 480;
	wi.surface_scale = 1.0f;
	return wi;
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	if (WebHost::s_canvas_selector.empty())
		return GetTopLevelWindowInfo();

	WindowInfo wi;
	wi.type = WindowInfo::Type::WebCanvas;
	wi.window_handle = const_cast<char*>(WebHost::s_canvas_selector.c_str());
	wi.surface_width = WebHost::s_canvas_width;
	wi.surface_height = WebHost::s_canvas_height;
	wi.surface_scale = 1.0f;
	return wi;
}

void Host::ReleaseRenderWindow()
{
}

void WebHost::CaptureCompleted(u32 frame, u32 dump_frame, s32 dump_loop, u32 oracle_frame, u64 render_ns, u32 width, u32 height, std::vector<u32> pixels)
{
	// GS thread (event loop). An empty image records a failed readback.
	u32 changed = width * height;
	if (!pixels.empty() && width == s_last_capture_width && height == s_last_capture_height)
	{
		changed = 0;
		for (size_t i = 0; i < pixels.size(); i++)
			changed += (pixels[i] != s_last_capture[i]);
	}
	if (!pixels.empty())
	{
		s_last_capture = pixels;
		s_last_capture_width = width;
		s_last_capture_height = height;
	}

	const u64 hash = pixels.empty() ? 0 : GSXXH3_64bits(pixels.data(), pixels.size() * sizeof(u32));

	std::lock_guard lock(s_frame_mutex);
	if (s_frames.size() >= MAX_QUEUED_FRAMES)
		s_frames.pop_front();
	s_frames.push_back(CapturedFrame{frame, width, height, dump_frame, dump_loop, oracle_frame, render_ns, changed, hash, std::move(pixels)});
}

void Host::BeginPresentFrame()
{
	const u32 frame = WebHost::s_frame_count.fetch_add(1, std::memory_order_release);

	const u64 now = GetCPUTicks();
	const u64 render_ns = (WebHost::s_last_present_ticks != 0) ? (now - WebHost::s_last_present_ticks) : 0;
	WebHost::s_last_present_ticks = now;

	if (frame == 0 && g_gs_device)
	{
		std::lock_guard lock(WebHost::s_gpu_info_mutex);
		WebHost::s_gpu_driver_info = g_gs_device->GetDriverInfo();
	}

	const u32 oracle_frame = WebHost::s_oracle_frame_number;
	const u32 every = WebHost::s_frame_capture_every.load(std::memory_order_acquire);
	const bool listed = std::find(WebHost::s_frame_capture_list.begin(), WebHost::s_frame_capture_list.end(), oracle_frame) != WebHost::s_frame_capture_list.end();
	if ((every == 0 || (frame % every) != 0) && !listed)
		return;
	if (WebHost::s_readback_mode.load(std::memory_order_acquire) != 1)
		return;
	if (!g_gs_renderer || !GSIsHardwareRenderer() || !g_gs_device->GetCurrent())
		return;

	// Internal resolution, aspect corrected and cropped: what a surfaceless pcsx2-gsrunner or
	// tracerunner writes for its PNGs, so the pixels compare against them.
	const u32 dump_frame = WebHost::s_dump_frame_number;
	const s32 dump_loop = WebHost::s_dump_loop_number;
	g_gs_renderer->SaveSnapshotToMemoryAsync(0, 0, true, true,
		[frame, dump_frame, dump_loop, oracle_frame, render_ns](u32 width, u32 height, std::vector<u32> pixels) {
			WebHost::CaptureCompleted(frame, dump_frame, dump_loop, oracle_frame, render_ns, width, height, std::move(pixels));
		});
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
	// A new VM counts frames from zero again, so the schedule replays from its start.
	std::lock_guard lock(WebHost::s_pad_mutex);
	WebHost::s_pad_schedule_next = 0;
}

void Host::OnVMStarted()
{
}

void Host::OnVSyncTrace()
{
	const u32 frame = g_FrameCount;
	const u32 mask = WebHost::s_trace_mask.load(std::memory_order_acquire);
	if (mask != 0)
	{
		std::string records;
		if (mask & WebHost::TraceCPU)
			records += TraceHash::FormatCPURecord(frame, WebHost::s_trace_ram_every.load(std::memory_order_acquire));
		if (mask & WebHost::TraceAudio)
			records += WebAudio::HasActiveStream() ? WebAudio::FormatAudioRecord(frame) : TraceHash::FormatAudioRecord(frame);

		if (!records.empty())
		{
			std::lock_guard lock(WebHost::s_trace_mutex);
			WebHost::s_trace_buffer += records;
		}
	}

	const u32 frame_limit = WebHost::s_frame_limit.load(std::memory_order_acquire);
	if (frame_limit > 0 && (frame + 1) >= frame_limit && VMManager::GetState() == VMState::Running)
		VMManager::SetState(VMState::Stopping);
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	if (block)
	{
		std::mutex done_mutex;
		std::condition_variable done_cv;
		bool done = false;
		{
			std::lock_guard lock(WebHost::s_cpu_task_mutex);
			WebHost::s_cpu_tasks.push_back([&]() {
				function();
				std::lock_guard done_lock(done_mutex);
				done = true;
				done_cv.notify_one();
			});
		}
		WebHost::s_cpu_task_sema.Post();
		std::unique_lock lock(done_mutex);
		done_cv.wait(lock, [&]() { return done; });
		return;
	}

	{
		std::lock_guard lock(WebHost::s_cpu_task_mutex);
		WebHost::s_cpu_tasks.push_back(std::move(function));
	}
	WebHost::s_cpu_task_sema.Post();
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

void Host::OnAchievementsRefreshed()
{
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return true;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

void Host::PumpMessagesOnCPUThread()
{
	WebHost::ProcessCPUThreadTasks();
	WebHost::ApplyPadOnCPUThread();

	// Update the GS thread's copies of the frame numbers the native runners post the same way, so
	// the captured frames carry the numbers their PNGs are named with: pcsx2-gsrunner's dump
	// position, pcsx2-tracerunner's g_FrameCount + 1.
	if (!MTGS::IsOpen())
		return;

	if (GSDumpReplayer::IsReplayingDump())
	{
		MTGS::RunOnGSThread([frame_number = GSDumpReplayer::GetFrameNumber()]() { WebHost::s_dump_frame_number = frame_number; });
		MTGS::RunOnGSThread([loop_number = GSDumpReplayer::GetLoopCount()]() { WebHost::s_dump_loop_number = loop_number; });
	}
	else
	{
		MTGS::RunOnGSThread([frame_number = g_FrameCount + 1]() { WebHost::s_oracle_frame_number = frame_number; });
	}
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()
