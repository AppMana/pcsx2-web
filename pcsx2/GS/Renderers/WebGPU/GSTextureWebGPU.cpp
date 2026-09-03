// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSGL.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/WebGPU/GSDeviceWebGPU.h"
#include "GS/Renderers/WebGPU/GSTextureWebGPU.h"

#include "common/BitUtils.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "common/StringUtil.h"

#include <memory>
#include <utility>

static bool IsDepthFormat(WGPUTextureFormat format)
{
	return (format == WGPUTextureFormat_Depth32Float || format == WGPUTextureFormat_Depth32FloatStencil8);
}

GSTextureWebGPU::GSTextureWebGPU(Usage usage, Format format, int width, int height, int levels, WGPUTexture texture,
	WGPUTextureView view, WGPUTextureFormat wgpu_format)
	: GSTexture()
	, m_texture(texture)
	, m_view(view)
	, m_wgpu_format(wgpu_format)
{
	m_usage = usage;
	m_format = format;
	m_size.x = width;
	m_size.y = height;
	m_mipmap_levels = levels;
}

GSTextureWebGPU::~GSTextureWebGPU()
{
	Destroy();
}

std::unique_ptr<GSTextureWebGPU> GSTextureWebGPU::Create(Usage usage, Format format, int width, int height, int levels)
{
	pxAssert(ValidateUsageAndFormat(usage, format));

	GSDeviceWebGPU* const dev = GSDeviceWebGPU::GetInstance();
	const WGPUTextureFormat wgpu_format = dev->LookupNativeFormat(format);
	if (wgpu_format == WGPUTextureFormat_Undefined)
		return {};

	WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
	desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
	if (IsRenderTargetOrDepthStencil(usage) || (levels > 1 && !IsCompressedFormat(format)))
		desc.usage |= WGPUTextureUsage_RenderAttachment;
	desc.dimension = WGPUTextureDimension_2D;
	desc.size = {static_cast<u32>(width), static_cast<u32>(height), 1u};
	desc.format = wgpu_format;
	desc.mipLevelCount = static_cast<u32>(levels);
	desc.sampleCount = 1;

	WGPUTexture texture = wgpuDeviceCreateTexture(dev->GetDevice(), &desc);
	if (!texture)
	{
		Console.Error("WebGPU: Failed to create %dx%d texture (format %u)", width, height, static_cast<u32>(format));
		return {};
	}

	std::unique_ptr<GSTextureWebGPU> tex(new GSTextureWebGPU(usage, format, width, height, levels, texture, nullptr, wgpu_format));
	tex->m_view = tex->CreateView(0, levels, false);
	if (!tex->m_view)
	{
		Console.Error("WebGPU: Failed to create texture view");
		return {};
	}

	return tex;
}

std::unique_ptr<GSTextureWebGPU> GSTextureWebGPU::Adopt(WGPUTexture texture, Usage usage, Format format, int width, int height, WGPUTextureFormat wgpu_format)
{
	wgpuTextureAddRef(texture);
	std::unique_ptr<GSTextureWebGPU> tex(new GSTextureWebGPU(usage, format, width, height, 1, texture, nullptr, wgpu_format));
	tex->m_view = tex->CreateView(0, 1, true);
	if (!tex->m_view)
		return {};

	tex->m_attachment_view = tex->m_view;
	wgpuTextureViewAddRef(tex->m_attachment_view);
	return tex;
}

WGPUTextureView GSTextureWebGPU::CreateView(int base_level, int num_levels, bool attachment) const
{
	WGPUTextureViewDescriptor desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	desc.format = m_wgpu_format;
	desc.dimension = WGPUTextureViewDimension_2D;
	desc.baseMipLevel = static_cast<u32>(base_level);
	desc.mipLevelCount = static_cast<u32>(num_levels);
	desc.baseArrayLayer = 0;
	desc.arrayLayerCount = 1;
	if (!attachment && IsDepthFormat(m_wgpu_format))
	{
		desc.aspect = WGPUTextureAspect_DepthOnly;
		desc.format = WGPUTextureFormat_Depth32Float;
	}
	else
	{
		desc.aspect = WGPUTextureAspect_All;
	}
	return wgpuTextureCreateView(m_texture, &desc);
}

