// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#include "fmt/format.h"

#include "common/Assertions.h"
#include "common/CocoaTools.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include <xxhash.h>

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/Counters.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/Host/AudioStream.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/Recording/InputRecording.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SPU2/defs.h"
#include "pcsx2/TraceHash.h"
#include "pcsx2/VMManager.h"

#include "svnrev.h"

// Down here because X11 has a lot of defines that can conflict
#if defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/select.h>
#include <unistd.h>
#endif

namespace TraceRunner
{
	static void InitializeConsole();
	static bool InitializeConfig();
	static void SettingsOverride();
	static void ApplyUserSettings();
	static bool ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params);
	static bool OpenTraceFiles();
	static void CloseTraceFiles();
	static bool WriteManifest();
	static bool StartInputPlayback();
	static void LogCallback(LOGLEVEL level, ConsoleColors color, std::string_view message);

	static bool CreatePlatformWindow();
	static void DestroyPlatformWindow();
	static std::optional<WindowInfo> GetPlatformWindowInfo();
	static void PumpPlatformMessages(bool forever = false);
	static void StopPlatformMessagePump();
} // namespace TraceRunner

static constexpr u32 WINDOW_WIDTH = 640;
static constexpr u32 WINDOW_HEIGHT = 480;

static MemorySettingsInterface s_settings_interface;

static std::vector<std::pair<std::string, std::string>> s_setting_overrides;
static std::vector<std::pair<std::string, std::string>> s_user_settings;
static std::string s_trace_dir;
static std::string s_bios_dir;
static std::string s_input_path;
static std::string s_image_path;
static std::string s_renderer_name = "Auto";
static bool s_cpu_recompiler = false;
static u32 s_frame_limit = 0;
static u32 s_trace_ram_every = 0;
static u32 s_png_every = 0;
static std::map<u32, u32> s_gsdump_frames;
static bool s_audio_hash = false;
static std::optional<bool> s_use_window;
static bool s_no_console = false;

static std::FILE* s_cpu_file = nullptr;
static std::FILE* s_tty_file = nullptr;
static std::FILE* s_audio_file = nullptr;
static std::mutex s_tty_mutex;

// Owned by the GS thread.
static u32 s_gs_frame_number = 0;

static std::mutex s_device_info_mutex;
static std::string s_device_renderer;
static std::string s_device_adapter;
static std::string s_device_driver;

static void RecordSetting(const char* section, const char* key, std::string value)
{
	const std::string name = fmt::format("{}/{}", section, key);
	for (auto& it : s_setting_overrides)
	{
		if (it.first == name)
		{
			it.second = std::move(value);
			return;
		}
	}

	s_setting_overrides.emplace_back(name, std::move(value));
}

static void SetBoolSetting(const char* section, const char* key, bool value)
{
	s_settings_interface.SetBoolValue(section, key, value);
	RecordSetting(section, key, value ? "true" : "false");
}

static void SetIntSetting(const char* section, const char* key, s32 value)
{
	s_settings_interface.SetIntValue(section, key, value);
	RecordSetting(section, key, fmt::format("{}", value));
}

static void SetStringSetting(const char* section, const char* key, const char* value)
{
	s_settings_interface.SetStringValue(section, key, value);
	RecordSetting(section, key, value);
}

static std::string JsonEscape(std::string_view str)
{
	std::string ret;
	ret.reserve(str.size() + 2);
	for (const char ch : str)
	{
		switch (ch)
		{
			case '"':
				ret += "\\\"";
				break;
			case '\\':
				ret += "\\\\";
				break;
			case '\n':
				ret += "\\n";
				break;
			case '\r':
				ret += "\\r";
				break;
			case '\t':
				ret += "\\t";
				break;
			default:
				if (static_cast<unsigned char>(ch) < 0x20)
					ret += fmt::format("\\u{:04x}", static_cast<unsigned char>(ch));
				else
					ret += ch;
				break;
		}
	}
	return ret;
}

static std::string JsonString(std::string_view str)
{
	return fmt::format("\"{}\"", JsonEscape(str));
}

static std::optional<u64> HashFile(const std::string& path)
{
	auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
	if (!fp)
		return std::nullopt;

	XXH3_state_t state;
	XXH3_64bits_reset(&state);

	std::vector<u8> buffer(1024 * 1024);
	for (;;)
	{
		const size_t read = std::fread(buffer.data(), 1, buffer.size(), fp.get());
		if (read == 0)
			break;

		XXH3_64bits_update(&state, buffer.data(), read);
	}

	if (std::ferror(fp.get()))
		return std::nullopt;

	return XXH3_64bits_digest(&state);
}

