// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Host/WebAudioStream.h"
#include "Host/AudioStream.h"

#include "GS/GSXXH.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"

#include "fmt/format.h"

#include <emscripten/webaudio.h>

#include <alloca.h>
#include <atomic>
#include <cstring>
#include <malloc.h>

namespace
{
	class WebAudioStream final : public AudioStream
	{
	public:
		WebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
		~WebAudioStream() override;

		void Open(bool stretch_enabled);

		/// Worklet thread: fills one render quantum of planar output from the ring buffer.
		void Render(float* planar, u32 channels, u32 frames);

		/// CPU thread: the audio.jsonl record for the frames written since the previous call.
		std::string FormatRecord(u32 frame);

	protected:
		void OnChunkWritten(const SampleType* chunk) override;

	private:
		XXH3_state_t m_write_hash;
		u32 m_write_hash_frames = 0;
	};
} // namespace

namespace WebAudio
{
	static constexpr u32 WORKLET_STACK_SIZE = 128 * 1024;
	static constexpr const char* PROCESSOR_NAME = "pcsx2-audio";

	static bool ProcessCallback(int num_inputs, const AudioSampleFrame* inputs, int num_outputs,
		AudioSampleFrame* outputs, int num_params, const AudioParamFrame* params, void* user_data);
	static void OnWorkletStarted(EMSCRIPTEN_WEBAUDIO_T context, bool success, void* user_data);
	static void OnProcessorCreated(EMSCRIPTEN_WEBAUDIO_T context, bool success, void* user_data);

	static std::atomic<u32> s_stats[STAT_COUNT];

	// The worklet thread only ever sees the stream through this pointer. It marks itself busy
	// before loading the pointer, and the stream's destructor clears the pointer before waiting
	// for the busy flag to drop, so a render never outlives the stream it renders from.
	static std::atomic<WebAudioStream*> s_active_stream{nullptr};
	static std::atomic<u32> s_render_busy{0};

	static EMSCRIPTEN_WEBAUDIO_T s_context = 0;
	static EMSCRIPTEN_WEBAUDIO_T s_node = 0;
	static void* s_worklet_stack = nullptr;

	static void SetState(State state)
	{
		s_stats[STAT_STATE].store(static_cast<u32>(static_cast<int>(state)), std::memory_order_release);
	}
} // namespace WebAudio

WebAudioStream::WebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
	: AudioStream(sample_rate, parameters)
{
	XXH3_64bits_reset(&m_write_hash);
}

WebAudioStream::~WebAudioStream()
{
	WebAudioStream* expected = this;
	if (WebAudio::s_active_stream.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst))
	{
		while (WebAudio::s_render_busy.load(std::memory_order_seq_cst) != 0)
		{
		}
	}
}

void WebAudioStream::Open(bool stretch_enabled)
{
	BaseInitialize(&StereoSampleReaderImpl, stretch_enabled);
	WebAudio::s_active_stream.store(this, std::memory_order_seq_cst);
}

void WebAudioStream::Render(float* planar, u32 channels, u32 frames)
{
	if (GetBufferedFramesRelaxed() < frames)
		WebAudio::s_stats[WebAudio::STAT_UNDERRUNS].fetch_add(1, std::memory_order_relaxed);

	SampleType* interleaved = static_cast<SampleType*>(alloca(frames * NUM_INPUT_CHANNELS * sizeof(SampleType)));
	ReadFrames(interleaved, frames);

	u32 nonzero = 0;
	for (u32 i = 0; i < frames; i++)
	{
		const float left = interleaved[i * 2];
		const float right = interleaved[i * 2 + 1];
		planar[i] = left;
		if (channels > 1)
			planar[frames + i] = right;
		nonzero += (left != 0.0f || right != 0.0f) ? 1 : 0;
	}
	for (u32 c = 2; c < channels; c++)
		std::memset(planar + c * frames, 0, frames * sizeof(float));

	WebAudio::s_stats[WebAudio::STAT_PULLED_FRAMES].fetch_add(frames, std::memory_order_relaxed);
	WebAudio::s_stats[WebAudio::STAT_NONZERO_FRAMES].fetch_add(nonzero, std::memory_order_relaxed);
}

void WebAudioStream::OnChunkWritten(const SampleType* chunk)
{
	XXH3_64bits_update(&m_write_hash, chunk, CHUNK_SIZE * NUM_INPUT_CHANNELS * sizeof(SampleType));
	m_write_hash_frames += CHUNK_SIZE;
	WebAudio::s_stats[WebAudio::STAT_WRITTEN_FRAMES].fetch_add(CHUNK_SIZE, std::memory_order_relaxed);
}

std::string WebAudioStream::FormatRecord(u32 frame)
{
	const u64 hash = XXH3_64bits_digest(&m_write_hash);
	const u32 frames = m_write_hash_frames;
	XXH3_64bits_reset(&m_write_hash);
	m_write_hash_frames = 0;
	return fmt::format("{{\"frame\":{},\"frames\":{},\"hash\":\"{:016x}\"}}\n", frame, frames, hash);
}

