// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Browser audio output on Emscripten's Wasm Audio Worklets. The AudioContext belongs to the
// page (it needs a user gesture); the page registers it with the module's thread, which then
// starts the worklet thread, registers the processor and connects the node through the
// standard emscripten/webaudio.h calls. The worklet thread pulls AudioStream::ReadFrames
// straight out of the shared ring buffer once per render quantum.

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

namespace WebAudio
{
	enum class State : int
	{
		Failed = -1,
		Detached = 0,
		Starting = 1,
		Ready = 2,
	};

	// Counters at StatsAddress(), u32 each in this order, updated with atomics by the worklet
	// thread (callbacks..idle_callbacks) and the CPU thread (written_frames, dropped_chunks).
	enum StatsIndex : u32
	{
		STAT_STATE, // WebAudio::State
		STAT_SAMPLE_RATE, // context sample rate reported by the page
		STAT_QUANTUM, // frames per process() call
		STAT_CALLBACKS, // process() calls
		STAT_PULLED_FRAMES, // frames read from the stream
		STAT_NONZERO_FRAMES, // pulled frames with a non-zero sample in either channel
		STAT_UNDERRUNS, // process() calls that found fewer buffered frames than the quantum
		STAT_IDLE_CALLBACKS, // process() calls with no stream or a paused stream (silence)
		STAT_WRITTEN_FRAMES, // frames the SPU2 wrote to the stream
		STAT_COUNT
	};

	/// Starts the worklet on the page's AudioContext. Call once from the module's main thread
	/// after the page has handed over its message port; returns false when a previous attach is
	/// still in progress or succeeded.
	bool Attach(u32 sample_rate);

	State GetState();

	/// Address of the STAT_COUNT u32 counters for the page to read with atomics.
	const void* StatsAddress();

	/// True while a WebAudio backed stream is the SPU2 output.
	bool HasActiveStream();

	/// One audio.jsonl line (newline terminated) hashing every frame the SPU2 wrote since the
	/// previous call, the same record the tracerunner writes at each vsync. Empty when no
	/// WebAudio stream is active.
	std::string FormatAudioRecord(u32 frame);
} // namespace WebAudio
