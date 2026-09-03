// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Null/GSDeviceNull.h"
#include "GS/Renderers/Null/GSTextureNull.h"

GSDeviceNull::GSDeviceNull() = default;

GSDeviceNull::~GSDeviceNull() = default;

bool GSDeviceNull::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	if (!GSDevice::Create(vsync_mode, allow_present_throttle))
		return false;

	if (!AcquireWindow(false))
		return false;

	m_name = "Null";
	m_max_texture_size = 8192;
	m_features = FeatureSupport();
	return true;
}

void GSDeviceNull::Destroy()
{
	GSDevice::Destroy();
	m_window_info = WindowInfo();
}

RenderAPI GSDeviceNull::GetRenderAPI() const
{
	return RenderAPI::None;
}

bool GSDeviceNull::HasSurface() const
{
	return false;
}

void GSDeviceNull::DestroySurface()
{
	m_window_info.type = WindowInfo::Type::Surfaceless;
}

bool GSDeviceNull::UpdateWindow()
{
	return AcquireWindow(false);
}

void GSDeviceNull::ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale)
{
	m_window_info.surface_width = new_window_width;
	m_window_info.surface_height = new_window_height;
	m_window_info.surface_scale = new_window_scale;
}

bool GSDeviceNull::SupportsExclusiveFullscreen() const
{
	return false;
}

GSDevice::PresentResult GSDeviceNull::BeginPresent(bool frame_skip)
{
	return PresentResult::FrameSkipped;
}

void GSDeviceNull::EndPresent()
{
}

void GSDeviceNull::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_vsync_mode = mode;
	m_allow_present_throttle = allow_present_throttle;
}

std::string GSDeviceNull::GetDriverInfo() const
{
	return "Null device";
}

bool GSDeviceNull::SetGPUTimingEnabled(bool enabled)
{
	return false;
}

float GSDeviceNull::GetAndResetAccumulatedGPUTime()
{
	return 0.0f;
}

bool GSDeviceNull::SetGPUPipelineStatisticsEnabled(bool enabled)
{
	return false;
}

GPUPipelineStatistics GSDeviceNull::GetAndResetAccumulatedGPUPipelineStatistics()
{
	return GPUPipelineStatistics{};
}

void GSDeviceNull::PushDebugGroup(const char* fmt, ...)
{
}

void GSDeviceNull::PopDebugGroup()
{
}

void GSDeviceNull::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
}

std::unique_ptr<GSDownloadTexture> GSDeviceNull::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return std::make_unique<GSDownloadTextureNull>(width, height, format);
}

void GSDeviceNull::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
}

void GSDeviceNull::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, PresentShader shader, float shaderTime, Filter filter)
{
}

void GSDeviceNull::DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvertSelector shader)
{
}

void GSDeviceNull::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
{
}

void GSDeviceNull::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
}

void GSDeviceNull::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
{
}

void GSDeviceNull::RenderHW(GSHWDrawConfig& config)
{
}

void GSDeviceNull::ClearSamplerCache()
{
}

void GSDeviceNull::BeginDSAsRT(GSTexture* ds, const GSVector4i& drawarea)
{
}

GSTexture* GSDeviceNull::CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format)
{
	return new GSTextureNull(usage, width, height, levels, format);
}

void GSDeviceNull::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter)
{
}

void GSDeviceNull::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb)
{
}

void GSDeviceNull::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
}

void GSDeviceNull::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
}

bool GSDeviceNull::DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only, const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	return false;
}

void GSDeviceNull::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvertSelector shader, Filter filter)
{
}

void GSDeviceNull::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, const GSVector4& dRect, PresentShader shader, Filter filter)
{
}