WGPUTextureView GSTextureWebGPU::GetAttachmentView(int level)
{
	if (level == 0)
	{
		if (!m_attachment_view)
		{
			if (m_mipmap_levels == 1 && !IsDepthFormat(m_wgpu_format))
			{
				m_attachment_view = m_view;
				wgpuTextureViewAddRef(m_attachment_view);
			}
			else
			{
				m_attachment_view = CreateView(0, 1, true);
			}
		}

		return m_attachment_view;
	}

	if (m_level_attachment_views.empty())
		m_level_attachment_views.resize(m_mipmap_levels, nullptr);
	if (!m_level_attachment_views[level])
		m_level_attachment_views[level] = CreateView(level, 1, true);
	return m_level_attachment_views[level];
}

WGPUTextureView GSTextureWebGPU::GetLevelView(int level)
{
	if (m_mipmap_levels == 1)
		return m_view;

	if (m_level_views.empty())
		m_level_views.resize(m_mipmap_levels, nullptr);
	if (!m_level_views[level])
		m_level_views[level] = CreateView(level, 1, false);
	return m_level_views[level];
}

void GSTextureWebGPU::Destroy()
{
	if (m_texture)
		GSDeviceWebGPU::GetInstance()->UnbindTexture(this);

	for (WGPUTextureView view : m_level_views)
	{
		if (view)
			wgpuTextureViewRelease(view);
	}
	m_level_views.clear();
	for (WGPUTextureView view : m_level_attachment_views)
	{
		if (view)
			wgpuTextureViewRelease(view);
	}
	m_level_attachment_views.clear();

	if (m_attachment_view)
	{
		wgpuTextureViewRelease(m_attachment_view);
		m_attachment_view = nullptr;
	}
	if (m_view)
	{
		wgpuTextureViewRelease(m_view);
		m_view = nullptr;
	}
	if (m_texture)
	{
		wgpuTextureRelease(m_texture);
		m_texture = nullptr;
	}

#ifdef PCSX2_DEVBUILD
	m_debug_name.clear();
#endif
}

void* GSTextureWebGPU::GetNativeHandle() const
{
	return const_cast<GSTextureWebGPU*>(this);
}

bool GSTextureWebGPU::UploadFromStagingRing(int level, const GSVector4i& rc, u32 buffer_offset, u32 pitch)
{
	GSDeviceWebGPU* const dev = GSDeviceWebGPU::GetInstance();
	dev->EndRenderPass();

	WGPUTexelCopyBufferInfo src = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
	src.buffer = dev->GetTextureUploadBuffer().GetBuffer();
	src.layout.offset = buffer_offset;
	src.layout.bytesPerRow = pitch;
	src.layout.rowsPerImage = WGPU_COPY_STRIDE_UNDEFINED;

	WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	dst.texture = m_texture;
	dst.mipLevel = static_cast<u32>(level);
	dst.origin = {static_cast<u32>(rc.x), static_cast<u32>(rc.y), 0u};
	dst.aspect = WGPUTextureAspect_All;

	const WGPUExtent3D size = {static_cast<u32>(rc.width()), static_cast<u32>(rc.height()), 1u};
	wgpuCommandEncoderCopyBufferToTexture(dev->GetCommandEncoder(), &src, &dst, &size);
	return true;
}

