// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSTexture.h"

#include <vector>

class GSTextureNull final : public GSTexture
{
public:
	GSTextureNull(Usage usage, int width, int height, int levels, Format format);
	~GSTextureNull() override;

	void* GetNativeHandle() const override;

	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) override;
	bool Map(GSMap& m, const GSVector4i* r = nullptr, int layer = 0) override;
	void Unmap() override;
	void GenerateMipmap() override;

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override;
#endif
};

class GSDownloadTextureNull final : public GSDownloadTexture
{
public:
	GSDownloadTextureNull(u32 width, u32 height, GSTexture::Format format);
	~GSDownloadTextureNull() override;

	void CopyFromTexture(const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch) override;

	bool Map(const GSVector4i& read_rc) override;
	void Unmap() override;
	void Flush() override;

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override;
#endif

private:
	std::vector<u8> m_buffer;
};
