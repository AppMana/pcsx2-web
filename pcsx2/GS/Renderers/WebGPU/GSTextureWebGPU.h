// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSTexture.h"
#include "GS/GS.h"

#include <webgpu/webgpu.h>

#include <limits>
#include <memory>
#include <vector>

class GSTextureWebGPU final : public GSTexture
{
public:
	~GSTextureWebGPU() override;

	static std::unique_ptr<GSTextureWebGPU> Create(Usage usage, Format format, int width, int height, int levels);
	static std::unique_ptr<GSTextureWebGPU> Adopt(WGPUTexture texture, Usage usage, Format format, int width, int height, WGPUTextureFormat wgpu_format);

	void Destroy();

	__fi WGPUTexture GetTexture() const { return m_texture; }
	__fi WGPUTextureView GetView() const { return m_view; }
	__fi WGPUTextureFormat GetWGPUFormat() const { return m_wgpu_format; }
	__fi bool HasStencil() const { return (m_wgpu_format == WGPUTextureFormat_Depth32FloatStencil8); }

	WGPUTextureView GetAttachmentView(int level = 0);
	WGPUTextureView GetLevelView(int level);

	void* GetNativeHandle() const override;

	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) override;
	bool Map(GSMap& m, const GSVector4i* r = nullptr, int layer = 0) override;
	void Unmap() override;
	void GenerateMipmap() override;

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override;
#endif

	void CommitClear();

	__fi void SetUseFenceCounter(u64 counter) { m_use_fence_counter = counter; }
	__fi u64 GetUseFenceCounter() const { return m_use_fence_counter; }

private:
	GSTextureWebGPU(Usage usage, Format format, int width, int height, int levels, WGPUTexture texture,
		WGPUTextureView view, WGPUTextureFormat wgpu_format);

	WGPUTextureView CreateView(int base_level, int num_levels, bool attachment) const;
	bool UploadFromStagingRing(int level, const GSVector4i& rc, u32 buffer_offset, u32 pitch);

	WGPUTexture m_texture = nullptr;
	WGPUTextureView m_view = nullptr;
	WGPUTextureView m_attachment_view = nullptr;
	std::vector<WGPUTextureView> m_level_views;
	std::vector<WGPUTextureView> m_level_attachment_views;
	WGPUTextureFormat m_wgpu_format = WGPUTextureFormat_Undefined;

	u64 m_use_fence_counter = 0;

	int m_map_level = std::numeric_limits<int>::max();
	u32 m_map_pitch = 0;
	u32 m_map_offset = 0;
	GSVector4i m_map_area = GSVector4i::zero();
};

class GSDownloadTextureWebGPU final : public GSDownloadTexture
{
public:
	~GSDownloadTextureWebGPU() override;

	static std::unique_ptr<GSDownloadTextureWebGPU> Create(u32 width, u32 height, GSTexture::Format format);

	void CopyFromTexture(
		const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch) override;

	bool Map(const GSVector4i& read_rc) override;
	void Unmap() override;

	void Flush() override;

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override;
#endif

private:
	enum class MapState
	{
		Unmapped,
		Pending,
		Mapped
	};

	GSDownloadTextureWebGPU(u32 width, u32 height, GSTexture::Format format);

	static void MapCallback(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void* userdata2);

	void UnmapBuffer();

	WGPUBuffer m_buffer = nullptr;
	WGPUFuture m_map_future = WGPU_FUTURE_INIT;
	MapState m_map_state = MapState::Unmapped;
	bool m_map_failed = false;

	u64 m_copy_fence_counter = 0;
	u32 m_buffer_size = 0;
};