bool GSTextureWebGPU::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	if (layer >= m_mipmap_levels)
		return false;

	pxAssert(!IsDepthStencil());
	g_perfmon.Put(GSPerfMon::TextureUploads, 1);

	GSDeviceWebGPU* const dev = GSDeviceWebGPU::GetInstance();
	const u32 width = r.width();
	const u32 height = r.height();
	const u32 block_size = GetCompressedBlockSize();
	const u32 block_rows = (height + (block_size - 1)) / block_size;

	if (IsRenderTarget())
	{
		if (!r.eq(GSVector4i(0, 0, m_size.x, m_size.y)))
			CommitClear();
		else
			m_state = State::Dirty;
	}

	const bool used_this_frame = (m_use_fence_counter == dev->GetCurrentFenceCounter());
	if (!used_this_frame)
	{
		WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		dst.texture = m_texture;
		dst.mipLevel = static_cast<u32>(layer);
		dst.origin = {static_cast<u32>(r.x), static_cast<u32>(r.y), 0u};
		dst.aspect = WGPUTextureAspect_All;

		WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		layout.offset = 0;
		layout.bytesPerRow = static_cast<u32>(pitch);
		layout.rowsPerImage = WGPU_COPY_STRIDE_UNDEFINED;

		const WGPUExtent3D size = {width, height, 1u};
		wgpuQueueWriteTexture(dev->GetQueue(), &dst, data, static_cast<size_t>(pitch) * block_rows, &layout, &size);
	}
	else
	{
		const u32 upload_pitch = Common::AlignUpPow2(static_cast<u32>(pitch), GSDeviceWebGPU::TEXEL_COPY_ROW_ALIGNMENT);
		const u32 required_size = upload_pitch * block_rows;
		WebGPUStreamBuffer& sbuffer = dev->GetTextureUploadBuffer();
		if (required_size > (sbuffer.GetCurrentSize() / 2) || !sbuffer.ReserveMemory(required_size, GSDeviceWebGPU::TEXEL_COPY_ROW_ALIGNMENT))
		{
			dev->ExecuteCommandBuffer(false, "While waiting for %u bytes in texture upload buffer", required_size);
			return Update(r, data, pitch, layer);
		}

		StringUtil::StrideMemCpy(sbuffer.GetCurrentHostPointer(), upload_pitch, data, pitch, std::min<u32>(upload_pitch, pitch), block_rows);
		const u32 buffer_offset = sbuffer.GetCurrentOffset();
		sbuffer.CommitMemory(required_size);

		GL_PUSH("GSTextureWebGPU::Update({%d,%d} %dx%d Lvl:%u", r.x, r.y, r.width(), r.height(), layer);
		UploadFromStagingRing(layer, r, buffer_offset, upload_pitch);
	}

	if (IsTexture())
		m_needs_mipmaps_generated |= (layer == 0);

	return true;
}

bool GSTextureWebGPU::Map(GSMap& m, const GSVector4i* r, int layer)
{
	if (layer >= m_mipmap_levels || IsCompressedFormat())
		return false;

	GSDeviceWebGPU* const dev = GSDeviceWebGPU::GetInstance();
	m_map_area = r ? *r : GetRect();
	m_map_level = layer;
	m_map_pitch = Common::AlignUpPow2(CalcUploadPitch(m_map_area.width()), GSDeviceWebGPU::TEXEL_COPY_ROW_ALIGNMENT);

	const u32 required_size = CalcUploadSize(m_map_area.height(), m_map_pitch);
	WebGPUStreamBuffer& buffer = dev->GetTextureUploadBuffer();
	if (required_size >= (buffer.GetCurrentSize() / 2))
		return false;

	if (!buffer.ReserveMemory(required_size, GSDeviceWebGPU::TEXEL_COPY_ROW_ALIGNMENT))
	{
		dev->ExecuteCommandBuffer(false, "While waiting for %u bytes in texture upload buffer", required_size);
		if (!buffer.ReserveMemory(required_size, GSDeviceWebGPU::TEXEL_COPY_ROW_ALIGNMENT))
			pxFailRel("Failed to reserve texture upload memory");
	}

	m_map_offset = buffer.GetCurrentOffset();
	m.bits = buffer.GetCurrentHostPointer();
	m.pitch = static_cast<int>(m_map_pitch);
	return true;
}

