// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Host/AudioStream.h"

std::vector<std::pair<std::string, std::string>> AudioStream::GetCubebDriverNames()
{
	return {};
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetCubebOutputDevices(const char* driver)
{
	return {};
}

std::unique_ptr<AudioStream> AudioStream::CreateCubebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	const char* driver_name, const char* device_name, bool stretch_enabled, Error* error)
{
	return CreateNullStream(sample_rate, parameters.buffer_ms);
}

std::unique_ptr<AudioStream> AudioStream::CreateSDLAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	bool stretch_enabled, Error* error)
{
	return CreateNullStream(sample_rate, parameters.buffer_ms);
}
