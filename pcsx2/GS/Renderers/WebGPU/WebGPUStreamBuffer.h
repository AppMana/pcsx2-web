// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <webgpu/webgpu.h>

class WebGPUStreamBuffer
{
public:
	WebGPUStreamBuffer();
	WebGPUStreamBuffer(const WebGPUStreamBuffer&) = delete;
	~WebGPUStreamBuffer();

	WebGPUStreamBuffer& operator=(const WebGPUStreamBuffer&) = delete;

	__fi bool IsValid() const { return (m_buffer != nullptr); }
	__fi WGPUBuffer GetBuffer() const { return m_buffer; }
	__fi u8* GetHostPointer() const { return m_host_pointer; }
	__fi u8* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
	__fi u32 GetCurrentSize() const { return m_size; }
	__fi u32 GetCurrentSpace() const { return m_current_space; }
	__fi u32 GetCurrentOffset() const { return m_current_offset; }

	bool Create(WGPUDevice device, WGPUQueue queue, WGPUBufferUsage usage, u32 size, const char* label);
	void Destroy();

	bool ReserveMemory(u32 num_bytes, u32 alignment);
	void CommitMemory(u32 final_num_bytes);

	void Flush();
	void OnSubmit();

private:
	WGPUQueue m_queue = nullptr;
	WGPUBuffer m_buffer = nullptr;
	u8* m_host_pointer = nullptr;

	u32 m_size = 0;
	u32 m_limit = 0;
	u32 m_current_offset = 0;
	u32 m_current_space = 0;
	u32 m_dirty_start = 0;
	u32 m_encoder_start = 0;
	bool m_wrapped = false;
};
