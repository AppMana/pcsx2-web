// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Pcsx2Types.h"
#include "common/Console.h"
#include "common/FPControl.h"
#include "common/HostSys.h"
#include "common/Threading.h"
#include "common/WindowInfo.h"

#include <emscripten/heap.h>

#include <ctime>
#include <thread>
#include <unistd.h>

u64 GetPhysicalMemory()
{
	return static_cast<u64>(emscripten_get_heap_max());
}

u64 GetAvailablePhysicalMemory()
{
	const size_t max_size = emscripten_get_heap_max();
	const size_t cur_size = emscripten_get_heap_size();
	return (max_size > cur_size) ? static_cast<u64>(max_size - cur_size) : 0;
}

u64 GetTickFrequency()
{
	return 1000000000;
}

u64 GetCPUTicks()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (static_cast<u64>(ts.tv_sec) * 1000000000ULL) + ts.tv_nsec;
}

std::string GetOSVersionString()
{
	return "Emscripten";
}

bool Common::InhibitScreensaver(bool inhibit)
{
	return false;
}

void Common::SetMousePosition(int x, int y)
{
}

bool Common::AttachMousePositionCb(std::function<void(int, int)> cb)
{
	return false;
}

void Common::DetachMousePositionCb()
{
}

bool Common::PlaySoundAsync(const char* path)
{
	return false;
}

void Threading::Sleep(int ms)
{
	usleep(1000 * ms);
}

void Threading::SleepUntil(u64 ticks)
{
	const u64 now = GetCPUTicks();
	if (ticks <= now)
		return;

	const u64 delta = ticks - now;
	struct timespec ts;
	ts.tv_sec = static_cast<time_t>(delta / 1000000000ULL);
	ts.tv_nsec = static_cast<long>(delta % 1000000000ULL);
	nanosleep(&ts, nullptr);
}

static thread_local FPControlRegister s_wasm_fpcr = FPControlRegister::GetDefault();

FPControlRegister FPControlRegister::GetCurrent()
{
	return s_wasm_fpcr;
}

void FPControlRegister::SetCurrent(FPControlRegister value)
{
	s_wasm_fpcr = value;
}