bool TraceRunner::InitializeConfig()
{
	EmuFolders::SetAppRoot();
	if (!EmuFolders::SetResourcesDirectory() || !EmuFolders::SetDataDirectory(nullptr))
		return false;

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	const char* error;
	if (!VMManager::PerformEarlyHardwareChecks(&error))
		return false;

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

	// don't provide an ini path, or bother loading. we'll store everything in memory.
	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);

	VMManager::SetDefaultSettings(si, true, true, true, true, true);

	VMManager::Internal::LoadStartupSettings();
	return true;
}

void Host::CommitBaseSettingChanges()
{
	// nothing to save, we're all in memory
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	// not running any UI, so no settings requests will come in
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	// nothing
}

bool Host::LocaleCircleConfirm()
{
	// not running any UI, so no settings requests will come in
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
	// noop
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
	// noop
}

void Host::EndTextInput()
{
	// noop
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return TraceRunner::GetPlatformWindowInfo();
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
	return TraceRunner::GetPlatformWindowInfo();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
	const u32 frame = s_gs_frame_number;

	if (!s_trace_dir.empty())
	{
		const auto dump = s_gsdump_frames.find(frame);
		if (dump != s_gsdump_frames.end())
		{
			GSQueueSnapshot(Path::Combine(s_trace_dir, fmt::format("dumps" FS_OSPATH_SEPARATOR_STR "frame{:05}.png", frame)),
				dump->second);
		}
		else if (s_png_every > 0 && (frame % s_png_every) == 0)
		{
			GSQueueSnapshot(Path::Combine(s_trace_dir, fmt::format("frames" FS_OSPATH_SEPARATOR_STR "frame{:05}.png", frame)), 0);
		}
	}

	std::unique_lock lock(s_device_info_mutex);
	if (s_device_renderer.empty())
	{
		s_device_renderer = Pcsx2Config::GSOptions::GetRendererName(GSGetCurrentRenderer());
		if (g_gs_device)
		{
			s_device_adapter = g_gs_device->GetName();
			s_device_driver = g_gs_device->GetDriverInfo();
		}
	}
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVSyncTrace()
{
	const u32 frame = g_FrameCount;

	if (s_cpu_file)
		std::fputs(TraceHash::FormatCPURecord(frame, s_trace_ram_every).c_str(), s_cpu_file);

	if (s_audio_file)
	{
		const std::string line = TraceHash::FormatAudioRecord(frame);
		if (!line.empty())
			std::fputs(line.c_str(), s_audio_file);
	}

	if (s_frame_limit > 0 && (frame + 1) >= s_frame_limit && VMManager::GetState() == VMState::Running)
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
	pxFailRel("Not implemented");
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
	// noop
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
	// noop
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
	// noop
}

void Host::OnAchievementsRefreshed()
{
	// noop
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
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

static void PrintCommandLineVersion()
{
	std::fprintf(stderr, "PCSX2 Trace Runner Version %s\n", GIT_REV);
	std::fprintf(stderr, "https://pcsx2.net/\n");
	std::fprintf(stderr, "\n");
}

static void PrintCommandLineHelp(const char* progname)
{
	PrintCommandLineVersion();
	std::fprintf(stderr, "Usage: %s [parameters] [--] <game.iso|program.elf>\n", progname);
	std::fprintf(stderr, "\n");
	std::fprintf(stderr, "  -help: Displays this information and exits.\n");
	std::fprintf(stderr, "  -version: Displays version information and exits.\n");
	std::fprintf(stderr, "  -bios <dir>: Directory containing the BIOS image.\n");
	std::fprintf(stderr, "  -renderer <sw|vulkan|null>: Sets the graphics renderer. Defaults to Auto.\n");
	std::fprintf(stderr, "  -nulldevice: Runs the software renderer on the null GS device (no GPU, nothing is presented).\n");
	std::fprintf(stderr, "  -cpu <interpreter|recompiler>: Sets the EE/IOP/VU execution mode. Defaults to interpreter.\n");
	std::fprintf(stderr, "  -frames <count>: Stops after this many vsyncs.\n");
	std::fprintf(stderr, "  -input <file.p2m2>: Replays an input recording from power on.\n");
	std::fprintf(stderr, "  -trace <dir>: Trace output directory (cpu.jsonl, tty.txt, audio.jsonl, frames/, dumps/, manifest.json).\n");
	std::fprintf(stderr, "  -trace-ram-every <K>: Hashes EE/IOP/VU/SPU2 memory every K frames. 0 disables.\n");
	std::fprintf(stderr, "  -gsdump-at <f1,f2:len,...>: Saves a GS dump at frame f for len frames (default 1).\n");
	std::fprintf(stderr, "  -png-every <K>: Saves a PNG of the presented frame every K frames.\n");
	std::fprintf(stderr, "  -audio-hash: Hashes the SPU2 output per vsync.\n");
	std::fprintf(stderr, "  -setting <Section/Key=Value>: Overrides a setting. Applied after all fixed overrides.\n");
	std::fprintf(stderr, "  -window: Forces a window to be displayed.\n");
	std::fprintf(stderr, "  -surfaceless: Disables showing a window.\n");
	std::fprintf(stderr, "  -logfile <filename>: Writes emu log to filename.\n");
	std::fprintf(stderr, "  -noshadercache: Disables the shader cache (useful for parallel runs).\n");
	std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
						 "    parameters make up the filename. Use when the filename contains\n"
						 "    spaces or starts with a dash.\n");
	std::fprintf(stderr, "\n");
}

void TraceRunner::InitializeConsole()
{
	const char* var = std::getenv("PCSX2_NOCONSOLE");
	s_no_console = (var && StringUtil::FromChars<bool>(var).value_or(false));
	if (!s_no_console)
		Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);
}

