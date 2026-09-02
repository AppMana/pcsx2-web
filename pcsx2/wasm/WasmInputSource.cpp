// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "wasm/WasmInputSource.h"

WasmInputSource::WasmInputSource() = default;

WasmInputSource::~WasmInputSource() = default;

bool WasmInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	m_initialized = true;
	return true;
}

void WasmInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
}

bool WasmInputSource::ReloadDevices()
{
	return false;
}

void WasmInputSource::Shutdown()
{
	m_initialized = false;
}

bool WasmInputSource::IsInitialized()
{
	return m_initialized;
}

void WasmInputSource::PollEvents()
{
}

std::vector<std::pair<std::string, std::string>> WasmInputSource::EnumerateDevices()
{
	return {};
}

std::vector<InputBindingKey> WasmInputSource::EnumerateMotors()
{
	return {};
}

bool WasmInputSource::GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping)
{
	return false;
}

InputLayout WasmInputSource::GetControllerLayout(u32 index)
{
	return InputLayout::Unknown;
}

void WasmInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
}

void WasmInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity, float small_intensity)
{
}

std::optional<InputBindingKey> WasmInputSource::ParseKeyString(const std::string_view device, const std::string_view binding)
{
	return std::nullopt;
}

TinyString WasmInputSource::ConvertKeyToString(InputBindingKey key, bool display, bool migration)
{
	return TinyString();
}

TinyString WasmInputSource::ConvertKeyToIcon(InputBindingKey key)
{
	return TinyString();
}
