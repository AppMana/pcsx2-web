// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Null/GSTextureNull.h"

GSTextureNull::GSTextureNull(Usage usage, int width, int height, int levels, Format format)
{
	m_size = GSVector2i(width, height);
	m_mipmap_levels = levels;
	m_usage = usage;
	m_format = format;
}

GSTextureNull::~GSTextureNull() = default;

void* GSTextureNull::GetNativeHandle() const
{
	return nullptr;
}

bool GSTextureNull::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	return true;
}

bool GSTextureNull::Map(GSMap& m, const GSVector4i* r, int layer)
{
	return false;
}

void GSTextureNull::Unmap()
{
}

void GSTextureNull::GenerateMipmap()
{
}

#ifdef PCSX2_DEVBUILD
void GSTextureNull::SetDebugName(std::string_view name)
{
}
#endif

GSDownloadTextureNull::GSDownloadTextureNull(u32 width, u32 height, GSTexture::Format format)
	: GSDownloadTexture(width, height, format)
	, m_buffer(GetBufferSize(width, height, format), 0)
{
	m_current_pitch = GetTransferPitch(width, 1);
}

GSDownloadTextureNull::~GSDownloadTextureNull() = default;

void GSDownloadTextureNull::CopyFromTexture(const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	m_current_pitch = GetTransferPitch(use_transfer_pitch ? static_cast<u32>(drc.width()) : m_width, 1);
}

bool GSDownloadTextureNull::Map(const GSVector4i& read_rc)
{
	m_map_pointer = m_buffer.data();
	return true;
}

void GSDownloadTextureNull::Unmap()
{
	m_map_pointer = nullptr;
}

void GSDownloadTextureNull::Flush()
{
}

#ifdef PCSX2_DEVBUILD
void GSDownloadTextureNull::SetDebugName(std::string_view name)
{
}
#endif