std::unique_ptr<AudioStream> AudioStream::CreateWebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	bool stretch_enabled, Error* error)
{
	if (WebAudio::s_active_stream.load(std::memory_order_acquire))
	{
		Error::SetStringView(error, "A Web Audio stream is already open.");
		return nullptr;
	}

	// The worklet node has one stereo output; expansion modes have nowhere to go.
	AudioStreamParameters params = parameters;
	params.expansion_mode = AudioExpansionMode::Disabled;

	std::unique_ptr<WebAudioStream> stream = std::make_unique<WebAudioStream>(sample_rate, params);
	stream->Open(stretch_enabled);
	return stream;
}

bool WebAudio::ProcessCallback(int num_inputs, const AudioSampleFrame* inputs, int num_outputs,
	AudioSampleFrame* outputs, int num_params, const AudioParamFrame* params, void* user_data)
{
	s_stats[STAT_CALLBACKS].fetch_add(1, std::memory_order_relaxed);
	if (num_outputs < 1)
		return true;

	AudioSampleFrame& out = outputs[0];
	const u32 channels = static_cast<u32>(out.numberOfChannels);
	const u32 frames = static_cast<u32>(out.samplesPerChannel);

	s_render_busy.store(1, std::memory_order_seq_cst);
	WebAudioStream* stream = s_active_stream.load(std::memory_order_seq_cst);
	if (stream && !stream->IsPaused())
	{
		stream->Render(out.data, channels, frames);
	}
	else
	{
		std::memset(out.data, 0, channels * frames * sizeof(float));
		s_stats[STAT_IDLE_CALLBACKS].fetch_add(1, std::memory_order_relaxed);
	}
	s_render_busy.store(0, std::memory_order_seq_cst);

	// Keep the processor alive; silence is produced while nothing is playing.
	return true;
}

void WebAudio::OnWorkletStarted(EMSCRIPTEN_WEBAUDIO_T context, bool success, void* user_data)
{
	if (!success)
	{
		Console.Error("WebAudio: starting the audio worklet thread failed.");
		SetState(State::Failed);
		return;
	}

	WebAudioWorkletProcessorCreateOptions options = {};
	options.name = PROCESSOR_NAME;
	options.numAudioParams = 0;
	options.audioParamDescriptors = nullptr;
	emscripten_create_wasm_audio_worklet_processor_async(context, &options, &OnProcessorCreated, nullptr);
}

void WebAudio::OnProcessorCreated(EMSCRIPTEN_WEBAUDIO_T context, bool success, void* user_data)
{
	if (!success)
	{
		Console.Error("WebAudio: registering the audio worklet processor failed.");
		SetState(State::Failed);
		return;
	}

	int output_channels[1] = {static_cast<int>(AudioStream::NUM_INPUT_CHANNELS)};
	EmscriptenAudioWorkletNodeCreateOptions options = {};
	options.numberOfInputs = 0;
	options.numberOfOutputs = 1;
	options.outputChannelCounts = output_channels;
	options.channelCount = 0;
	options.channelCountMode = WEBAUDIO_CHANNEL_COUNT_MODE_MAX;
	options.channelInterpretation = WEBAUDIO_CHANNEL_INTERPRETATION_SPEAKERS;
	s_node = emscripten_create_wasm_audio_worklet_node(context, PROCESSOR_NAME, &options, &ProcessCallback, nullptr);
	emscripten_audio_node_connect(s_node, context, 0, 0);

	s_stats[STAT_SAMPLE_RATE].store(static_cast<u32>(emscripten_audio_context_sample_rate(context)), std::memory_order_relaxed);
	s_stats[STAT_QUANTUM].store(static_cast<u32>(emscripten_audio_context_quantum_size(context)), std::memory_order_relaxed);
	Console.WriteLnFmt("WebAudio: worklet node connected ({} Hz, {} frames per quantum).",
		s_stats[STAT_SAMPLE_RATE].load(std::memory_order_relaxed), s_stats[STAT_QUANTUM].load(std::memory_order_relaxed));
	SetState(State::Ready);
}

bool WebAudio::Attach(u32 sample_rate)
{
	const State state = GetState();
	if (state == State::Starting || state == State::Ready)
		return false;

	if (!s_worklet_stack)
		s_worklet_stack = memalign(16, WORKLET_STACK_SIZE);
	if (!s_worklet_stack)
	{
		SetState(State::Failed);
		return false;
	}

	EmscriptenWebAudioCreateAttributes attributes = {};
	attributes.latencyHint = "interactive";
	attributes.sampleRate = sample_rate;
	attributes.renderSizeHint = AUDIO_CONTEXT_RENDER_SIZE_DEFAULT;
	s_context = emscripten_create_audio_context(&attributes);
	if (s_context <= 0)
	{
		SetState(State::Failed);
		return false;
	}

	SetState(State::Starting);
	emscripten_start_wasm_audio_worklet_thread_async(s_context, s_worklet_stack, WORKLET_STACK_SIZE, &OnWorkletStarted, nullptr);
	return true;
}

WebAudio::State WebAudio::GetState()
{
	return static_cast<State>(static_cast<int>(s_stats[STAT_STATE].load(std::memory_order_acquire)));
}

const void* WebAudio::StatsAddress()
{
	return s_stats;
}

bool WebAudio::HasActiveStream()
{
	return s_active_stream.load(std::memory_order_acquire) != nullptr;
}

std::string WebAudio::FormatAudioRecord(u32 frame)
{
	WebAudioStream* stream = s_active_stream.load(std::memory_order_acquire);
	if (!stream)
		return std::string();
	return stream->FormatRecord(frame);
}