void GSTextureWebGPU::Unmap()
{
	pxAssert(m_map_level < m_mipmap_levels && !IsCompressedFormat());
	g_perfmon.Put(GSPerfMon::TextureUploads, 1);

	GSDeviceWebGPU* const dev = GSDeviceWebGPU::GetInstance();
	const u32 height = m_map_area.height();
	const u32 required_size = CalcUploadSize(height, m_map_pitch);
	WebGPUStreamBuffer& buffer = dev->GetTextureUploadBuffer();
	const u8* host_data = buffer.GetCurrentHostPointer();
	buffer.CommitMemory(required_size);

	if (IsRenderTarget())
	{
		if (!m_map_area.eq(GSVector4i(0, 0, m_size.x, m_size.y)))
			CommitClear();
		else
			m_state = State::Dirty;
	}

	const bool used_this_frame = (m_use_fence_counter == dev->GetCurrentFenceCounter());
	if (!used_this_frame)
	{
		WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		dst.texture = m_texture;
		dst.mipLevel = static_cast<u32>(m_map_level);
		dst.origin = {static_cast<u32>(m_map_area.x), static_cast<u32>(m_map_area.y), 0u};
		dst.aspect = WGPUTextureAspect_All;

		WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		layout.offset = 0;
		layout.bytesPerRow = m_map_pitch;
		layout.rowsPerImage = WGPU_COPY_STRIDE_UNDEFINED;

		const WGPUExtent3D size = {static_cast<u32>(m_map_area.width()), height, 1u};
		wgpuQueueWriteTexture(dev->GetQueue(), &dst, host_data, required_size, &layout, &size);
	}
	else
	{
		GL_PUSH("GSTextureWebGPU::Unmap({%d,%d} %dx%d Lvl:%u", m_map_area.x, m_map_area.y, m_map_area.width(),
			m_map_area.height(), m_map_level);
		UploadFromStagingRing(m_map_level, m_map_area, m_map_offset, m_map_pitch);
	}

	if (IsTexture())
		m_needs_mipmaps_generated |= (m_map_level == 0);

	m_map_level = std::numeric_limits<int>::max();
}

void GSTextureWebGPU::GenerateMipmap()
{
	GSDeviceWebGPU::GetInstance()->GenerateMipmaps(this);
}

void GSTextureWebGPU::CommitClear()
{
	if (m_state != GSTexture::State::Cleared)
		return;

	GSDeviceWebGPU::GetInstance()->CommitClear(this);
}

#ifdef PCSX2_DEVBUILD

void GSTextureWebGPU::SetDebugName(std::string_view name)
{
	if (name.empty())
		return;

	wgpuTextureSetLabel(m_texture, {name.data(), name.size()});
	m_debug_name = name;
}

#endif

GSDownloadTextureWebGPU::GSDownloadTextureWebGPU(u32 width, u32 height, GSTexture::Format format)
	: GSDownloadTexture(width, height, format)
{
}

GSDownloadTextureWebGPU::~GSDownloadTextureWebGPU()
{
	if (m_buffer)
	{
		if (m_map_state == MapState::Pending)
		{
			if (m_async_map)
				CancelAsyncMap();
			else
				GSDeviceWebGPU::GetInstance()->WaitForFuture(m_map_future);
		}
		wgpuBufferRelease(m_buffer);
	}
}

std::unique_ptr<GSDownloadTextureWebGPU> GSDownloadTextureWebGPU::Create(u32 width, u32 height, GSTexture::Format format)
{
	const u32 buffer_size = Common::AlignUpPow2(GetBufferSize(width, height, format, GSDeviceWebGPU::TEXEL_COPY_ROW_ALIGNMENT), 4);

	WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
	desc.label = {"Download buffer", WGPU_STRLEN};
	desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
	desc.size = buffer_size;

	WGPUBuffer buffer = wgpuDeviceCreateBuffer(GSDeviceWebGPU::GetInstance()->GetDevice(), &desc);
	if (!buffer)
	{
		Console.Error("WebGPU: Failed to create %u byte download buffer", buffer_size);
		return {};
	}

	std::unique_ptr<GSDownloadTextureWebGPU> tex(new GSDownloadTextureWebGPU(width, height, format));
	tex->m_buffer = buffer;
	tex->m_buffer_size = buffer_size;
	return tex;
}

void GSDownloadTextureWebGPU::CancelAsyncMap()
{
	// wgpuBufferUnmap() on a buffer with a map in flight rejects that map; the callback still fires
	// (with an abort status) and must not touch this texture any more.
	AsyncMapRequest* const request = std::exchange(m_async_map, nullptr);
	request->owner = nullptr;
	const MapAsyncCallback callback = std::move(request->callback);
	m_map_state = MapState::Unmapped;
	m_map_pointer = nullptr;
	GSDeviceWebGPU::GetInstance()->AddPendingAsyncMap(-1);
	if (callback)
		callback(false);
}