static std::string MakeAbsolutePath(std::string_view path)
{
	if (Path::IsAbsolute(path))
		return std::string(path);

	return Path::Combine(FileSystem::GetWorkingDirectory(), path);
}

static bool ParseGSDumpFrames(std::string_view str)
{
	for (const std::string_view& entry : StringUtil::SplitString(str, ','))
	{
		const std::string_view::size_type colon = entry.find(':');
		const std::optional<u32> frame = StringUtil::FromChars<u32>(entry.substr(0, colon));
		std::optional<u32> length = 1;
		if (colon != std::string_view::npos)
			length = StringUtil::FromChars<u32>(entry.substr(colon + 1));

		if (!frame.has_value() || !length.has_value() || length.value() == 0)
		{
			Console.ErrorFmt("Invalid GS dump frame specification '{}'.", entry);
			return false;
		}

		s_gsdump_frames[frame.value()] = length.value();
	}

	return true;
}

bool TraceRunner::ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params)
{
	bool no_more_args = false;
	std::string filename;
	for (int i = 1; i < argc; i++)
	{
		if (!no_more_args)
		{
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

			if (CHECK_ARG("-help"))
			{
				PrintCommandLineHelp(argv[0]);
				return false;
			}
			else if (CHECK_ARG("-version"))
			{
				PrintCommandLineVersion();
				return false;
			}
			else if (CHECK_ARG_PARAM("-bios"))
			{
				s_bios_dir = MakeAbsolutePath(StringUtil::StripWhitespace(argv[++i]));
				if (!FileSystem::DirectoryExists(s_bios_dir.c_str()))
				{
					Console.ErrorFmt("BIOS directory '{}' does not exist.", s_bios_dir);
					return false;
				}

				SetStringSetting("Folders", "Bios", s_bios_dir.c_str());
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderer"))
			{
				const char* rname = argv[++i];

				GSRendererType type = GSRendererType::Auto;
				if (StringUtil::Strcasecmp(rname, "sw") == 0)
					type = GSRendererType::SW;
#ifdef ENABLE_VULKAN
				else if (StringUtil::Strcasecmp(rname, "vulkan") == 0)
					type = GSRendererType::VK;
#endif
				else if (StringUtil::Strcasecmp(rname, "null") == 0)
					type = GSRendererType::Null;
				else
				{
					Console.Error("Unknown renderer '%s'", rname);
					return false;
				}

				s_renderer_name = Pcsx2Config::GSOptions::GetRendererName(type);
				Console.WriteLn("Using %s renderer.", s_renderer_name.c_str());
				SetIntSetting("EmuCore/GS", "Renderer", static_cast<int>(type));
				continue;
			}
			else if (CHECK_ARG("-nulldevice"))
			{
				Console.WriteLn("Using null GS device.");
				SetBoolSetting("EmuCore/GS", "NullDevice", true);
				continue;
			}
			else if (CHECK_ARG_PARAM("-cpu"))
			{
				const char* mode = argv[++i];
				if (StringUtil::Strcasecmp(mode, "interpreter") == 0)
					s_cpu_recompiler = false;
				else if (StringUtil::Strcasecmp(mode, "recompiler") == 0)
					s_cpu_recompiler = true;
				else
				{
					Console.Error("Unknown CPU mode '%s'", mode);
					return false;
				}

				continue;
			}
			else if (CHECK_ARG_PARAM("-frames"))
			{
				s_frame_limit = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
				if (s_frame_limit == 0)
				{
					Console.Error("Invalid frame count.");
					return false;
				}

				continue;
			}
			else if (CHECK_ARG_PARAM("-input"))
			{
				s_input_path = MakeAbsolutePath(StringUtil::StripWhitespace(argv[++i]));
				if (!FileSystem::FileExists(s_input_path.c_str()))
				{
					Console.ErrorFmt("Input recording '{}' does not exist.", s_input_path);
					return false;
				}

				continue;
			}
			else if (CHECK_ARG_PARAM("-trace"))
			{
				s_trace_dir = MakeAbsolutePath(StringUtil::StripWhitespace(argv[++i]));
				if (s_trace_dir.empty())
				{
					Console.Error("Invalid trace directory specified.");
					return false;
				}

				continue;
			}
			else if (CHECK_ARG_PARAM("-trace-ram-every"))
			{
				const std::optional<u32> value = StringUtil::FromChars<u32>(argv[++i]);
				if (!value.has_value())
				{
					Console.Error("Invalid RAM hash interval.");
					return false;
				}

				s_trace_ram_every = value.value();
				continue;
			}
			else if (CHECK_ARG_PARAM("-gsdump-at"))
			{
				if (!ParseGSDumpFrames(argv[++i]))
					return false;

				continue;
			}
			else if (CHECK_ARG_PARAM("-png-every"))
			{
				const std::optional<u32> value = StringUtil::FromChars<u32>(argv[++i]);
				if (!value.has_value())
				{
					Console.Error("Invalid PNG interval.");
					return false;
				}

				s_png_every = value.value();
				continue;
			}
			else if (CHECK_ARG("-audio-hash"))
			{
				s_audio_hash = true;
				continue;
			}
			else if (CHECK_ARG_PARAM("-setting"))
			{
				const std::string_view arg(argv[++i]);
				const std::string_view::size_type equals = arg.find('=');
				const std::string_view::size_type slash = arg.rfind('/', equals);
				if (equals == std::string_view::npos || slash == std::string_view::npos || slash == 0 || (slash + 1) == equals)
				{
					Console.ErrorFmt("Invalid setting '{}', expected Section/Key=Value.", arg);
					return false;
				}

				s_user_settings.emplace_back(std::string(arg.substr(0, equals)), std::string(arg.substr(equals + 1)));
				continue;
			}
			else if (CHECK_ARG_PARAM("-logfile"))
			{
				const char* logfile = argv[++i];
				if (std::strlen(logfile) > 0)
				{
					// disable timestamps, since we want to be able to diff the logs
					Console.WriteLn("Logging to %s...", logfile);
					VMManager::Internal::SetFileLogPath(logfile);
					SetBoolSetting("Logging", "EnableFileLogging", true);
				}

				continue;
			}
			else if (CHECK_ARG("-noshadercache"))
			{
				Console.WriteLn("Disabling shader cache");
				SetBoolSetting("EmuCore/GS", "DisableShaderCache", true);
				continue;
			}
			else if (CHECK_ARG("-window"))
			{
				Console.WriteLn("Creating window");
				s_use_window = true;
				continue;
			}
			else if (CHECK_ARG("-surfaceless"))
			{
				Console.WriteLn("Running surfaceless");
				s_use_window = false;
				continue;
			}
			else if (CHECK_ARG("--"))
			{
				no_more_args = true;
				continue;
			}
			else if (argv[i][0] == '-')
			{
				Console.Error("Unknown parameter: '%s'", argv[i]);
				return false;
			}

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
		}

		if (!filename.empty())
			filename += ' ';
		filename += argv[i];
	}

	if (filename.empty())
	{
		Console.Error("No ELF or disc image provided.");
		return false;
	}

	s_image_path = MakeAbsolutePath(filename);
	if (!FileSystem::FileExists(s_image_path.c_str()))
	{
		Console.ErrorFmt("'{}' does not exist.", s_image_path);
		return false;
	}

	if (VMManager::IsElfFileName(s_image_path))
		params.elf_override = s_image_path;
	else
		params.filename = s_image_path;

	params.disable_achievements_hardcore_mode = true;

	if (s_trace_dir.empty())
	{
		Console.Error("No trace directory provided.");
		return false;
	}

	for (const std::string& path : {s_trace_dir, Path::Combine(s_trace_dir, "frames"), Path::Combine(s_trace_dir, "dumps")})
	{
		if (!FileSystem::DirectoryExists(path.c_str()) && !FileSystem::CreateDirectoryPath(path.c_str(), true))
		{
			Console.ErrorFmt("Failed to create trace directory '{}'.", path);
			return false;
		}
	}

	return true;
}

void TraceRunner::SettingsOverride()
{
	SetBoolSetting("EmuCore/GS", "FrameLimitEnable", false);
	SetIntSetting("EmuCore/GS", "VsyncEnable", 0);
	SetIntSetting("EmuCore/GS", "ScreenshotFormat", static_cast<int>(GSScreenshotFormat::PNG));
	SetIntSetting("EmuCore/GS", "ScreenshotQuality", 10);

	SetBoolSetting("EmuCore/Speedhacks", "vuThread", false);

	SetBoolSetting("EmuCore/CPU/Recompiler", "EnableEE", s_cpu_recompiler);
	SetBoolSetting("EmuCore/CPU/Recompiler", "EnableIOP", s_cpu_recompiler);
	SetBoolSetting("EmuCore/CPU/Recompiler", "EnableVU0", s_cpu_recompiler);
	SetBoolSetting("EmuCore/CPU/Recompiler", "EnableVU1", s_cpu_recompiler);
	SetBoolSetting("EmuCore/CPU/Recompiler", "EnableEECache", false);

	SetBoolSetting("EmuCore", "EnableRecordingTools", true);
	SetBoolSetting("EmuCore", "SaveStateOnShutdown", false);
	SetBoolSetting("EmuCore", "BackupSavestate", false);
	SetBoolSetting("EmuCore", "HostFs", false);
	SetBoolSetting("EmuCore", "EnableCheats", false);
	SetBoolSetting("EmuCore", "EnableWideScreenPatches", false);
	SetBoolSetting("EmuCore", "EnableNoInterlacingPatches", false);

	SetBoolSetting("Achievements", "Enabled", false);

	SetStringSetting("SPU2/Output", "Backend", "Null");
	SetStringSetting("SPU2/Output", "SyncMode", "Disabled");
	SetIntSetting("SPU2/Output", "StandardVolume", 100);
	SetIntSetting("SPU2/Output", "FastForwardVolume", 100);
	SetBoolSetting("SPU2/Output", "OutputMuted", false);

	// ensure all input sources are disabled, we're not using them
	SetBoolSetting("InputSources", "SDL", false);
	SetBoolSetting("InputSources", "XInput", false);

	// none of the bindings are going to resolve to anything
	Pad::ClearPortBindings(s_settings_interface, 0);
	s_settings_interface.ClearSection("Hotkeys");

	SetBoolSetting("Logging", "EnableSystemConsole", !s_no_console);
	SetBoolSetting("Logging", "EnableTimestamps", false);
	SetBoolSetting("Logging", "EnableVerbose", true);
	SetBoolSetting("Logging", "EnableEEConsole", true);
	SetBoolSetting("Logging", "EnableIOPConsole", true);

	// remove memory cards, so we don't have sharing violations
	for (u32 i = 0; i < 2; i++)
	{
		SetBoolSetting("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);
		SetStringSetting("MemoryCards", fmt::format("Slot{}_Filename", i + 1).c_str(), "");
	}
}

void TraceRunner::ApplyUserSettings()
{
	for (const auto& [name, value] : s_user_settings)
	{
		const std::string::size_type slash = name.rfind('/');
		const std::string section = name.substr(0, slash);
		const std::string key = name.substr(slash + 1);
		SetStringSetting(section.c_str(), key.c_str(), value.c_str());
	}
}

void TraceRunner::LogCallback(LOGLEVEL level, ConsoleColors color, std::string_view message)
{
	const char* prefix;
	if (color == Color_Cyan)
		prefix = "EE: ";
	else if (color == Color_Yellow)
		prefix = "IOP: ";
	else
		return;

	std::unique_lock lock(s_tty_mutex);
	if (!s_tty_file)
		return;

	std::fputs(prefix, s_tty_file);
	std::fwrite(message.data(), 1, message.size(), s_tty_file);
	std::fputc('\n', s_tty_file);
}

bool TraceRunner::OpenTraceFiles()
{
	const std::string cpu_path = Path::Combine(s_trace_dir, "cpu.jsonl");
	s_cpu_file = FileSystem::OpenCFile(cpu_path.c_str(), "wb");
	if (!s_cpu_file)
	{
		Console.ErrorFmt("Failed to open '{}'.", cpu_path);
		return false;
	}

	const std::string tty_path = Path::Combine(s_trace_dir, "tty.txt");
	{
		std::unique_lock lock(s_tty_mutex);
		s_tty_file = FileSystem::OpenCFile(tty_path.c_str(), "wb");
	}
	if (!s_tty_file)
	{
		Console.ErrorFmt("Failed to open '{}'.", tty_path);
		return false;
	}

	Log::SetHostOutputLevel(LOGLEVEL_INFO, LogCallback);

	if (s_audio_hash)
	{
		const std::string audio_path = Path::Combine(s_trace_dir, "audio.jsonl");
		s_audio_file = FileSystem::OpenCFile(audio_path.c_str(), "wb");
		if (!s_audio_file)
		{
			Console.ErrorFmt("Failed to open '{}'.", audio_path);
			return false;
		}
	}

	return true;
}

void TraceRunner::CloseTraceFiles()
{
	Log::SetHostOutputLevel(LOGLEVEL_NONE, nullptr);

	if (s_cpu_file)
	{
		std::fclose(s_cpu_file);
		s_cpu_file = nullptr;
	}

	{
		std::unique_lock lock(s_tty_mutex);
		if (s_tty_file)
		{
			std::fclose(s_tty_file);
			s_tty_file = nullptr;
		}
	}

	if (s_audio_file)
	{
		std::fclose(s_audio_file);
		s_audio_file = nullptr;
	}
}

bool TraceRunner::WriteManifest()
{
	const std::optional<u64> image_hash = HashFile(s_image_path);
	if (!image_hash.has_value())
	{
		Console.ErrorFmt("Failed to hash '{}'.", s_image_path);
		return false;
	}

	std::string renderer = s_renderer_name;
	std::string adapter;
	std::string driver;
	{
		std::unique_lock lock(s_device_info_mutex);
		if (!s_device_renderer.empty())
			renderer = s_device_renderer;
		adapter = s_device_adapter;
		driver = s_device_driver;
	}

	std::string json = "{\n";
	json += fmt::format("  \"renderer\": {},\n", JsonString(renderer));
	json += fmt::format("  \"cpu\": {},\n", JsonString(s_cpu_recompiler ? "recompiler" : "interpreter"));
	json += fmt::format("  \"frames\": {},\n", s_frame_limit);
	json += fmt::format("  \"trace_ram_every\": {},\n", s_trace_ram_every);
	json += fmt::format("  \"png_every\": {},\n", s_png_every);
	json += fmt::format("  \"audio_hash\": {},\n", s_audio_hash ? "true" : "false");
	json += "  \"gsdump_at\": [";
	bool first = true;
	for (const auto& [frame, length] : s_gsdump_frames)
	{
		json += fmt::format("{}{{\"frame\": {}, \"length\": {}}}", first ? "" : ", ", frame, length);
		first = false;
	}
	json += "],\n";
	json += fmt::format("  \"input\": {},\n", s_input_path.empty() ? "null" : JsonString(s_input_path));
	json += fmt::format("  \"image\": {{\"path\": {}, \"elf\": {}, \"xxh3\": {}}},\n", JsonString(s_image_path),
		VMManager::IsElfFileName(s_image_path) ? "true" : "false", JsonString(TraceHash::HashToString(image_hash.value())));
	json += fmt::format("  \"bios\": {},\n", s_bios_dir.empty() ? "null" : JsonString(s_bios_dir));
	json += fmt::format("  \"pcsx2\": {{\"rev\": {}, \"hash\": {}}},\n", JsonString(GIT_REV), JsonString(GIT_HASH));
	json += fmt::format("  \"adapter\": {},\n", JsonString(adapter));
	json += fmt::format("  \"driver\": {},\n", JsonString(driver));
	json += "  \"settings\": {\n";
	first = true;
	for (const auto& [name, value] : s_setting_overrides)
	{
		json += fmt::format("{}    {}: {}", first ? "" : ",\n", JsonString(name), JsonString(value));
		first = false;
	}
	json += "\n  }\n}\n";

	const std::string path = Path::Combine(s_trace_dir, "manifest.json");
	if (!FileSystem::WriteStringToFile(path.c_str(), json))
	{
		Console.ErrorFmt("Failed to write '{}'.", path);
		return false;
	}

	return true;
}

bool TraceRunner::StartInputPlayback()
{
	if (s_input_path.empty())
		return true;

	const std::string copy_path = Path::Combine(s_trace_dir, "input.p2m2");
	if (!FileSystem::CopyFilePath(s_input_path.c_str(), copy_path.c_str(), true))
	{
		Console.ErrorFmt("Failed to copy input recording to '{}'.", copy_path);
		return false;
	}

	if (!g_InputRecording.play(copy_path))
	{
		Console.ErrorFmt("Failed to start input recording '{}'.", copy_path);
		return false;
	}

	return true;
}

#ifdef _WIN32
// We can't handle unicode in filenames if we don't use wmain on Win32.
#define main real_main
#endif

static void CPUThreadMain(VMBootParameters* params, std::atomic<int>* ret)
{
	ret->store(EXIT_FAILURE);

	if (VMManager::Internal::CPUThreadInitialize())
	{
		// apply new settings (e.g. pick up renderer change)
		VMManager::ApplySettings();

		if (VMManager::Initialize(*params) == VMBootResult::StartupSuccess)
		{
			if (TraceRunner::StartInputPlayback() && TraceRunner::OpenTraceFiles() && TraceRunner::WriteManifest())
			{
				VMManager::SetState(VMState::Running);
				while (VMManager::GetState() == VMState::Running)
					VMManager::Execute();
				ret->store(EXIT_SUCCESS);
			}

			VMManager::Shutdown(false);
			TraceRunner::CloseTraceFiles();
			if (ret->load() == EXIT_SUCCESS && !TraceRunner::WriteManifest())
				ret->store(EXIT_FAILURE);
		}
	}

	VMManager::Internal::CPUThreadShutdown();
	TraceRunner::StopPlatformMessagePump();
}

int main(int argc, char* argv[])
{
	CrashHandler::Install();
	TraceRunner::InitializeConsole();

	if (!TraceRunner::InitializeConfig())
	{
		Console.Error("Failed to initialize config.");
		return EXIT_FAILURE;
	}

	VMBootParameters params;
	if (!TraceRunner::ParseCommandLineArgs(argc, argv, params))
		return EXIT_FAILURE;

	if (s_use_window.value_or(true) && !TraceRunner::CreatePlatformWindow())
	{
		Console.Error("Failed to create window.");
		return EXIT_FAILURE;
	}

	// Override settings that shouldn't be picked up from defaults or INIs.
	TraceRunner::SettingsOverride();
	TraceRunner::ApplyUserSettings();

	if (s_audio_hash)
		AudioStream::SetNullStreamFactory(TraceHash::CreateAudioStream);

	std::atomic<int> thread_ret;
	std::thread cputhread(CPUThreadMain, &params, &thread_ret);
	TraceRunner::PumpPlatformMessages(/*forever=*/true);
	cputhread.join();

	TraceRunner::DestroyPlatformWindow();

	return thread_ret.load();
}

void Host::PumpMessagesOnCPUThread()
{
	// update GS thread copy of frame number
	MTGS::RunOnGSThread([frame_number = g_FrameCount + 1]() { s_gs_frame_number = frame_number; });
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

//////////////////////////////////////////////////////////////////////////
// Platform specific code
//////////////////////////////////////////////////////////////////////////

#ifdef _WIN32

static constexpr LPCWSTR WINDOW_CLASS_NAME = L"PCSX2TraceRunner";
static HWND s_hwnd = NULL;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool TraceRunner::CreatePlatformWindow()
{
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(WNDCLASSEXW);
	wc.style = 0;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = GetModuleHandle(nullptr);
	wc.hIcon = NULL;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszMenuName = NULL;
	wc.lpszClassName = WINDOW_CLASS_NAME;
	wc.hIconSm = NULL;

	if (!RegisterClassExW(&wc))
	{
		Console.Error("Window registration failed.");
		return false;
	}

	s_hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, L"PCSX2 Trace Runner",
		WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_MINIMIZEBOX | WS_SYSMENU | WS_SIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_WIDTH,
		WINDOW_HEIGHT, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!s_hwnd)
	{
		Console.Error("CreateWindowEx failed.");
		return false;
	}

	ShowWindow(s_hwnd, SW_SHOW);
	UpdateWindow(s_hwnd);

	// make sure all messages are processed before returning
	PumpPlatformMessages();
	return true;
}

void TraceRunner::DestroyPlatformWindow()
{
	if (!s_hwnd)
		return;

	PumpPlatformMessages();
	DestroyWindow(s_hwnd);
	s_hwnd = {};
}

std::optional<WindowInfo> TraceRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;

	if (s_hwnd)
	{
		RECT rc = {};
		GetWindowRect(s_hwnd, &rc);
		wi.surface_width = static_cast<u32>(rc.right - rc.left);
		wi.surface_height = static_cast<u32>(rc.bottom - rc.top);
		wi.surface_scale = 1.0f;
		wi.type = WindowInfo::Type::Win32;
		wi.window_handle = s_hwnd;
	}
	else
	{
		wi.type = WindowInfo::Type::Surfaceless;
	}

	return wi;
}

static constexpr int SHUTDOWN_MSG = WM_APP + 0x100;
static DWORD MainThreadID;

void TraceRunner::PumpPlatformMessages(bool forever)
{
	MSG msg;
	while (true)
	{
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if (msg.message == SHUTDOWN_MSG)
				return;
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (!forever)
			return;
		WaitMessage();
	}
}

void TraceRunner::StopPlatformMessagePump()
{
	PostThreadMessageW(MainThreadID, SHUTDOWN_MSG, 0, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int wmain(int argc, wchar_t** argv)
{
	std::vector<std::string> u8_args;
	u8_args.reserve(static_cast<size_t>(argc));
	for (int i = 0; i < argc; i++)
		u8_args.push_back(StringUtil::WideStringToUTF8String(argv[i]));

	std::vector<char*> u8_argptrs;
	u8_argptrs.reserve(u8_args.size());
	for (int i = 0; i < argc; i++)
		u8_argptrs.push_back(u8_args[i].data());
	u8_argptrs.push_back(nullptr);

	MainThreadID = GetCurrentThreadId();

	return real_main(argc, u8_argptrs.data());
}

#elif defined(__APPLE__)

static void* s_window;
static WindowInfo s_wi;

bool TraceRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_window, "Tried to create window when there already was one!");
	s_window = CocoaTools::CreateWindow("PCSX2 Trace Runner", WINDOW_WIDTH, WINDOW_HEIGHT);
	CocoaTools::GetWindowInfoFromWindow(&s_wi, s_window);
	PumpPlatformMessages();
	return s_window;
}

void TraceRunner::DestroyPlatformWindow()
{
	if (s_window) {
		CocoaTools::DestroyWindow(s_window);
		s_window = nullptr;
	}
}

std::optional<WindowInfo> TraceRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_window)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void TraceRunner::PumpPlatformMessages(bool forever)
{
	CocoaTools::RunCocoaEventLoop(forever);
}

