// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/WebGPU/WebGPUStreamBuffer.h"

#include "common/BitUtils.h"
#include "common/Assertions.h"
#include "common/Console.h"

#include <cstdlib>
#include <cstring>

WebGPUStreamBuffer::WebGPUStreamBuffer() = default;

WebGPUStreamBuffer::~WebGPUStreamBuffer()
{
	Destroy();
}

bool WebGPUStreamBuffer::Create(WGPUDevice device, WGPUQueue queue, WGPUBufferUsage usage, u32 size, const char* label)
{
	Destroy();

	pxAssert((size & 3) == 0);

	WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
	desc.label = {label, WGPU_STRLEN};
	desc.usage = usage | WGPUBufferUsage_CopyDst;
	desc.size = size;
	m_buffer = wgpuDeviceCreateBuffer(device, &desc);
	if (!m_buffer)
	{
		Console.Error("WebGPU: Failed to create %u byte stream buffer '%s'", size, label);
		return false;
	}

	m_host_pointer = static_cast<u8*>(std::aligned_alloc(32, Common::AlignUpPow2(size, 32)));
	if (!m_host_pointer)
	{
		wgpuBufferRelease(m_buffer);
		m_buffer = nullptr;
		return false;
	}

	m_queue = queue;
	m_size = size;
	m_limit = size;
	m_current_offset = 0;
	m_current_space = size;
	m_dirty_start = 0;
	m_encoder_start = 0;
	m_wrapped = false;
	return true;
}

void WebGPUStreamBuffer::Destroy()
{
	if (m_buffer)
	{
		wgpuBufferRelease(m_buffer);
		m_buffer = nullptr;
	}
	if (m_host_pointer)
	{
		std::free(m_host_pointer);
		m_host_pointer = nullptr;
	}
	m_queue = nullptr;
	m_size = 0;
	m_limit = 0;
	m_current_offset = 0;
	m_current_space = 0;
	m_dirty_start = 0;
	m_encoder_start = 0;
	m_wrapped = false;
}

bool WebGPUStreamBuffer::ReserveMemory(u32 num_bytes, u32 alignment)
{
	const u32 aligned_offset = Common::AlignUp(m_current_offset, alignment);
	if (aligned_offset + num_bytes <= m_limit)
	{
		m_current_offset = aligned_offset;
		m_current_space = m_limit - aligned_offset;
		return true;
	}

	if (m_wrapped || num_bytes > m_encoder_start)
		return false;

	Flush();
	m_wrapped = true;
	m_limit = m_encoder_start;
	m_current_offset = 0;
	m_dirty_start = 0;
	m_current_space = m_limit;
	return true;
}

void WebGPUStreamBuffer::CommitMemory(u32 final_num_bytes)
{
	pxAssert(final_num_bytes <= m_current_space);
	m_current_offset += final_num_bytes;
	m_current_space -= final_num_bytes;
}

void WebGPUStreamBuffer::Flush()
{
	if (m_dirty_start == m_current_offset)
		return;

	const u32 start = m_dirty_start & ~3u;
	const u32 end = std::min(Common::AlignUpPow2(m_current_offset, 4), m_size);
	wgpuQueueWriteBuffer(m_queue, m_buffer, start, m_host_pointer + start, end - start);
	m_dirty_start = m_current_offset;
}

void WebGPUStreamBuffer::OnSubmit()
{
	Flush();
	m_encoder_start = m_current_offset;
	m_dirty_start = m_current_offset;
	m_wrapped = false;
	m_limit = m_size;
	m_current_space = m_size - m_current_offset;
}