void GSDownloadTextureWebGPU::UnmapBuffer()
{
	if (m_map_state == MapState::Pending)
	{
		if (m_async_map)
			CancelAsyncMap();
		else
			GSDeviceWebGPU::GetInstance()->WaitForFuture(m_map_future);
	}

	if (m_map_state != MapState::Unmapped)
		wgpuBufferUnmap(m_buffer);

	m_map_state = MapState::Unmapped;
	m_map_pointer = nullptr;
}

void GSDownloadTextureWebGPU::CopyFromTexture(
	const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	GSTextureWebGPU* const tex = static_cast<GSTextureWebGPU*>(stex);

	pxAssert(tex->GetFormat() == m_format);
	pxAssert(drc.width() == src.width() && drc.height() == src.height());
	pxAssert(src.z <= tex->GetWidth() && src.w <= tex->GetHeight());
	pxAssert(static_cast<u32>(drc.z) <= m_width && static_cast<u32>(drc.w) <= m_height);
	pxAssert(src_level < static_cast<u32>(tex->GetMipmapLevels()));
	pxAssert((drc.left == 0 && drc.top == 0) || !use_transfer_pitch);

	UnmapBuffer();

	u32 copy_offset, copy_size, copy_rows;
	m_current_pitch = GetTransferPitch(use_transfer_pitch ? static_cast<u32>(drc.width()) : m_width, GSDeviceWebGPU::TEXEL_COPY_ROW_ALIGNMENT);
	GetTransferSize(drc, &copy_offset, &copy_size, &copy_rows);

	g_perfmon.Put(GSPerfMon::Readbacks, 1);

	GSDeviceWebGPU* const dev = GSDeviceWebGPU::GetInstance();
	dev->EndRenderPass();
	tex->CommitClear();
	tex->SetUseFenceCounter(dev->GetCurrentFenceCounter());

	GL_INS("GSDownloadTextureWebGPU::CopyFromTexture: {%d,%d} %ux%u", src.left, src.top, src.width(), src.height());

	WGPUTexelCopyTextureInfo copy_src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	copy_src.texture = tex->GetTexture();
	copy_src.mipLevel = src_level;
	copy_src.origin = {static_cast<u32>(src.left), static_cast<u32>(src.top), 0u};
	copy_src.aspect = tex->IsDepthStencil() ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;

	WGPUTexelCopyBufferInfo copy_dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
	copy_dst.buffer = m_buffer;
	copy_dst.layout.offset = copy_offset;
	copy_dst.layout.bytesPerRow = m_current_pitch;
	copy_dst.layout.rowsPerImage = WGPU_COPY_STRIDE_UNDEFINED;

	const WGPUExtent3D extent = {static_cast<u32>(src.width()), static_cast<u32>(src.height()), 1u};
	wgpuCommandEncoderCopyTextureToBuffer(dev->GetCommandEncoder(), &copy_src, &copy_dst, &extent);

	m_copy_fence_counter = dev->GetCurrentFenceCounter();
	m_needs_flush = true;
}

void GSDownloadTextureWebGPU::MapCallback(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void* userdata2)
{
	GSDownloadTextureWebGPU* const tex = static_cast<GSDownloadTextureWebGPU*>(userdata1);
	if (status != WGPUMapAsyncStatus_Success)
	{
		Console.Error("WebGPU: Readback map failed: %.*s", static_cast<int>(message.length), message.data);
		tex->m_map_failed = true;
	}
}

void GSDownloadTextureWebGPU::MapCallbackAsync(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void* userdata2)
{
	std::unique_ptr<AsyncMapRequest> request(static_cast<AsyncMapRequest*>(userdata1));
	GSDownloadTextureWebGPU* const tex = request->owner;
	if (!tex)
		return;

	tex->m_async_map = nullptr;
	GSDeviceWebGPU::GetInstance()->AddPendingAsyncMap(-1);
	if (status != WGPUMapAsyncStatus_Success)
	{
		Console.Error("WebGPU: Asynchronous readback map failed (%u): %.*s", static_cast<u32>(status), static_cast<int>(message.length), message.data);
		tex->m_map_state = MapState::Unmapped;
		tex->m_map_pointer = nullptr;
		request->callback(false);
		return;
	}

	tex->m_map_pointer = static_cast<const u8*>(wgpuBufferGetConstMappedRange(tex->m_buffer, 0, tex->m_buffer_size));
	tex->m_map_state = MapState::Mapped;
	request->callback(tex->m_map_pointer != nullptr);
}

