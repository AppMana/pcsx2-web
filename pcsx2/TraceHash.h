// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Per-vsync state hashing shared by the trace hosts (pcsx2-tracerunner and the browser host), so
// that both produce the same cpu.jsonl and audio.jsonl records for the same emulated frames.

#pragma once

#include "common/Pcsx2Defs.h"

#include <memory>
#include <string>

class AudioStream;
struct VURegs;

namespace TraceHash
{
	u64 HashBuffer(const void* data, size_t size);
	std::string HashToString(u64 hash);

	u64 HashEE();
	u64 HashIOP();
	u64 HashVU(const VURegs& vu);
	u64 HashVUMemory();
	u64 HashEERAM();
	u64 HashIOPRAM();
	u64 HashSPU2RAM();

	/// One cpu.jsonl line (newline terminated) for the vsync at frame. The memory hashes are
	/// included when ram_every is non-zero and divides frame.
	std::string FormatCPURecord(u32 frame, u32 ram_every);

	/// Null audio stream that retains the SPU2 output for hashing. Install it with
	/// AudioStream::SetNullStreamFactory before the VM opens its audio output.
	std::unique_ptr<AudioStream> CreateAudioStream(u32 sample_rate, u32 buffer_ms);

	/// One audio.jsonl line (newline terminated) hashing the samples produced since the previous
	/// call. Empty when no stream from CreateAudioStream is active.
	std::string FormatAudioRecord(u32 frame);
} // namespace TraceHash