void TraceRunner::StopPlatformMessagePump()
{
	CocoaTools::StopMainThreadEventLoop();
}

#elif defined(__linux__)
static Display* s_display = nullptr;
static Window s_window = None;
static WindowInfo s_wi;
static std::atomic<bool> s_shutdown_requested{false};

bool TraceRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_display && s_window == None, "Tried to create window when there already was one!");

	s_display = XOpenDisplay(nullptr);
	if (!s_display)
	{
		Console.Error("Failed to open X11 display");
		return false;
	}

	int screen = DefaultScreen(s_display);
	Window root = RootWindow(s_display, screen);

	s_window = XCreateSimpleWindow(s_display, root, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, 1,
		BlackPixel(s_display, screen), WhitePixel(s_display, screen));

	if (s_window == None)
	{
		Console.Error("Failed to create X11 window");
		XCloseDisplay(s_display);
		s_display = nullptr;
		return false;
	}

	XStoreName(s_display, s_window, "PCSX2 Trace Runner");
	XSelectInput(s_display, s_window, StructureNotifyMask);
	XMapWindow(s_display, s_window);

	s_wi.type = WindowInfo::Type::X11;
	s_wi.display_connection = s_display;
	s_wi.window_handle = reinterpret_cast<void*>(s_window);
	s_wi.surface_width = WINDOW_WIDTH;
	s_wi.surface_height = WINDOW_HEIGHT;
	s_wi.surface_scale = 1.0f;

	XFlush(s_display);
	PumpPlatformMessages();
	return true;
}

void TraceRunner::DestroyPlatformWindow()
{
	if (s_display && s_window != None)
	{
		XDestroyWindow(s_display, s_window);
		s_window = None;
	}

	if (s_display)
	{
		XCloseDisplay(s_display);
		s_display = nullptr;
	}
}

std::optional<WindowInfo> TraceRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_display && s_window != None)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void TraceRunner::PumpPlatformMessages(bool forever)
{
	if (!s_display)
		return;

	do
	{
		while (XPending(s_display) > 0)
		{
			XEvent event;
			XNextEvent(s_display, &event);

			switch (event.type)
			{
				case ConfigureNotify:
				{
					const XConfigureEvent& configure = event.xconfigure;
					s_wi.surface_width = static_cast<u32>(configure.width);
					s_wi.surface_height = static_cast<u32>(configure.height);
					break;
				}
				case DestroyNotify:
					return;
				default:
					break;
			}
		}

		if (s_shutdown_requested.load())
			return;

		if (forever)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	} while (forever && !s_shutdown_requested.load());
}

void TraceRunner::StopPlatformMessagePump()
{
	s_shutdown_requested.store(true);
}
#endif // _WIN32 / __APPLE__
