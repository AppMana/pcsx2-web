// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "TraceHash.h"

#include "Host/AudioStream.h"
#include "Memory.h"
#include "R3000A.h"
#include "R5900.h"
#include "SPU2/defs.h"
#include "VU.h"
#include "VUmicro.h"

#include "fmt/format.h"

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include <xxhash.h>

#include <vector>

namespace TraceHash
{
	class TraceAudioStream final : public AudioStream
	{
	public:
		TraceAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
			: AudioStream(sample_rate, parameters)
		{
			BaseInitialize(&StereoSampleReaderImpl, false);
		}

		~TraceAudioStream() override;

		u32 Drain(std::vector<SampleType>& samples)
		{
			const u32 num_frames = GetBufferedFramesRelaxed();
			samples.resize(static_cast<size_t>(num_frames) * NUM_INPUT_CHANNELS);
			if (num_frames > 0)
				ReadFrames(samples.data(), num_frames);
			return num_frames;
		}
	};

	static TraceAudioStream* s_audio_stream = nullptr;
	static std::vector<AudioStream::SampleType> s_audio_samples;

	static void HashUpdate(XXH3_state_t* state, const void* data, size_t size)
	{
		XXH3_64bits_update(state, data, size);
	}

	template <typename T>
	static void HashUpdate(XXH3_state_t* state, const T& value)
	{
		XXH3_64bits_update(state, &value, sizeof(value));
	}
} // namespace TraceHash

TraceHash::TraceAudioStream::~TraceAudioStream()
{
	if (s_audio_stream == this)
		s_audio_stream = nullptr;
}

u64 TraceHash::HashBuffer(const void* data, size_t size)
{
	return XXH3_64bits(data, size);
}

std::string TraceHash::HashToString(u64 hash)
{
	return fmt::format("{:016x}", hash);
}

u64 TraceHash::HashEE()
{
	XXH3_state_t state;
	XXH3_64bits_reset(&state);
	HashUpdate(&state, cpuRegs.GPR);
	HashUpdate(&state, cpuRegs.HI);
	HashUpdate(&state, cpuRegs.LO);
	HashUpdate(&state, &cpuRegs.CP0.r[0], sizeof(u32) * 9);
	HashUpdate(&state, &cpuRegs.CP0.r[10], sizeof(u32) * 22);
	HashUpdate(&state, cpuRegs.pc);
	HashUpdate(&state, cpuRegs.sa);
	HashUpdate(&state, fpuRegs.fpr);
	HashUpdate(&state, fpuRegs.fprc);
	HashUpdate(&state, fpuRegs.ACC);
	return XXH3_64bits_digest(&state);
}

u64 TraceHash::HashIOP()
{
	XXH3_state_t state;
	XXH3_64bits_reset(&state);
	HashUpdate(&state, psxRegs.GPR);
	HashUpdate(&state, psxRegs.CP0);
	HashUpdate(&state, psxRegs.pc);
	HashUpdate(&state, psxRegs.interrupt);
	return XXH3_64bits_digest(&state);
}

u64 TraceHash::HashVU(const VURegs& vu)
{
	XXH3_state_t state;
	XXH3_64bits_reset(&state);
	HashUpdate(&state, vu.VF);
	for (const REG_VI& vi : vu.VI)
		HashUpdate(&state, vi.UL);
	HashUpdate(&state, vu.ACC);
	HashUpdate(&state, vu.q.UL);
	HashUpdate(&state, vu.p.UL);
	HashUpdate(&state, vu.micro_macflags);
	HashUpdate(&state, vu.micro_clipflags);
	HashUpdate(&state, vu.micro_statusflags);
	HashUpdate(&state, vu.macflag);
	HashUpdate(&state, vu.statusflag);
	HashUpdate(&state, vu.clipflag);
	return XXH3_64bits_digest(&state);
}

u64 TraceHash::HashVUMemory()
{
	XXH3_state_t state;
	XXH3_64bits_reset(&state);
	HashUpdate(&state, VU0.Micro, VU0_PROGSIZE);
	HashUpdate(&state, VU0.Mem, VU0_MEMSIZE);
	HashUpdate(&state, VU1.Micro, VU1_PROGSIZE);
	HashUpdate(&state, VU1.Mem, VU1_MEMSIZE);
	return XXH3_64bits_digest(&state);
}

u64 TraceHash::HashEERAM()
{
	return XXH3_64bits(eeMem->Main, Ps2MemSize::MainRam);
}

u64 TraceHash::HashIOPRAM()
{
	return XXH3_64bits(iopMem->Main, Ps2MemSize::IopRam);
}

u64 TraceHash::HashSPU2RAM()
{
	return XXH3_64bits(_spu2mem, sizeof(_spu2mem));
}

std::string TraceHash::FormatCPURecord(u32 frame, u32 ram_every)
{
	std::string line = fmt::format("{{\"frame\":{},\"ee\":\"{}\",\"iop\":\"{}\",\"vu0\":\"{}\",\"vu1\":\"{}\"", frame,
		HashToString(HashEE()), HashToString(HashIOP()), HashToString(HashVU(VU0)), HashToString(HashVU(VU1)));

	if (ram_every > 0 && (frame % ram_every) == 0)
	{
		line += fmt::format(",\"eeram\":\"{}\",\"iopram\":\"{}\",\"vumem\":\"{}\",\"spu2ram\":\"{}\"",
			HashToString(HashEERAM()), HashToString(HashIOPRAM()), HashToString(HashVUMemory()),
			HashToString(HashSPU2RAM()));
	}

	line += "}\n";
	return line;
}

std::unique_ptr<AudioStream> TraceHash::CreateAudioStream(u32 sample_rate, u32 buffer_ms)
{
	AudioStreamParameters params;
	params.expansion_mode = AudioExpansionMode::Disabled;
	params.buffer_ms = 1000;

	std::unique_ptr<TraceAudioStream> stream = std::make_unique<TraceAudioStream>(sample_rate, params);
	s_audio_stream = stream.get();
	return stream;
}

std::string TraceHash::FormatAudioRecord(u32 frame)
{
	if (!s_audio_stream)
		return std::string();

	const u32 num_frames = s_audio_stream->Drain(s_audio_samples);
	const u64 hash = XXH3_64bits(s_audio_samples.data(), s_audio_samples.size() * sizeof(AudioStream::SampleType));
	return fmt::format("{{\"frame\":{},\"frames\":{},\"hash\":\"{}\"}}\n", frame, num_frames, HashToString(hash));
}