bool GSDownloadTextureWebGPU::MapAsync(MapAsyncCallback callback)
{
	if (m_needs_flush)
		Flush();

	if (m_map_state == MapState::Mapped)
	{
		callback(true);
		return true;
	}
	if (m_map_state == MapState::Pending)
		return false;

	AsyncMapRequest* const request = new AsyncMapRequest{this, std::move(callback)};
	WGPUBufferMapCallbackInfo cbi = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
	cbi.mode = WGPUCallbackMode_AllowSpontaneous;
	cbi.callback = &GSDownloadTextureWebGPU::MapCallbackAsync;
	cbi.userdata1 = request;
	m_map_failed = false;
	m_async_map = request;
	m_map_state = MapState::Pending;
	GSDeviceWebGPU::GetInstance()->AddPendingAsyncMap(1);
	m_map_future = wgpuBufferMapAsync(m_buffer, WGPUMapMode_Read, 0, WGPU_WHOLE_MAP_SIZE, cbi);
	return true;
}

bool GSDownloadTextureWebGPU::Map(const GSVector4i& read_rc)
{
	if (m_map_state == MapState::Mapped)
		return true;

	if (m_needs_flush)
		Flush();

	GSDeviceWebGPU* const dev = GSDeviceWebGPU::GetInstance();

	// Without a blocking wait the contents are only reachable through MapAsync(); readers that
	// need them now (the texture cache) are disabled by HWDownloadMode = NoReadbacks.
	if (dev->IsEventLoopDriven())
		return false;

	if (m_map_state == MapState::Unmapped)
	{
		WGPUBufferMapCallbackInfo cbi = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
		cbi.mode = WGPUCallbackMode_AllowProcessEvents;
		cbi.callback = &GSDownloadTextureWebGPU::MapCallback;
		cbi.userdata1 = this;
		m_map_failed = false;
		m_map_future = wgpuBufferMapAsync(m_buffer, WGPUMapMode_Read, 0, WGPU_WHOLE_MAP_SIZE, cbi);
		m_map_state = MapState::Pending;
	}

	if (!dev->WaitForFuture(m_map_future) || m_map_failed)
	{
		m_map_state = MapState::Unmapped;
		m_map_pointer = nullptr;
		return false;
	}

	m_map_pointer = static_cast<const u8*>(wgpuBufferGetConstMappedRange(m_buffer, 0, m_buffer_size));
	m_map_state = MapState::Mapped;
	return (m_map_pointer != nullptr);
}

void GSDownloadTextureWebGPU::Unmap()
{
}

void GSDownloadTextureWebGPU::Flush()
{
	if (!m_needs_flush)
		return;

	m_needs_flush = false;

	GSDeviceWebGPU* const dev = GSDeviceWebGPU::GetInstance();
	if (dev->GetCompletedFenceCounter() >= m_copy_fence_counter)
		return;

	if (dev->IsEventLoopDriven())
	{
		// Submit the copy; mapAsync() orders itself after it, so the wait happens in the callback.
		if (dev->GetCurrentFenceCounter() == m_copy_fence_counter)
			dev->ExecuteCommandBuffer(false);
		return;
	}

	if (dev->GetCurrentFenceCounter() == m_copy_fence_counter)
		dev->ExecuteCommandBufferForReadback();
	else
		dev->WaitForFenceCounter(m_copy_fence_counter);
}

#ifdef PCSX2_DEVBUILD

void GSDownloadTextureWebGPU::SetDebugName(std::string_view name)
{
	if (name.empty())
		return;

	wgpuBufferSetLabel(m_buffer, {name.data(), name.size()});
}

#endif
