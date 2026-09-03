// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GS.h"
#include "GS/GSGL.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "GS/Renderers/WebGPU/GSDeviceWebGPU.h"
#include "GS/Renderers/Common/GSDevice.h"

#include "Host.h"
#include "ImGui/ImGuiManager.h"

#include "common/BitUtils.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ScopedGuard.h"
#include "common/SmallString.h"
#include "common/StringUtil.h"

#include "fmt/format.h"
#include "imgui.h"

#include <cstdlib>
#include <cstring>

enum : u32
{
	VERTEX_BUFFER_SIZE = 32 * 1024 * 1024,
	INDEX_BUFFER_SIZE = 16 * 1024 * 1024,
	VERTEX_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
	FRAGMENT_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
	TEXTURE_BUFFER_SIZE = 64 * 1024 * 1024,
	MAX_BIND_GROUP_CACHE_SIZE = 8192,
};

static constexpr WGPUStringView StringView(const char* str)
{
	return WGPUStringView{str, WGPU_STRLEN};
}

static std::string_view ToStringView(WGPUStringView sv)
{
	if (!sv.data)
		return {};
	return (sv.length == WGPU_STRLEN) ? std::string_view(sv.data) : std::string_view(sv.data, sv.length);
}

static bool IsDATEModePrimIDInit(u32 flag)
{
	return flag == 1 || flag == 2;
}

static constexpr WGPUColor s_present_clear_color = {0.0, 0.0, 0.0, 1.0};

GSDeviceWebGPU::GSDeviceWebGPU()
{
	std::memset(&m_pipeline_selector, 0, sizeof(m_pipeline_selector));
}

GSDeviceWebGPU::~GSDeviceWebGPU() = default;

//////////////////////////////////////////////////////////////////////////
// Instance / adapter / device
//////////////////////////////////////////////////////////////////////////

WGPUInstance GSDeviceWebGPU::CreateWGPUInstance()
{
	WGPUInstanceDescriptor desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
	static constexpr WGPUInstanceFeatureName required_features[] = {WGPUInstanceFeatureName_TimedWaitAny};
	if (wgpuHasInstanceFeature(WGPUInstanceFeatureName_TimedWaitAny))
	{
		desc.requiredFeatureCount = std::size(required_features);
		desc.requiredFeatures = required_features;
	}

	return wgpuCreateInstance(&desc);
}

void GSDeviceWebGPU::RequestAdapterCallback(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1, void* userdata2)
{
	WGPUAdapter* const out = static_cast<WGPUAdapter*>(userdata1);
	if (status == WGPURequestAdapterStatus_Success)
	{
		*out = adapter;
		return;
	}

	const std::string_view msg = ToStringView(message);
	Console.Error("WebGPU: RequestAdapter failed (%u): %.*s", static_cast<u32>(status), static_cast<int>(msg.size()), msg.data());
}

WGPUAdapter GSDeviceWebGPU::RequestAdapter(WGPUInstance instance, WGPUSurface surface)
{
	WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
	options.featureLevel = WGPUFeatureLevel_Core;
	options.compatibleSurface = surface;

	if (const char* backend = std::getenv("PCSX2_WEBGPU_BACKEND"))
	{
		if (StringUtil::Strcasecmp(backend, "vulkan") == 0)
			options.backendType = WGPUBackendType_Vulkan;
		else if (StringUtil::Strcasecmp(backend, "null") == 0)
			options.backendType = WGPUBackendType_Null;
		else if (StringUtil::Strcasecmp(backend, "d3d12") == 0)
			options.backendType = WGPUBackendType_D3D12;
		else if (StringUtil::Strcasecmp(backend, "d3d11") == 0)
			options.backendType = WGPUBackendType_D3D11;
		else if (StringUtil::Strcasecmp(backend, "metal") == 0)
			options.backendType = WGPUBackendType_Metal;
		else if (StringUtil::Strcasecmp(backend, "opengl") == 0)
			options.backendType = WGPUBackendType_OpenGL;
		else if (StringUtil::Strcasecmp(backend, "opengles") == 0)
			options.backendType = WGPUBackendType_OpenGLES;
	}

	WGPUAdapter adapter = nullptr;
	WGPURequestAdapterCallbackInfo cbi = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
	cbi.mode = WGPUCallbackMode_WaitAnyOnly;
	cbi.callback = &GSDeviceWebGPU::RequestAdapterCallback;
	cbi.userdata1 = &adapter;

	WGPUFutureWaitInfo wait = WGPU_FUTURE_WAIT_INFO_INIT;
	wait.future = wgpuInstanceRequestAdapter(instance, &options, cbi);
	const WGPUWaitStatus wstatus = wgpuInstanceWaitAny(instance, 1, &wait, UINT64_MAX);
	if (wstatus != WGPUWaitStatus_Success)
	{
		Console.Error("WebGPU: Waiting for adapter failed (%u)", static_cast<u32>(wstatus));
		return nullptr;
	}

	return adapter;
}

void GSDeviceWebGPU::RequestDeviceCallback(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void* userdata2)
{
	WGPUDevice* const out = static_cast<WGPUDevice*>(userdata1);
	if (status == WGPURequestDeviceStatus_Success)
	{
		*out = device;
		return;
	}

	const std::string_view msg = ToStringView(message);
	Console.Error("WebGPU: RequestDevice failed (%u): %.*s", static_cast<u32>(status), static_cast<int>(msg.size()), msg.data());
}

void GSDeviceWebGPU::DeviceLostCallback(WGPUDevice const* device, WGPUDeviceLostReason reason, WGPUStringView message, void* userdata1, void* userdata2)
{
	GSDeviceWebGPU* const dev = static_cast<GSDeviceWebGPU*>(userdata1);
	if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_CallbackCancelled)
		return;

	const std::string_view msg = ToStringView(message);
	Console.Error("WebGPU: Device lost (%u): %.*s", static_cast<u32>(reason), static_cast<int>(msg.size()), msg.data());
	if (dev)
		dev->m_device_lost = true;
}

void GSDeviceWebGPU::UncapturedErrorCallback(WGPUDevice const* device, WGPUErrorType type, WGPUStringView message, void* userdata1, void* userdata2)
{
	const std::string_view msg = ToStringView(message);
	Console.Error("WebGPU: Uncaptured error (%u): %.*s", static_cast<u32>(type), static_cast<int>(msg.size()), msg.data());
}

void GSDeviceWebGPU::QueueWorkDoneCallback(WGPUQueueWorkDoneStatus status, WGPUStringView message, void* userdata1, void* userdata2)
{
	GSDeviceWebGPU* const dev = static_cast<GSDeviceWebGPU*>(userdata1);
	const u64 counter = static_cast<u64>(reinterpret_cast<uintptr_t>(userdata2));
	if (status != WGPUQueueWorkDoneStatus_Success)
	{
		const std::string_view msg = ToStringView(message);
		Console.Error("WebGPU: Queue work failed (%u): %.*s", static_cast<u32>(status), static_cast<int>(msg.size()), msg.data());
	}

	dev->m_completed_fence_counter = std::max(dev->m_completed_fence_counter, counter);
	while (!dev->m_pending_submits.empty() && dev->m_pending_submits.front().first <= dev->m_completed_fence_counter)
		dev->m_pending_submits.pop_front();
}

void GSDeviceWebGPU::CompilationInfoCallback(WGPUCompilationInfoRequestStatus status, WGPUCompilationInfo const* info, void* userdata1, void* userdata2)
{
	bool* const has_error = static_cast<bool*>(userdata1);
	const char* label = static_cast<const char*>(userdata2);
	if (status != WGPUCompilationInfoRequestStatus_Success || !info)
		return;

	for (size_t i = 0; i < info->messageCount; i++)
	{
		const WGPUCompilationMessage& msg = info->messages[i];
		const std::string_view text = ToStringView(msg.message);
		if (msg.type == WGPUCompilationMessageType_Error)
		{
			*has_error = true;
			Console.Error("WebGPU: %s:%llu:%llu: error: %.*s", label, static_cast<unsigned long long>(msg.lineNum),
				static_cast<unsigned long long>(msg.linePos), static_cast<int>(text.size()), text.data());
		}
		else if (msg.type == WGPUCompilationMessageType_Warning)
		{
			Console.Warning("WebGPU: %s:%llu:%llu: warning: %.*s", label, static_cast<unsigned long long>(msg.lineNum),
				static_cast<unsigned long long>(msg.linePos), static_cast<int>(text.size()), text.data());
		}
	}
}

bool GSDeviceWebGPU::WaitForFuture(WGPUFuture future)
{
	if (future.id == 0)
		return true;

	WGPUFutureWaitInfo wait = WGPU_FUTURE_WAIT_INFO_INIT;
	wait.future = future;
	const WGPUWaitStatus status = wgpuInstanceWaitAny(m_instance, 1, &wait, UINT64_MAX);
	if (status != WGPUWaitStatus_Success)
	{
		Console.Error("WebGPU: wgpuInstanceWaitAny() failed: %u", static_cast<u32>(status));
		return false;
	}

	return true;
}

void GSDeviceWebGPU::ProcessEvents()
{
	wgpuInstanceProcessEvents(m_instance);
}

void GSDeviceWebGPU::WaitForFenceCounter(u64 fence_counter)
{
	if (m_completed_fence_counter >= fence_counter)
		return;

	ProcessEvents();

	while (m_completed_fence_counter < fence_counter)
	{
		WGPUFuture future = WGPU_FUTURE_INIT;
		for (const auto& [counter, pending_future] : m_pending_submits)
		{
			if (counter >= fence_counter)
			{
				future = pending_future;
				break;
			}
		}

		if (future.id == 0)
		{
			pxAssertMsg(fence_counter < m_current_fence_counter, "Waiting for a fence counter which has not been submitted");
			m_completed_fence_counter = std::max(m_completed_fence_counter, fence_counter);
			break;
		}

		if (!WaitForFuture(future))
		{
			m_device_lost = true;
			m_completed_fence_counter = std::max(m_completed_fence_counter, fence_counter);
			break;
		}
	}
}

void GSDeviceWebGPU::WaitForGPUIdle()
{
	if (m_command_encoder)
		ExecuteCommandBuffer(false);

	if (m_current_fence_counter > 1)
		WaitForFenceCounter(m_current_fence_counter - 1);
}

WGPUCommandEncoder GSDeviceWebGPU::GetCommandEncoder()
{
	if (!m_command_encoder)
	{
		WGPUCommandEncoderDescriptor desc = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
		m_command_encoder = wgpuDeviceCreateCommandEncoder(m_device, &desc);
	}

	return m_command_encoder;
}

void GSDeviceWebGPU::FlushStreamBuffers()
{
	m_vertex_stream_buffer.OnSubmit();
	m_index_stream_buffer.OnSubmit();
	m_expand_index_stream_buffer.OnSubmit();
	m_vertex_uniform_stream_buffer.OnSubmit();
	m_fragment_uniform_stream_buffer.OnSubmit();
	m_texture_stream_buffer.OnSubmit();
}

void GSDeviceWebGPU::SubmitCommandBuffer()
{
	EndRenderPass();

	FlushStreamBuffers();

	if (m_command_encoder)
	{
		WGPUCommandBufferDescriptor desc = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
		WGPUCommandBuffer cmdbuf = wgpuCommandEncoderFinish(m_command_encoder, &desc);
		wgpuCommandEncoderRelease(m_command_encoder);
		m_command_encoder = nullptr;

		if (cmdbuf)
		{
			wgpuQueueSubmit(m_queue, 1, &cmdbuf);
			wgpuCommandBufferRelease(cmdbuf);
		}
	}

	WGPUQueueWorkDoneCallbackInfo cbi = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
	cbi.mode = WGPUCallbackMode_AllowProcessEvents;
	cbi.callback = &GSDeviceWebGPU::QueueWorkDoneCallback;
	cbi.userdata1 = this;
	cbi.userdata2 = reinterpret_cast<void*>(static_cast<uintptr_t>(m_current_fence_counter));
	const WGPUFuture future = wgpuQueueOnSubmittedWorkDone(m_queue, cbi);
	m_pending_submits.emplace_back(m_current_fence_counter, future);
	m_current_fence_counter++;

	ProcessEvents();
}

void GSDeviceWebGPU::MoveToNextCommandBuffer()
{
	InvalidateCachedState();
}

void GSDeviceWebGPU::ExecuteCommandBuffer(bool wait_for_completion)
{
	if (m_device_lost)
		return;

	const u64 counter = m_current_fence_counter;
	SubmitCommandBuffer();
	MoveToNextCommandBuffer();

	if (wait_for_completion)
		WaitForFenceCounter(counter);

	m_dirty_flags |= DIRTY_FLAG_VS_PUSH_CONSTANTS;
}

void GSDeviceWebGPU::ExecuteCommandBuffer(bool wait_for_completion, const char* reason, ...)
{
	std::va_list ap;
	va_start(ap, reason);
	const std::string reason_str(StringUtil::StdStringFromFormatV(reason, ap));
	va_end(ap);

	Console.Warning("WebGPU: Executing command buffer due to '%s'", reason_str.c_str());
	ExecuteCommandBuffer(wait_for_completion);
}

void GSDeviceWebGPU::ExecuteCommandBufferAndRestartRenderPass(bool wait_for_completion, const char* reason)
{
	Console.Warning("WebGPU: Executing command buffer due to '%s'", reason);

	const bool was_in_render_pass = InRenderPass();
	GSTextureWebGPU* const rt = m_pass_render_target;
	GSTextureWebGPU* const ds = m_pass_depth_target;
	GSTextureWebGPU* const current_rt = m_current_render_target;
	GSTextureWebGPU* const current_ds = m_current_depth_target;
	const GSVector4i scissor = m_scissor;
	const GSVector4i viewport = m_viewport;
	const bool presenting = m_is_presenting;

	ExecuteCommandBuffer(wait_for_completion);

	if (presenting)
	{
		RenderPassLoadOps ops;
		BeginRenderPass(m_surface_texture.get(), nullptr, ops);
		m_is_presenting = true;
		m_viewport = viewport;
		m_scissor = scissor;
		m_dirty_flags |= DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR;
		return;
	}

	m_current_render_target = current_rt;
	m_current_depth_target = current_ds;
	m_scissor = scissor;
	m_viewport = viewport;
	if (was_in_render_pass && (rt || ds))
		BeginRenderPass(rt, ds, GetLoadOpsForTargets(rt, ds, false));
}

void GSDeviceWebGPU::ExecuteCommandBufferForReadback()
{
	ExecuteCommandBuffer(true);
}

//////////////////////////////////////////////////////////////////////////
// Adapter enumeration
//////////////////////////////////////////////////////////////////////////

std::vector<GSAdapterInfo> GSDeviceWebGPU::GetAdapterInfo()
{
	std::vector<GSAdapterInfo> ret;

	WGPUInstance instance = CreateWGPUInstance();
	if (!instance)
		return ret;

	WGPUAdapter adapter = RequestAdapter(instance, nullptr);
	if (adapter)
	{
		WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
		WGPULimits limits = WGPU_LIMITS_INIT;
		if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success && wgpuAdapterGetLimits(adapter, &limits) == WGPUStatus_Success)
		{
			GSAdapterInfo ai;
			ai.name = std::string(ToStringView(info.device));
			ai.max_texture_size = limits.maxTextureDimension2D;
			ai.max_upscale_multiplier = GSGetMaxUpscaleMultiplier(ai.max_texture_size);
			ret.push_back(std::move(ai));
			wgpuAdapterInfoFreeMembers(info);
		}

		wgpuAdapterRelease(adapter);
	}

	wgpuInstanceRelease(instance);
	return ret;
}

RenderAPI GSDeviceWebGPU::GetRenderAPI() const
{
	return RenderAPI::WebGPU;
}

bool GSDeviceWebGPU::HasSurface() const
{
	return (m_surface != nullptr);
}

//////////////////////////////////////////////////////////////////////////
// Create / Destroy
//////////////////////////////////////////////////////////////////////////

bool GSDeviceWebGPU::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	if (!GSDevice::Create(vsync_mode, allow_present_throttle))
		return false;

	if (!CreateDeviceAndSurface())
		return false;

	if (!CheckFeatures())
	{
		Host::ReportErrorAsync("GS", "Your GPU does not support the required WebGPU features.");
		return false;
	}

	{
		std::optional<std::string> shader = ReadShaderSource("shaders/webgpu/tfx.wgsl");
		if (!shader.has_value())
		{
			Host::ReportErrorAsync("GS", "Failed to read shaders/webgpu/tfx.wgsl.");
			return false;
		}

		m_tfx_source = std::move(*shader);
	}

	if (!CreateBuffers())
		return false;

	if (!CreateLayouts())
	{
		Host::ReportErrorAsync("GS", "Failed to create pipeline layouts");
		return false;
	}

	if (!CreateNullTexture())
	{
		Host::ReportErrorAsync("GS", "Failed to create dummy texture");
		return false;
	}

	if (!CreateSamplers())
		return false;

	if (!CompileConvertPipelines() || !CompilePresentPipelines() || !CompileInterlacePipelines() ||
		!CompileMergePipelines() || !CompilePostProcessingPipelines())
	{
		Host::ReportErrorAsync("GS", "Failed to compile utility pipelines");
		return false;
	}

	if (!CompileImGuiPipeline())
		return false;

	InvalidateCachedState();
	return true;
}

void GSDeviceWebGPU::Destroy()
{
	GSDevice::Destroy();

	if (m_device)
	{
		EndRenderPass();
		WaitForGPUIdle();
	}

	DestroySurface();
	DestroyResources();

	if (m_queue)
	{
		wgpuQueueRelease(m_queue);
		m_queue = nullptr;
	}
	if (m_device)
	{
		wgpuDeviceRelease(m_device);
		m_device = nullptr;
	}
	if (m_adapter)
	{
		wgpuAdapterRelease(m_adapter);
		m_adapter = nullptr;
	}
	if (m_instance)
	{
		wgpuInstanceRelease(m_instance);
		m_instance = nullptr;
	}
}

bool GSDeviceWebGPU::CreateDeviceAndSurface()
{
	if (!AcquireWindow(true))
		return false;

	m_instance = CreateWGPUInstance();
	if (!m_instance)
	{
		Host::ReportErrorAsync("Error", "Failed to create WebGPU instance.");
		return false;
	}

	m_timed_wait_any = wgpuHasInstanceFeature(WGPUInstanceFeatureName_TimedWaitAny);

	if (m_window_info.type != WindowInfo::Type::Surfaceless && !CreateSurface())
		return false;

	m_adapter = RequestAdapter(m_instance, m_surface);
	if (!m_adapter)
	{
		Host::ReportErrorAsync("Error", "No WebGPU adapter available.");
		return false;
	}

	WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
	if (wgpuAdapterGetInfo(m_adapter, &info) == WGPUStatus_Success)
	{
		m_adapter_name = std::string(ToStringView(info.device));
		m_adapter_description = std::string(ToStringView(info.description));
		m_backend_type = info.backendType;
		wgpuAdapterInfoFreeMembers(info);
	}
	m_name = m_adapter_name;

	WGPULimits adapter_limits = WGPU_LIMITS_INIT;
	if (wgpuAdapterGetLimits(m_adapter, &adapter_limits) != WGPUStatus_Success)
	{
		Host::ReportErrorAsync("Error", "Failed to query WebGPU adapter limits.");
		return false;
	}

	m_device_features.depth32float_stencil8 = wgpuAdapterHasFeature(m_adapter, WGPUFeatureName_Depth32FloatStencil8);
	m_device_features.dual_source_blending = wgpuAdapterHasFeature(m_adapter, WGPUFeatureName_DualSourceBlending);
	m_device_features.primitive_index = wgpuAdapterHasFeature(m_adapter, WGPUFeatureName_PrimitiveIndex);
	m_device_features.texture_formats_tier1 = wgpuAdapterHasFeature(m_adapter, WGPUFeatureName_TextureFormatsTier1);
	m_device_features.float32_blendable = wgpuAdapterHasFeature(m_adapter, WGPUFeatureName_Float32Blendable);
	m_device_features.float32_filterable = wgpuAdapterHasFeature(m_adapter, WGPUFeatureName_Float32Filterable);
	m_device_features.texture_compression_bc = wgpuAdapterHasFeature(m_adapter, WGPUFeatureName_TextureCompressionBC);
	m_device_features.timestamp_query = wgpuAdapterHasFeature(m_adapter, WGPUFeatureName_TimestampQuery);

	if (const char* disabled = std::getenv("PCSX2_WEBGPU_DISABLE_FEATURES"))
	{
		const std::string_view list(disabled);
		m_device_features.dual_source_blending &= (list.find("dsb") == std::string_view::npos);
		m_device_features.primitive_index &= (list.find("primid") == std::string_view::npos);
		m_device_features.depth32float_stencil8 &= (list.find("stencil") == std::string_view::npos);
		m_device_features.texture_formats_tier1 &= (list.find("tier1") == std::string_view::npos);
		m_device_features.texture_compression_bc &= (list.find("bc") == std::string_view::npos);
	}

	std::vector<WGPUFeatureName> required_features;
	if (m_device_features.depth32float_stencil8)
		required_features.push_back(WGPUFeatureName_Depth32FloatStencil8);
	if (m_device_features.dual_source_blending)
		required_features.push_back(WGPUFeatureName_DualSourceBlending);
	if (m_device_features.primitive_index)
		required_features.push_back(WGPUFeatureName_PrimitiveIndex);
	if (m_device_features.texture_formats_tier1)
		required_features.push_back(WGPUFeatureName_TextureFormatsTier1);
	if (m_device_features.float32_blendable)
		required_features.push_back(WGPUFeatureName_Float32Blendable);
	if (m_device_features.float32_filterable)
		required_features.push_back(WGPUFeatureName_Float32Filterable);
	if (m_device_features.texture_compression_bc)
		required_features.push_back(WGPUFeatureName_TextureCompressionBC);

	WGPULimits required_limits = WGPU_LIMITS_INIT;
	required_limits.maxTextureDimension2D = adapter_limits.maxTextureDimension2D;
	required_limits.maxBufferSize = adapter_limits.maxBufferSize;
	required_limits.maxStorageBufferBindingSize = adapter_limits.maxStorageBufferBindingSize;
	required_limits.maxColorAttachmentBytesPerSample = adapter_limits.maxColorAttachmentBytesPerSample;

	WGPUDeviceDescriptor desc = WGPU_DEVICE_DESCRIPTOR_INIT;
	desc.label = StringView("PCSX2");
	desc.requiredFeatureCount = required_features.size();
	desc.requiredFeatures = required_features.data();
	desc.requiredLimits = &required_limits;
	desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
	desc.deviceLostCallbackInfo.callback = &GSDeviceWebGPU::DeviceLostCallback;
	desc.deviceLostCallbackInfo.userdata1 = this;
	desc.uncapturedErrorCallbackInfo.callback = &GSDeviceWebGPU::UncapturedErrorCallback;
	desc.uncapturedErrorCallbackInfo.userdata1 = this;

	WGPURequestDeviceCallbackInfo cbi = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
	cbi.mode = WGPUCallbackMode_WaitAnyOnly;
	cbi.callback = &GSDeviceWebGPU::RequestDeviceCallback;
	cbi.userdata1 = &m_device;

	if (!WaitForFuture(wgpuAdapterRequestDevice(m_adapter, &desc, cbi)) || !m_device)
	{
		Host::ReportErrorAsync("Error", "Failed to create WebGPU device.");
		return false;
	}

	if (wgpuDeviceGetLimits(m_device, &m_limits) != WGPUStatus_Success)
		m_limits = required_limits;

	m_queue = wgpuDeviceGetQueue(m_device);
	if (!m_queue)
		return false;

	INFO_LOG("WebGPU: Using adapter '{}' ({}), backend {}", m_adapter_name, m_adapter_description, static_cast<u32>(m_backend_type));

	if (m_surface && !ConfigureSurface())
		return false;

	return true;
}

bool GSDeviceWebGPU::CreateSurface()
{
	WGPUSurfaceDescriptor desc = WGPU_SURFACE_DESCRIPTOR_INIT;
	desc.label = StringView("PCSX2 Surface");

	WGPUSurfaceSourceXlibWindow xlib = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
	WGPUSurfaceSourceWaylandSurface wayland = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
	WGPUSurfaceSourceWindowsHWND hwnd = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
	WGPUSurfaceSourceMetalLayer metal = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;

	switch (m_window_info.type)
	{
		case WindowInfo::Type::X11:
			xlib.display = m_window_info.display_connection;
			xlib.window = reinterpret_cast<uintptr_t>(m_window_info.window_handle);
			desc.nextInChain = &xlib.chain;
			break;

		case WindowInfo::Type::Wayland:
			wayland.display = m_window_info.display_connection;
			wayland.surface = m_window_info.window_handle;
			desc.nextInChain = &wayland.chain;
			break;

		case WindowInfo::Type::Win32:
			hwnd.hwnd = m_window_info.window_handle;
			desc.nextInChain = &hwnd.chain;
			break;

		case WindowInfo::Type::MacOS:
			metal.layer = m_window_info.surface_handle;
			desc.nextInChain = &metal.chain;
			break;

		default:
			Console.Error("WebGPU: Unsupported window type %u", static_cast<u32>(m_window_info.type));
			return false;
	}

	m_surface = wgpuInstanceCreateSurface(m_instance, &desc);
	if (!m_surface)
	{
		Console.Error("WebGPU: Failed to create surface");
		return false;
	}

	return true;
}

bool GSDeviceWebGPU::ConfigureSurface()
{
	WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
	if (wgpuSurfaceGetCapabilities(m_surface, m_adapter, &caps) != WGPUStatus_Success || caps.formatCount == 0)
	{
		Console.Error("WebGPU: Failed to query surface capabilities");
		return false;
	}

	m_surface_format = caps.formats[0];
	for (size_t i = 0; i < caps.formatCount; i++)
	{
		if (caps.formats[i] == WGPUTextureFormat_RGBA8Unorm || caps.formats[i] == WGPUTextureFormat_BGRA8Unorm)
		{
			m_surface_format = caps.formats[i];
			break;
		}
	}

	WGPUPresentMode wanted_mode = WGPUPresentMode_Fifo;
	if (m_vsync_mode == GSVSyncMode::Disabled)
		wanted_mode = WGPUPresentMode_Immediate;
	else if (m_vsync_mode == GSVSyncMode::Mailbox)
		wanted_mode = WGPUPresentMode_Mailbox;

	m_present_mode = WGPUPresentMode_Fifo;
	for (size_t i = 0; i < caps.presentModeCount; i++)
	{
		if (caps.presentModes[i] == wanted_mode)
		{
			m_present_mode = wanted_mode;
			break;
		}
	}

	WGPUCompositeAlphaMode alpha_mode = WGPUCompositeAlphaMode_Auto;
	for (size_t i = 0; i < caps.alphaModeCount; i++)
	{
		if (caps.alphaModes[i] == WGPUCompositeAlphaMode_Opaque)
		{
			alpha_mode = WGPUCompositeAlphaMode_Opaque;
			break;
		}
	}

	wgpuSurfaceCapabilitiesFreeMembers(caps);

	WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
	config.device = m_device;
	config.format = m_surface_format;
	config.usage = WGPUTextureUsage_RenderAttachment;
	config.width = std::max<u32>(m_window_info.surface_width, 1);
	config.height = std::max<u32>(m_window_info.surface_height, 1);
	config.alphaMode = alpha_mode;
	config.presentMode = m_present_mode;
	wgpuSurfaceConfigure(m_surface, &config);
	m_surface_configured = true;
	return true;
}

bool GSDeviceWebGPU::CheckFeatures()
{
	const DeviceFeatures& df = m_device_features;

	m_features.broken_point_sampler = false;
	m_features.vs_expand = !GSConfig.DisableVertexShaderExpand;
	m_features.primitive_id = df.primitive_index && df.float32_blendable;
	m_features.texture_barrier = false;
	m_features.multidraw_fb_copy = GSConfig.OverrideTextureBarriers != 0;
	m_features.provoking_vertex_last = false;
	m_features.point_expand = false;
	m_features.line_expand = false;
	m_features.prefer_new_textures = true;
	m_features.dxt_textures = df.texture_compression_bc;
	m_features.bptc_textures = df.texture_compression_bc;
	m_features.framebuffer_fetch = false;
	m_features.stencil_buffer = df.depth32float_stencil8;
	m_features.cas_sharpening = false;
	m_features.test_and_sample_depth = false;
	m_features.depth_feedback = m_features.multidraw_fb_copy && GSConfig.DepthFeedbackMode == GSDepthFeedbackMode::Depth;
	m_features.aa1 = GSConfig.HWAA1 && m_features.vs_expand && m_features.feedback_loops();
	m_features.rov = false;
	m_features.dual_source_blend = df.dual_source_blending;

	m_max_texture_size = m_limits.maxTextureDimension2D;

	if (!m_features.multidraw_fb_copy && !m_features.stencil_buffer)
	{
		Host::AddKeyedOSDMessage("GSDeviceWebGPU_NoTextureBarrierOrStencilBuffer",
			TRANSLATE_STR("GS", "Stencil buffers and texture barriers are both unavailable, this will break some graphical effects."),
			Host::OSD_WARNING_DURATION);
	}

	Console.WriteLnFmt("WebGPU: depth32float-stencil8: {}", df.depth32float_stencil8 ? "yes" : "no");
	Console.WriteLnFmt("WebGPU: dual-source-blending: {}", df.dual_source_blending ? "yes" : "no");
	Console.WriteLnFmt("WebGPU: primitive-index: {}", df.primitive_index ? "yes" : "no");
	Console.WriteLnFmt("WebGPU: texture-formats-tier1: {}", df.texture_formats_tier1 ? "yes" : "no");
	Console.WriteLnFmt("WebGPU: float32-blendable: {}", df.float32_blendable ? "yes" : "no");
	Console.WriteLnFmt("WebGPU: float32-filterable: {}", df.float32_filterable ? "yes" : "no");
	Console.WriteLnFmt("WebGPU: texture-compression-bc: {}", df.texture_compression_bc ? "yes" : "no");
	Console.WriteLnFmt("WebGPU: Features: vs_expand={} primitive_id={} multidraw_fb_copy={} stencil_buffer={} dual_source_blend={} depth_feedback={} aa1={}",
		static_cast<bool>(m_features.vs_expand), static_cast<bool>(m_features.primitive_id), static_cast<bool>(m_features.multidraw_fb_copy),
		static_cast<bool>(m_features.stencil_buffer), static_cast<bool>(m_features.dual_source_blend), static_cast<bool>(m_features.depth_feedback),
		static_cast<bool>(m_features.aa1));

	return true;
}

WGPUTextureFormat GSDeviceWebGPU::LookupNativeFormat(GSTexture::Format format) const
{
	switch (format)
	{
		case GSTexture::Format::Color:
			return WGPUTextureFormat_RGBA8Unorm;
		case GSTexture::Format::ColorHQ:
			return WGPUTextureFormat_RGB10A2Unorm;
		case GSTexture::Format::ColorHDR:
			return WGPUTextureFormat_RGBA16Float;
		case GSTexture::Format::ColorClip:
			return m_device_features.texture_formats_tier1 ? WGPUTextureFormat_RGBA16Unorm : WGPUTextureFormat_RGBA16Float;
		case GSTexture::Format::DepthStencil:
			return m_device_features.depth32float_stencil8 ? WGPUTextureFormat_Depth32FloatStencil8 : WGPUTextureFormat_Depth32Float;
		case GSTexture::Format::DepthColor:
			return WGPUTextureFormat_R32Float;
		case GSTexture::Format::UNorm8:
			return WGPUTextureFormat_R8Unorm;
		case GSTexture::Format::UInt16:
			return WGPUTextureFormat_R16Uint;
		case GSTexture::Format::UInt32:
			return WGPUTextureFormat_R32Uint;
		case GSTexture::Format::PrimID:
			return WGPUTextureFormat_R32Float;
		case GSTexture::Format::BC1:
			return WGPUTextureFormat_BC1RGBAUnorm;
		case GSTexture::Format::BC2:
			return WGPUTextureFormat_BC2RGBAUnorm;
		case GSTexture::Format::BC3:
			return WGPUTextureFormat_BC3RGBAUnorm;
		case GSTexture::Format::BC7:
			return WGPUTextureFormat_BC7RGBAUnorm;
		default:
			return WGPUTextureFormat_Undefined;
	}
}

u32 GSDeviceWebGPU::GetBytesPerTexel(GSTexture::Format format)
{
	return GSTexture::GetCompressedBytesPerBlock(format);
}

bool GSDeviceWebGPU::CreateNullTexture()
{
	m_null_texture = GSTextureWebGPU::Create(GSTexture::RenderTarget, GSTexture::Format::Color, 1, 1, 1);
	if (!m_null_texture)
		return false;

	const u32 zero = 0;
	m_null_texture->Update(GSVector4i(0, 0, 1, 1), &zero, sizeof(zero), 0);
	m_null_texture->SetState(GSTexture::State::Dirty);
	return true;
}

bool GSDeviceWebGPU::CreateBuffers()
{
	if (!m_vertex_stream_buffer.Create(m_device, m_queue, WGPUBufferUsage_Vertex | WGPUBufferUsage_Storage, VERTEX_BUFFER_SIZE, "Vertex Stream Buffer"))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate vertex buffer");
		return false;
	}

	if (!m_index_stream_buffer.Create(m_device, m_queue, WGPUBufferUsage_Index, INDEX_BUFFER_SIZE, "Index Stream Buffer"))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate index buffer");
		return false;
	}

	if (!m_expand_index_stream_buffer.Create(m_device, m_queue, WGPUBufferUsage_Storage, m_features.aa1 ? INDEX_BUFFER_SIZE : 256, "Expand Index Stream Buffer"))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate expansion index buffer (VS resource)");
		return false;
	}

	if (!m_vertex_uniform_stream_buffer.Create(m_device, m_queue, WGPUBufferUsage_Uniform, VERTEX_UNIFORM_BUFFER_SIZE, "Vertex Uniform Stream Buffer"))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate vertex uniform buffer");
		return false;
	}

	if (!m_fragment_uniform_stream_buffer.Create(m_device, m_queue, WGPUBufferUsage_Uniform, FRAGMENT_UNIFORM_BUFFER_SIZE, "Fragment Uniform Stream Buffer"))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate fragment uniform buffer");
		return false;
	}

	if (!m_texture_stream_buffer.Create(m_device, m_queue, WGPUBufferUsage_CopySrc, TEXTURE_BUFFER_SIZE, "Texture Stream Buffer"))
	{
		Host::ReportErrorAsync("GS", "Failed to allocate texture upload buffer");
		return false;
	}

	{
		WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
		desc.label = StringView("Expand Index Buffer");
		desc.usage = WGPUBufferUsage_Index;
		desc.size = EXPAND_BUFFER_SIZE;
		desc.mappedAtCreation = WGPU_TRUE;
		m_expand_index_buffer = wgpuDeviceCreateBuffer(m_device, &desc);
		if (!m_expand_index_buffer)
		{
			Host::ReportErrorAsync("GS", "Failed to allocate expansion index buffer");
			return false;
		}

		void* ptr = wgpuBufferGetMappedRange(m_expand_index_buffer, 0, EXPAND_BUFFER_SIZE);
		if (!ptr)
		{
			Host::ReportErrorAsync("GS", "Failed to map expansion index buffer");
			return false;
		}

		GenerateExpansionIndexBuffer(ptr);
		wgpuBufferUnmap(m_expand_index_buffer);
	}

	return true;
}

bool GSDeviceWebGPU::CreateLayouts()
{
	{
		std::array<WGPUBindGroupLayoutEntry, 5> entries = {};
		for (WGPUBindGroupLayoutEntry& entry : entries)
			entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;

		entries[0].binding = TFX_UBO_BINDING_VS;
		entries[0].visibility = WGPUShaderStage_Vertex;
		entries[0].buffer.type = WGPUBufferBindingType_Uniform;
		entries[0].buffer.hasDynamicOffset = WGPU_TRUE;
		entries[0].buffer.minBindingSize = sizeof(GSHWDrawConfig::VSConstantBuffer);

		entries[1].binding = TFX_UBO_BINDING_PS;
		entries[1].visibility = WGPUShaderStage_Fragment;
		entries[1].buffer.type = WGPUBufferBindingType_Uniform;
		entries[1].buffer.hasDynamicOffset = WGPU_TRUE;
		entries[1].buffer.minBindingSize = sizeof(GSHWDrawConfig::PSConstantBuffer);

		entries[2].binding = TFX_UBO_BINDING_VERTEX_STORAGE;
		entries[2].visibility = WGPUShaderStage_Vertex;
		entries[2].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;

		entries[3].binding = TFX_UBO_BINDING_INDEX_STORAGE;
		entries[3].visibility = WGPUShaderStage_Vertex;
		entries[3].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;

		entries[4].binding = TFX_UBO_BINDING_VS_PUSH;
		entries[4].visibility = WGPUShaderStage_Vertex;
		entries[4].buffer.type = WGPUBufferBindingType_Uniform;
		entries[4].buffer.hasDynamicOffset = WGPU_TRUE;
		entries[4].buffer.minBindingSize = sizeof(GSHWDrawConfig::VSPushConstants);

		WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		desc.label = StringView("TFX UBO layout");
		desc.entryCount = entries.size();
		desc.entries = entries.data();
		m_tfx_ubo_bind_group_layout = wgpuDeviceCreateBindGroupLayout(m_device, &desc);
		if (!m_tfx_ubo_bind_group_layout)
			return false;
	}

	for (u32 layout = 0; layout < NUM_TFX_TEXTURE_LAYOUTS; layout++)
	{
		const bool depth_texture = (layout != 0);
		std::array<WGPUBindGroupLayoutEntry, 6> entries = {};
		for (WGPUBindGroupLayoutEntry& entry : entries)
			entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;

		entries[0].binding = 0;
		entries[0].visibility = WGPUShaderStage_Fragment;
		entries[0].texture.sampleType = depth_texture ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
		entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;

		entries[1].binding = 1;
		entries[1].visibility = WGPUShaderStage_Fragment;
		entries[1].sampler.type = depth_texture ? WGPUSamplerBindingType_NonFiltering : WGPUSamplerBindingType_Filtering;

		for (u32 i = 2; i < 6; i++)
		{
			entries[i].binding = i;
			entries[i].visibility = WGPUShaderStage_Fragment;
			entries[i].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
			entries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
		}

		WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		desc.label = StringView(depth_texture ? "TFX texture layout (depth)" : "TFX texture layout");
		desc.entryCount = entries.size();
		desc.entries = entries.data();
		m_tfx_texture_bind_group_layouts[layout] = wgpuDeviceCreateBindGroupLayout(m_device, &desc);
		if (!m_tfx_texture_bind_group_layouts[layout])
			return false;

		const WGPUBindGroupLayout layouts[2] = {m_tfx_ubo_bind_group_layout, m_tfx_texture_bind_group_layouts[layout]};
		WGPUPipelineLayoutDescriptor pl_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pl_desc.label = StringView("TFX pipeline layout");
		pl_desc.bindGroupLayoutCount = std::size(layouts);
		pl_desc.bindGroupLayouts = layouts;
		m_tfx_pipeline_layouts[layout] = wgpuDeviceCreatePipelineLayout(m_device, &pl_desc);
		if (!m_tfx_pipeline_layouts[layout])
			return false;
	}

	for (u32 layout = 0; layout < NUM_UTILITY_LAYOUTS; layout++)
	{
		const bool depth_texture = (layout != 0);
		std::array<WGPUBindGroupLayoutEntry, 3> entries = {};
		for (WGPUBindGroupLayoutEntry& entry : entries)
			entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;

		entries[0].binding = 0;
		entries[0].visibility = WGPUShaderStage_Fragment;
		entries[0].texture.sampleType = depth_texture ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
		entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;

		entries[1].binding = 1;
		entries[1].visibility = WGPUShaderStage_Fragment;
		entries[1].sampler.type = depth_texture ? WGPUSamplerBindingType_NonFiltering : WGPUSamplerBindingType_Filtering;

		entries[2].binding = 2;
		entries[2].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		entries[2].buffer.type = WGPUBufferBindingType_Uniform;
		entries[2].buffer.hasDynamicOffset = WGPU_TRUE;
		entries[2].buffer.minBindingSize = UTILITY_UNIFORM_SIZE;

		WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		desc.label = StringView(depth_texture ? "Utility layout (depth)" : "Utility layout");
		desc.entryCount = entries.size();
		desc.entries = entries.data();
		m_utility_bind_group_layouts[layout] = wgpuDeviceCreateBindGroupLayout(m_device, &desc);
		if (!m_utility_bind_group_layouts[layout])
			return false;

		WGPUPipelineLayoutDescriptor pl_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pl_desc.label = StringView("Utility pipeline layout");
		pl_desc.bindGroupLayoutCount = 1;
		pl_desc.bindGroupLayouts = &m_utility_bind_group_layouts[layout];
		m_utility_pipeline_layouts[layout] = wgpuDeviceCreatePipelineLayout(m_device, &pl_desc);
		if (!m_utility_pipeline_layouts[layout])
			return false;
	}

	{
		std::array<WGPUBindGroupEntry, 5> entries = {};
		for (WGPUBindGroupEntry& entry : entries)
			entry = WGPU_BIND_GROUP_ENTRY_INIT;

		entries[0].binding = TFX_UBO_BINDING_VS;
		entries[0].buffer = m_vertex_uniform_stream_buffer.GetBuffer();
		entries[0].offset = 0;
		entries[0].size = sizeof(GSHWDrawConfig::VSConstantBuffer);
		entries[1].binding = TFX_UBO_BINDING_PS;
		entries[1].buffer = m_fragment_uniform_stream_buffer.GetBuffer();
		entries[1].offset = 0;
		entries[1].size = sizeof(GSHWDrawConfig::PSConstantBuffer);
		entries[2].binding = TFX_UBO_BINDING_VERTEX_STORAGE;
		entries[2].buffer = m_vertex_stream_buffer.GetBuffer();
		entries[2].offset = 0;
		entries[2].size = VERTEX_BUFFER_SIZE;
		entries[3].binding = TFX_UBO_BINDING_INDEX_STORAGE;
		entries[3].buffer = m_expand_index_stream_buffer.GetBuffer();
		entries[3].offset = 0;
		entries[3].size = m_expand_index_stream_buffer.GetCurrentSize();
		entries[4].binding = TFX_UBO_BINDING_VS_PUSH;
		entries[4].buffer = m_vertex_uniform_stream_buffer.GetBuffer();
		entries[4].offset = 0;
		entries[4].size = sizeof(GSHWDrawConfig::VSPushConstants);

		WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		desc.label = StringView("TFX UBO bind group");
		desc.layout = m_tfx_ubo_bind_group_layout;
		desc.entryCount = entries.size();
		desc.entries = entries.data();
		m_tfx_ubo_bind_group = wgpuDeviceCreateBindGroup(m_device, &desc);
		if (!m_tfx_ubo_bind_group)
			return false;
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// Samplers
//////////////////////////////////////////////////////////////////////////

WGPUSampler GSDeviceWebGPU::GetSampler(GSHWDrawConfig::SamplerSelector ss)
{
	const auto it = m_samplers.find(ss.key);
	if (it != m_samplers.end())
		return it->second;

	WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
	desc.addressModeU = ss.tau ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
	desc.addressModeV = ss.tav ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
	desc.addressModeW = WGPUAddressMode_ClampToEdge;
	desc.magFilter = ss.IsMagFilterLinear() ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	desc.minFilter = ss.IsMinFilterLinear() ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	desc.mipmapFilter = ss.IsMipFilterLinear() ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
	desc.lodMinClamp = 0.0f;
	desc.lodMaxClamp = (ss.lodclamp || !ss.UseMipmapFiltering()) ? 0.25f : 32.0f;
	desc.compare = WGPUCompareFunction_Undefined;
	desc.maxAnisotropy = 1;

	WGPUSampler sampler = wgpuDeviceCreateSampler(m_device, &desc);
	if (!sampler)
		Console.Error("WebGPU: Failed to create sampler %02X", ss.key);

	m_samplers.emplace(ss.key, sampler);
	return sampler;
}

bool GSDeviceWebGPU::CreateSamplers()
{
	m_point_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Point());
	m_linear_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Linear());
	if (!m_point_sampler || !m_linear_sampler)
		return false;

	m_tfx_sampler_sel = GSHWDrawConfig::SamplerSelector::Point().key;
	m_tfx_sampler = m_point_sampler;
	m_utility_sampler = m_point_sampler;
	return true;
}

void GSDeviceWebGPU::ClearSamplerCache()
{
	ExecuteCommandBuffer(true);
	m_tfx_bind_groups.clear();
	m_utility_bind_groups.clear();
	for (const auto& it : m_samplers)
	{
		if (it.second)
			wgpuSamplerRelease(it.second);
	}
	m_samplers.clear();
	m_point_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Point());
	m_linear_sampler = GetSampler(GSHWDrawConfig::SamplerSelector::Linear());
	m_utility_sampler = m_point_sampler;
	m_tfx_sampler = m_point_sampler;
	m_tfx_sampler_sel = GSHWDrawConfig::SamplerSelector::Point().key;
	m_dirty_flags |= DIRTY_FLAG_TFX_TEXTURES | DIRTY_FLAG_UTILITY_TEXTURE;
}

//////////////////////////////////////////////////////////////////////////
// Shaders
//////////////////////////////////////////////////////////////////////////

WGPUShaderModule GSDeviceWebGPU::CreateShaderModule(const std::string& source, const char* label)
{
	WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
	wgsl.code = {source.data(), source.size()};

	WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
	desc.nextInChain = &wgsl.chain;
	desc.label = StringView(label);

	WGPUShaderModule mod = wgpuDeviceCreateShaderModule(m_device, &desc);
	if (!mod)
	{
		Console.Error("WebGPU: wgpuDeviceCreateShaderModule() failed for %s", label);
		return nullptr;
	}

	bool has_error = false;
	WGPUCompilationInfoCallbackInfo cbi = WGPU_COMPILATION_INFO_CALLBACK_INFO_INIT;
	cbi.mode = WGPUCallbackMode_WaitAnyOnly;
	cbi.callback = &GSDeviceWebGPU::CompilationInfoCallback;
	cbi.userdata1 = &has_error;
	cbi.userdata2 = const_cast<char*>(label);
	WaitForFuture(wgpuShaderModuleGetCompilationInfo(mod, cbi));
	if (has_error)
	{
		Console.Error("WebGPU: Shader %s failed to compile", label);
		if (GSConfig.UseDebugDevice || std::getenv("PCSX2_WEBGPU_DUMP_SHADERS"))
		{
			static u32 dump_index = 0;
			const std::string path = Path::Combine(EmuFolders::Logs, fmt::format("webgpu_shader_fail_{}_{}.wgsl", dump_index++, label));
			FileSystem::WriteStringToFile(path.c_str(), source);
		}
		wgpuShaderModuleRelease(mod);
		return nullptr;
	}

	return mod;
}

void GSDeviceWebGPU::AddTFXVertexShaderMacros(WGSLPreprocessor& pp, GSHWDrawConfig::VSSelector sel, bool provoking_vertex_last)
{
	pp.Define("VERTEX_SHADER", 1);
	pp.Define("VS_TME", sel.tme);
	pp.Define("VS_FST", sel.fst);
	pp.Define("VS_IIP", sel.iip);
	pp.Define("VS_POINT_SIZE", sel.point_size);
	pp.Define("VS_EXPAND", static_cast<s64>(sel.expand));
	pp.Define("VS_PROVOKING_VERTEX_LAST", static_cast<s64>(provoking_vertex_last));
}

void GSDeviceWebGPU::AddTFXFragmentShaderMacros(WGSLPreprocessor& pp, const GSHWDrawConfig::PSSelector& sel)
{
	pp.Define("FRAGMENT_SHADER", 1);
	pp.Define("PS_FST", sel.fst);
	pp.Define("PS_WMS", sel.wms);
	pp.Define("PS_WMT", sel.wmt);
	pp.Define("PS_ADJS", sel.adjs);
	pp.Define("PS_ADJT", sel.adjt);
	pp.Define("PS_AEM_FMT", sel.aem_fmt);
	pp.Define("PS_PAL_FMT", sel.pal_fmt);
	pp.Define("PS_DST_FMT", sel.dst_fmt);
	pp.Define("PS_DEPTH_FMT", sel.depth_fmt);
	pp.Define("PS_CHANNEL_FETCH", sel.channel);
	pp.Define("PS_URBAN_CHAOS_HLE", sel.urban_chaos_hle);
	pp.Define("PS_TALES_OF_ABYSS_HLE", sel.tales_of_abyss_hle);
	pp.Define("PS_AEM", sel.aem);
	pp.Define("PS_TFX", sel.tfx);
	pp.Define("PS_TCC", sel.tcc);
	pp.Define("PS_ATST", static_cast<s64>(sel.atst));
	pp.Define("PS_AFAIL", static_cast<s64>(sel.afail));
	pp.Define("PS_FOG", sel.fog);
	pp.Define("PS_BLEND_HW", sel.blend_hw);
	pp.Define("PS_A_MASKED", sel.a_masked);
	pp.Define("PS_FBA", sel.fba);
	pp.Define("PS_LTF", sel.ltf);
	pp.Define("PS_AUTOMATIC_LOD", sel.automatic_lod);
	pp.Define("PS_MANUAL_LOD", sel.manual_lod);
	pp.Define("PS_COLCLIP", sel.colclip);
	pp.Define("PS_DATE", sel.date);
	pp.Define("PS_TCOFFSETHACK", sel.tcoffsethack);
	pp.Define("PS_REGION_RECT", sel.region_rect);
	pp.Define("PS_BLEND_A", sel.blend_a);
	pp.Define("PS_BLEND_B", sel.blend_b);
	pp.Define("PS_BLEND_C", sel.blend_c);
	pp.Define("PS_BLEND_D", sel.blend_d);
	pp.Define("PS_BLEND_MIX", sel.blend_mix);
	pp.Define("PS_ROUND_INV", sel.round_inv);
	pp.Define("PS_FIXED_ONE_A", sel.fixed_one_a);
	pp.Define("PS_IIP", sel.iip);
	pp.Define("PS_SHUFFLE", sel.shuffle);
	pp.Define("PS_SHUFFLE_SAME", sel.shuffle_same);
	pp.Define("PS_PROCESS_BA", sel.process_ba);
	pp.Define("PS_PROCESS_RG", sel.process_rg);
	pp.Define("PS_SHUFFLE_ACROSS", sel.shuffle_across);
	pp.Define("PS_READ16_SRC", sel.real16src);
	pp.Define("PS_WRITE_RG", sel.write_rg);
	pp.Define("PS_FBMASK", sel.fbmask);
	pp.Define("PS_COLCLIP_HW", sel.colclip_hw);
	pp.Define("PS_RTA_CORRECTION", sel.rta_correction);
	pp.Define("PS_RTA_SRC_CORRECTION", sel.rta_source_correction);
	pp.Define("PS_DITHER", sel.dither);
	pp.Define("PS_DITHER_ADJUST", sel.dither_adjust);
	pp.Define("PS_ZCLAMP", sel.zclamp);
	pp.Define("PS_ZFLOOR", sel.zfloor);
	pp.Define("PS_PABE", sel.pabe);
	pp.Define("PS_SCANMSK", sel.scanmsk);
	pp.Define("PS_TEX_IS_FB", sel.tex_is_fb);
	pp.Define("PS_NO_COLOR", sel.no_color);
	pp.Define("PS_NO_COLOR1", sel.no_color1);
	pp.Define("PS_ZTST", sel.ztst);
	pp.Define("PS_AA1", static_cast<s64>(sel.aa1));
	pp.Define("PS_ABE", sel.abe);
	pp.Define("PS_ANISOTROPIC_FILTERING", sel.sw_aniso);
	pp.Define("PS_ROV_COLOR", sel.rov_color);
	pp.Define("PS_ROV_DEPTH", static_cast<s64>(sel.rov_depth));
}

void GSDeviceWebGPU::AddConvertShaderMacros(WGSLPreprocessor& pp, ShaderConvertSelector sel)
{
	pp.Define("HAS_BILN", static_cast<s64>(sel.Biln()));
	pp.Define("HAS_STENCIL_OUTPUT", static_cast<s64>(sel.StencilOutput()));
	pp.Define("HAS_INTEGER_OUTPUT", static_cast<s64>(sel.IntegerOutputBpp() != 0));
	pp.Define("HAS_DEPTH_OUTPUT", static_cast<s64>(sel.DepthOutput()));
	pp.Define("HAS_FLOAT32_INPUT", static_cast<s64>(sel.Float32Input()));
	pp.Define("HAS_FLOAT32_OUTPUT", static_cast<s64>(sel.Float32Output()));
}

WGPUShaderModule GSDeviceWebGPU::GetUtilityVertexShader(const std::string& source, const char* file_label)
{
	WGSLPreprocessor pp;
	pp.Define("VERTEX_SHADER", 1);

	std::string processed;
	std::string error;
	if (!pp.Process(source, &processed, &error))
	{
		Console.Error("WebGPU: Preprocessing %s (vertex) failed: %s", file_label, error.c_str());
		return nullptr;
	}

	return CreateShaderModule(processed, file_label);
}

WGPUShaderModule GSDeviceWebGPU::GetUtilityFragmentShader(const std::string& source, const char* entry_point, const char* file_label, const WGSLPreprocessor* extra_defines)
{
	WGSLPreprocessor pp;
	if (extra_defines)
	{
		for (const auto& [name, value] : extra_defines->GetDefines())
			pp.Define(name, value);
	}
	pp.Define("FRAGMENT_SHADER", 1);
	if (entry_point)
		pp.Define(entry_point, "");

	std::string processed;
	std::string error;
	if (!pp.Process(source, &processed, &error))
	{
		Console.Error("WebGPU: Preprocessing %s (%s) failed: %s", file_label, entry_point ? entry_point : "fragment", error.c_str());
		return nullptr;
	}

	return CreateShaderModule(processed, file_label);
}

WGPUShaderModule GSDeviceWebGPU::GetTFXVertexShader(GSHWDrawConfig::VSSelector sel)
{
	const auto it = m_tfx_vertex_shaders.find(sel.key);
	if (it != m_tfx_vertex_shaders.end())
		return it->second;

	WGSLPreprocessor pp;
	AddTFXVertexShaderMacros(pp, sel, m_features.provoking_vertex_last);

	std::string processed;
	std::string error;
	WGPUShaderModule mod = nullptr;
	if (!pp.Process(m_tfx_source, &processed, &error))
		Console.Error("WebGPU: Preprocessing TFX vertex shader %02X failed: %s", sel.key, error.c_str());
	else
		mod = CreateShaderModule(processed, TinyString::from_format("TFX Vertex {:02X}", sel.key));

	m_tfx_vertex_shaders.emplace(sel.key, mod);
	return mod;
}

WGPUShaderModule GSDeviceWebGPU::GetTFXFragmentShader(const GSHWDrawConfig::PSSelector& sel)
{
	const auto it = m_tfx_fragment_shaders.find(sel);
	if (it != m_tfx_fragment_shaders.end())
		return it->second;

	WGSLPreprocessor pp;
	AddTFXFragmentShaderMacros(pp, sel);
	pp.Define("HAS_DUAL_SOURCE_BLEND", static_cast<s64>(m_device_features.dual_source_blending));
	pp.Define("HAS_PRIMITIVE_INDEX", static_cast<s64>(m_device_features.primitive_index));

	std::string processed;
	std::string error;
	WGPUShaderModule mod = nullptr;
	if (!pp.Process(m_tfx_source, &processed, &error))
		Console.Error("WebGPU: Preprocessing TFX fragment shader %016" PRIX64 "_%016" PRIX64 " failed: %s", sel.key_hi, sel.key_lo, error.c_str());
	else
		mod = CreateShaderModule(processed, TinyString::from_format("TFX Fragment {:016X}_{:016X}", sel.key_hi, sel.key_lo));

	m_tfx_fragment_shaders.emplace(sel, mod);
	return mod;
}

//////////////////////////////////////////////////////////////////////////
// Pipelines
//////////////////////////////////////////////////////////////////////////

GSDeviceWebGPU::UtilityPipeline GSDeviceWebGPU::CreateUtilityPipeline(WGPUShaderModule vs, WGPUShaderModule fs, const char* fs_entry,
	WGPUTextureFormat color_format, u32 color_write_mask, const WGPUBlendState* blend, WGPUTextureFormat depth_format,
	bool depth_write, bool stencil_write, WGPUPrimitiveTopology topology, bool imgui_vertex, bool depth_input, const char* label)
{
	std::array<WGPUVertexAttribute, 3> attributes = {};
	for (WGPUVertexAttribute& attr : attributes)
		attr = WGPU_VERTEX_ATTRIBUTE_INIT;

	WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
	vbl.stepMode = WGPUVertexStepMode_Vertex;
	if (imgui_vertex)
	{
		vbl.arrayStride = sizeof(ImDrawVert);
		vbl.attributeCount = 3;
		attributes[0] = {nullptr, WGPUVertexFormat_Float32x2, offsetof(ImDrawVert, pos), 0};
		attributes[1] = {nullptr, WGPUVertexFormat_Float32x2, offsetof(ImDrawVert, uv), 1};
		attributes[2] = {nullptr, WGPUVertexFormat_Unorm8x4, offsetof(ImDrawVert, col), 2};
	}
	else
	{
		vbl.arrayStride = sizeof(GSVertexPT1);
		vbl.attributeCount = 2;
		attributes[0] = {nullptr, WGPUVertexFormat_Float32x4, 0, 0};
		attributes[1] = {nullptr, WGPUVertexFormat_Float32x2, 16, 1};
	}
	vbl.attributes = attributes.data();

	WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	desc.label = StringView(label);
	desc.layout = m_utility_pipeline_layouts[depth_input ? 1 : 0];
	desc.vertex.module = vs;
	desc.vertex.entryPoint = StringView("vs_main");
	desc.vertex.bufferCount = 1;
	desc.vertex.buffers = &vbl;
	desc.primitive.topology = topology;
	desc.primitive.stripIndexFormat = (topology == WGPUPrimitiveTopology_TriangleStrip) ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Undefined;
	desc.primitive.frontFace = WGPUFrontFace_CW;
	desc.primitive.cullMode = WGPUCullMode_None;

	WGPUDepthStencilState dss = WGPU_DEPTH_STENCIL_STATE_INIT;
	if (depth_format != WGPUTextureFormat_Undefined)
	{
		dss.format = depth_format;
		dss.depthWriteEnabled = depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
		dss.depthCompare = WGPUCompareFunction_Always;
		if (stencil_write && depth_format == WGPUTextureFormat_Depth32FloatStencil8)
		{
			dss.stencilFront = {WGPUCompareFunction_Always, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep, WGPUStencilOperation_Replace};
			dss.stencilBack = dss.stencilFront;
			dss.stencilReadMask = 0xFF;
			dss.stencilWriteMask = 0xFF;
		}
		desc.depthStencil = &dss;
	}

	WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
	target.format = color_format;
	target.blend = blend;
	target.writeMask = color_write_mask;

	WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
	fragment.module = fs;
	fragment.entryPoint = StringView(fs_entry);
	fragment.targetCount = (color_format != WGPUTextureFormat_Undefined) ? 1 : 0;
	fragment.targets = (color_format != WGPUTextureFormat_Undefined) ? &target : nullptr;
	desc.fragment = &fragment;

	UtilityPipeline ret;
	ret.pipeline = wgpuDeviceCreateRenderPipeline(m_device, &desc);
	ret.depth_input = depth_input;
	if (!ret.pipeline)
		Console.Error("WebGPU: Failed to create pipeline '%s'", label);

	return ret;
}

bool GSDeviceWebGPU::CompileConvertPipelines()
{
	const std::optional<std::string> source = ReadShaderSource("shaders/webgpu/convert.wgsl");
	if (!source)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/webgpu/convert.wgsl.");
		return false;
	}

	WGPUShaderModule vs = GetUtilityVertexShader(*source, "convert.wgsl");
	if (!vs)
		return false;
	ScopedGuard vs_guard([&vs]() { wgpuShaderModuleRelease(vs); });

	m_convert.resize(ShaderConvertSelector::NUM_TOTAL_SHADERS);
	for (u32 i = 0; i < ShaderConvertSelector::NUM_TOTAL_SHADERS; i++)
	{
		const ShaderConvertSelector shader = ShaderConvertSelector::Get(i);

		WGSLPreprocessor defines;
		AddConvertShaderMacros(defines, shader);

		WGPUShaderModule ps = GetUtilityFragmentShader(*source, shader.EntryPoint(), "convert.wgsl", &defines);
		if (!ps)
			return false;
		ScopedGuard ps_guard([&ps]() { wgpuShaderModuleRelease(ps); });

		WGPUTextureFormat color_format = WGPUTextureFormat_Undefined;
		WGPUTextureFormat depth_format = WGPUTextureFormat_Undefined;
		bool depth_write = false;
		bool stencil_write = false;
		if (shader.DATMConvertShader())
		{
			depth_format = LookupNativeFormat(GSTexture::Format::DepthStencil);
			stencil_write = true;
		}
		else if (shader.DepthOutput())
		{
			depth_format = LookupNativeFormat(GSTexture::Format::DepthStencil);
			depth_write = true;
		}
		else
		{
			color_format = LookupNativeFormat(shader.OutputFormat());
		}

		const std::string label = fmt::format("Convert pipeline ({}, mask={:x}, depth={}, biln={})", shader.Name(), shader.Mask(),
			static_cast<int>(shader.DepthOutput()), static_cast<int>(shader.Biln()));
		m_convert[i] = CreateUtilityPipeline(vs, ps, shader.EntryPoint(), color_format, shader.Mask(), nullptr, depth_format,
			depth_write, stencil_write, WGPUPrimitiveTopology_TriangleStrip, false, shader.Float32Input(), label.c_str());
		if (!m_convert[i].pipeline)
			return false;
	}

	for (u32 datm = 0; datm < 4; datm++)
	{
		const std::string entry_point = fmt::format("ps_primid_image_init_{}", datm);
		WGPUShaderModule ps = GetUtilityFragmentShader(*source, entry_point.c_str(), "convert.wgsl");
		if (!ps)
			return false;
		ScopedGuard ps_guard([&ps]() { wgpuShaderModuleRelease(ps); });

		const std::string label = fmt::format("DATE image clear pipeline (datm={})", datm);
		m_primid_image_setup_pipelines[datm] = CreateUtilityPipeline(vs, ps, entry_point.c_str(), LookupNativeFormat(GSTexture::Format::PrimID),
			WGPUColorWriteMask_Red, nullptr, WGPUTextureFormat_Undefined, false, false, WGPUPrimitiveTopology_TriangleStrip, false, false, label.c_str());
		if (!m_primid_image_setup_pipelines[datm].pipeline)
			return false;
	}

	return true;
}

bool GSDeviceWebGPU::CompilePresentPipelines()
{
	const std::optional<std::string> shader = ReadShaderSource("shaders/webgpu/present.wgsl");
	if (!shader)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/webgpu/present.wgsl.");
		return false;
	}

	WGPUShaderModule vs = GetUtilityVertexShader(*shader, "present.wgsl");
	if (!vs)
		return false;
	ScopedGuard vs_guard([&vs]() { wgpuShaderModuleRelease(vs); });

	for (PresentShader i = PresentShader::COPY; i < PresentShader::Count; i = static_cast<PresentShader>(static_cast<int>(i) + 1))
	{
		const int index = static_cast<int>(i);
		WGPUShaderModule ps = GetUtilityFragmentShader(*shader, ShaderEntryPoint(i), "present.wgsl");
		if (!ps)
			return false;
		ScopedGuard ps_guard([&ps]() { wgpuShaderModuleRelease(ps); });

		const std::string label = fmt::format("Present pipeline {}", index);
		m_present[index] = CreateUtilityPipeline(vs, ps, ShaderEntryPoint(i), m_surface_format, WGPUColorWriteMask_All, nullptr,
			WGPUTextureFormat_Undefined, false, false, WGPUPrimitiveTopology_TriangleStrip, false, false, label.c_str());
		if (!m_present[index].pipeline)
			return false;

		const std::string offscreen_label = fmt::format("Present pipeline {} (offscreen)", index);
		m_present_offscreen[index] = CreateUtilityPipeline(vs, ps, ShaderEntryPoint(i), LookupNativeFormat(GSTexture::Format::Color),
			WGPUColorWriteMask_All, nullptr, WGPUTextureFormat_Undefined, false, false, WGPUPrimitiveTopology_TriangleStrip, false, false,
			offscreen_label.c_str());
		if (!m_present_offscreen[index].pipeline)
			return false;
	}

	return true;
}

bool GSDeviceWebGPU::CompileInterlacePipelines()
{
	const std::optional<std::string> shader = ReadShaderSource("shaders/webgpu/interlace.wgsl");
	if (!shader)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/webgpu/interlace.wgsl.");
		return false;
	}

	WGPUShaderModule vs = GetUtilityVertexShader(*shader, "interlace.wgsl");
	if (!vs)
		return false;
	ScopedGuard vs_guard([&vs]() { wgpuShaderModuleRelease(vs); });

	for (int i = 0; i < static_cast<int>(m_interlace.size()); i++)
	{
		const std::string entry_point = fmt::format("ps_main{}", i);
		WGPUShaderModule ps = GetUtilityFragmentShader(*shader, entry_point.c_str(), "interlace.wgsl");
		if (!ps)
			return false;
		ScopedGuard ps_guard([&ps]() { wgpuShaderModuleRelease(ps); });

		const std::string label = fmt::format("Interlace pipeline {}", i);
		m_interlace[i] = CreateUtilityPipeline(vs, ps, entry_point.c_str(), LookupNativeFormat(GSTexture::Format::Color), WGPUColorWriteMask_All,
			nullptr, WGPUTextureFormat_Undefined, false, false, WGPUPrimitiveTopology_TriangleStrip, false, false, label.c_str());
		if (!m_interlace[i].pipeline)
			return false;
	}

	return true;
}

bool GSDeviceWebGPU::CompileMergePipelines()
{
	const std::optional<std::string> shader = ReadShaderSource("shaders/webgpu/merge.wgsl");
	if (!shader)
	{
		Host::ReportErrorAsync("GS", "Failed to read shaders/webgpu/merge.wgsl.");
		return false;
	}

	WGPUShaderModule vs = GetUtilityVertexShader(*shader, "merge.wgsl");
	if (!vs)
		return false;
	ScopedGuard vs_guard([&vs]() { wgpuShaderModuleRelease(vs); });

	WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
	blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha};
	blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_Zero};

	for (int i = 0; i < static_cast<int>(m_merge.size()); i++)
	{
		const std::string entry_point = fmt::format("ps_main{}", i);
		WGPUShaderModule ps = GetUtilityFragmentShader(*shader, entry_point.c_str(), "merge.wgsl");
		if (!ps)
			return false;
		ScopedGuard ps_guard([&ps]() { wgpuShaderModuleRelease(ps); });

		const std::string label = fmt::format("Merge pipeline {}", i);
		m_merge[i] = CreateUtilityPipeline(vs, ps, entry_point.c_str(), LookupNativeFormat(GSTexture::Format::Color), WGPUColorWriteMask_All,
			&blend, WGPUTextureFormat_Undefined, false, false, WGPUPrimitiveTopology_TriangleStrip, false, false, label.c_str());
		if (!m_merge[i].pipeline)
			return false;
	}

	return true;
}

bool GSDeviceWebGPU::CompilePostProcessingPipelines()
{
	{
		const std::optional<std::string> shader = ReadShaderSource("shaders/webgpu/fxaa.wgsl");
		if (!shader)
		{
			Host::ReportErrorAsync("GS", "Failed to read shaders/webgpu/fxaa.wgsl.");
			return false;
		}

		WGPUShaderModule vs = GetUtilityVertexShader(*shader, "fxaa.wgsl");
		if (!vs)
			return false;
		ScopedGuard vs_guard([&vs]() { wgpuShaderModuleRelease(vs); });
		WGPUShaderModule ps = GetUtilityFragmentShader(*shader, "ps_main", "fxaa.wgsl");
		if (!ps)
			return false;
		ScopedGuard ps_guard([&ps]() { wgpuShaderModuleRelease(ps); });

		m_fxaa_pipeline = CreateUtilityPipeline(vs, ps, "ps_main", LookupNativeFormat(GSTexture::Format::Color), WGPUColorWriteMask_All,
			nullptr, WGPUTextureFormat_Undefined, false, false, WGPUPrimitiveTopology_TriangleStrip, false, false, "FXAA pipeline");
		if (!m_fxaa_pipeline.pipeline)
			return false;
	}

	{
		const std::optional<std::string> shader = ReadShaderSource("shaders/webgpu/shadeboost.wgsl");
		if (!shader)
		{
			Host::ReportErrorAsync("GS", "Failed to read shaders/webgpu/shadeboost.wgsl.");
			return false;
		}

		WGPUShaderModule vs = GetUtilityVertexShader(*shader, "shadeboost.wgsl");
		if (!vs)
			return false;
		ScopedGuard vs_guard([&vs]() { wgpuShaderModuleRelease(vs); });
		WGPUShaderModule ps = GetUtilityFragmentShader(*shader, "ps_main", "shadeboost.wgsl");
		if (!ps)
			return false;
		ScopedGuard ps_guard([&ps]() { wgpuShaderModuleRelease(ps); });

		m_shadeboost_pipeline = CreateUtilityPipeline(vs, ps, "ps_main", LookupNativeFormat(GSTexture::Format::Color), WGPUColorWriteMask_All,
			nullptr, WGPUTextureFormat_Undefined, false, false, WGPUPrimitiveTopology_TriangleStrip, false, false, "Shadeboost pipeline");
		if (!m_shadeboost_pipeline.pipeline)
			return false;
	}

	return true;
}

bool GSDeviceWebGPU::CompileImGuiPipeline()
{
	const std::optional<std::string> shader = ReadShaderSource("shaders/webgpu/imgui.wgsl");
	if (!shader.has_value())
	{
		Console.Error("WebGPU: Failed to read imgui.wgsl");
		return false;
	}

	WGPUShaderModule vs = GetUtilityVertexShader(*shader, "imgui.wgsl");
	if (!vs)
		return false;
	ScopedGuard vs_guard([&vs]() { wgpuShaderModuleRelease(vs); });
	WGPUShaderModule ps = GetUtilityFragmentShader(*shader, "ps_main", "imgui.wgsl");
	if (!ps)
		return false;
	ScopedGuard ps_guard([&ps]() { wgpuShaderModuleRelease(ps); });

	WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
	blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha};
	blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_Zero};

	m_imgui_pipeline = CreateUtilityPipeline(vs, ps, "ps_main", m_surface_format, WGPUColorWriteMask_All, &blend,
		WGPUTextureFormat_Undefined, false, false, WGPUPrimitiveTopology_TriangleList, true, false, "ImGui pipeline");
	return (m_imgui_pipeline.pipeline != nullptr);
}

WGPURenderPipeline GSDeviceWebGPU::CreateTFXPipeline(const PipelineSelector& p)
{
	static constexpr std::array<WGPUPrimitiveTopology, 3> topology_lookup = {{
		WGPUPrimitiveTopology_PointList,
		WGPUPrimitiveTopology_LineList,
		WGPUPrimitiveTopology_TriangleList,
	}};

	GSHWDrawConfig::BlendState pbs{p.bs};
	GSHWDrawConfig::PSSelector pps{p.ps};
	if (!p.bs.IsEffective(p.cms))
	{
		pbs = {};
		pps.no_color1 = true;
	}

	if (!m_device_features.dual_source_blending && pbs.enable &&
		(IsDualSourceBlendFactor(pbs.src_factor) || IsDualSourceBlendFactor(pbs.dst_factor) ||
			IsDualSourceBlendFactor(pbs.src_factor_alpha) || IsDualSourceBlendFactor(pbs.dst_factor_alpha)))
	{
		Console.Error("WebGPU: Dual source blend factors requested without dual-source-blending support.");
		pbs = {};
		pps.no_color1 = true;
	}

	WGPUShaderModule vs = GetTFXVertexShader(p.vs);
	WGPUShaderModule fs = GetTFXFragmentShader(pps);
	if (!vs || !fs)
		return nullptr;

	std::array<WGPUVertexAttribute, 7> attributes = {{
		{nullptr, WGPUVertexFormat_Float32x2, 0, 0},
		{nullptr, WGPUVertexFormat_Uint8x4, 8, 1},
		{nullptr, WGPUVertexFormat_Float32, 12, 2},
		{nullptr, WGPUVertexFormat_Uint16x2, 16, 3},
		{nullptr, WGPUVertexFormat_Uint32, 20, 4},
		{nullptr, WGPUVertexFormat_Uint16x2, 24, 5},
		{nullptr, WGPUVertexFormat_Unorm8x4, 28, 6},
	}};

	WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
	vbl.stepMode = WGPUVertexStepMode_Vertex;
	vbl.arrayStride = sizeof(GSVertex);
	vbl.attributeCount = attributes.size();
	vbl.attributes = attributes.data();

	WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	const TinyString label = TinyString::from_format("TFX Pipeline {:08X}/{:016X}_{:016X}", p.vs.key, p.ps.key_hi, p.ps.key_lo);
	desc.label = StringView(label.c_str());
	desc.layout = m_tfx_pipeline_layouts[p.tex_depth];
	desc.vertex.module = vs;
	desc.vertex.entryPoint = StringView("vs_main");
	if (p.vs.expand == GSHWDrawConfig::VSExpand::None)
	{
		desc.vertex.bufferCount = 1;
		desc.vertex.buffers = &vbl;
	}
	desc.primitive.topology = topology_lookup[p.topology];
	desc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
	desc.primitive.frontFace = WGPUFrontFace_CW;
	desc.primitive.cullMode = WGPUCullMode_None;

	static constexpr WGPUCompareFunction ztst[] = {
		WGPUCompareFunction_Never, WGPUCompareFunction_Always, WGPUCompareFunction_GreaterEqual, WGPUCompareFunction_Greater};

	WGPUDepthStencilState dss = WGPU_DEPTH_STENCIL_STATE_INIT;
	if (p.ds)
	{
		dss.format = LookupNativeFormat(GSTexture::Format::DepthStencil);
		dss.depthWriteEnabled = p.dss.zwe ? WGPUOptionalBool_True : WGPUOptionalBool_False;
		dss.depthCompare = ztst[p.dss.ztst];
		if (p.dss.date && m_device_features.depth32float_stencil8)
		{
			dss.stencilFront = {WGPUCompareFunction_Equal, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep,
				p.dss.date_one ? WGPUStencilOperation_Zero : WGPUStencilOperation_Keep};
			dss.stencilBack = dss.stencilFront;
			dss.stencilReadMask = 1;
			dss.stencilWriteMask = 1;
		}
		desc.depthStencil = &dss;
	}

	static constexpr std::array<WGPUBlendFactor, 16> wgpu_blend_factors = {{
		WGPUBlendFactor_Src, WGPUBlendFactor_OneMinusSrc, WGPUBlendFactor_Dst, WGPUBlendFactor_OneMinusDst,
		WGPUBlendFactor_Src1, WGPUBlendFactor_OneMinusSrc1, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha,
		WGPUBlendFactor_DstAlpha, WGPUBlendFactor_OneMinusDstAlpha, WGPUBlendFactor_Src1Alpha, WGPUBlendFactor_OneMinusSrc1Alpha,
		WGPUBlendFactor_Constant, WGPUBlendFactor_OneMinusConstant, WGPUBlendFactor_One, WGPUBlendFactor_Zero,
	}};
	static constexpr std::array<WGPUBlendOperation, 3> wgpu_blend_ops = {{
		WGPUBlendOperation_Add, WGPUBlendOperation_Subtract, WGPUBlendOperation_ReverseSubtract,
	}};

	WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
	WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
	WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
	fragment.module = fs;
	fragment.entryPoint = StringView("ps_main");
	if (p.rt)
	{
		if (IsDATEModePrimIDInit(p.ps.date))
		{
			target.format = LookupNativeFormat(GSTexture::Format::PrimID);
			target.writeMask = WGPUColorWriteMask_Red;
			blend.color = {WGPUBlendOperation_Min, WGPUBlendFactor_One, WGPUBlendFactor_One};
			blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_Zero};
			target.blend = &blend;
		}
		else
		{
			target.format = LookupNativeFormat(p.ps.colclip_hw ? GSTexture::Format::ColorClip : GSTexture::Format::Color);
			target.writeMask = pps.no_color ? WGPUColorWriteMask_None : static_cast<WGPUColorWriteMask>(p.cms.wrgba);
			if (pbs.enable && !pps.no_color)
			{
				blend.color = {wgpu_blend_ops[pbs.op], wgpu_blend_factors[pbs.src_factor], wgpu_blend_factors[pbs.dst_factor]};
				blend.alpha = {WGPUBlendOperation_Add, wgpu_blend_factors[pbs.src_factor_alpha], wgpu_blend_factors[pbs.dst_factor_alpha]};
				target.blend = &blend;
			}
		}

		fragment.targetCount = 1;
		fragment.targets = &target;
	}
	desc.fragment = &fragment;

	WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(m_device, &desc);
	if (!pipeline)
		Console.Error("WebGPU: Failed to create %s", label.c_str());

	return pipeline;
}

WGPURenderPipeline GSDeviceWebGPU::GetTFXPipeline(const PipelineSelector& p)
{
	const auto it = m_tfx_pipelines.find(p);
	if (it != m_tfx_pipelines.end())
		return it->second;

	WGPURenderPipeline pipeline = CreateTFXPipeline(p);
	m_tfx_pipelines.emplace(p, pipeline);
	return pipeline;
}

void GSDeviceWebGPU::DestroyResources()
{
	for (auto& it : m_tfx_bind_groups)
		wgpuBindGroupRelease(it.second);
	m_tfx_bind_groups.clear();
	for (auto& it : m_utility_bind_groups)
		wgpuBindGroupRelease(it.second);
	m_utility_bind_groups.clear();

	for (auto& it : m_tfx_pipelines)
	{
		if (it.second)
			wgpuRenderPipelineRelease(it.second);
	}
	m_tfx_pipelines.clear();
	for (auto& it : m_tfx_fragment_shaders)
	{
		if (it.second)
			wgpuShaderModuleRelease(it.second);
	}
	m_tfx_fragment_shaders.clear();
	for (auto& it : m_tfx_vertex_shaders)
	{
		if (it.second)
			wgpuShaderModuleRelease(it.second);
	}
	m_tfx_vertex_shaders.clear();

	const auto release_pipeline = [](UtilityPipeline& p) {
		if (p.pipeline)
			wgpuRenderPipelineRelease(p.pipeline);
		p.pipeline = nullptr;
	};
	for (UtilityPipeline& it : m_interlace)
		release_pipeline(it);
	for (UtilityPipeline& it : m_merge)
		release_pipeline(it);
	for (UtilityPipeline& it : m_present)
		release_pipeline(it);
	for (UtilityPipeline& it : m_present_offscreen)
		release_pipeline(it);
	for (UtilityPipeline& it : m_convert)
		release_pipeline(it);
	m_convert.clear();
	for (UtilityPipeline& it : m_primid_image_setup_pipelines)
		release_pipeline(it);
	release_pipeline(m_fxaa_pipeline);
	release_pipeline(m_shadeboost_pipeline);
	release_pipeline(m_imgui_pipeline);

	for (const auto& it : m_samplers)
	{
		if (it.second)
			wgpuSamplerRelease(it.second);
	}
	m_samplers.clear();
	m_point_sampler = nullptr;
	m_linear_sampler = nullptr;

	if (m_null_texture)
	{
		m_null_texture->Destroy();
		m_null_texture.reset();
	}

	m_texture_stream_buffer.Destroy();
	m_fragment_uniform_stream_buffer.Destroy();
	m_vertex_uniform_stream_buffer.Destroy();
	m_index_stream_buffer.Destroy();
	m_vertex_stream_buffer.Destroy();
	m_expand_index_stream_buffer.Destroy();
	if (m_expand_index_buffer)
	{
		wgpuBufferRelease(m_expand_index_buffer);
		m_expand_index_buffer = nullptr;
	}

	if (m_tfx_ubo_bind_group)
	{
		wgpuBindGroupRelease(m_tfx_ubo_bind_group);
		m_tfx_ubo_bind_group = nullptr;
	}
	for (WGPUPipelineLayout& layout : m_tfx_pipeline_layouts)
	{
		if (layout)
			wgpuPipelineLayoutRelease(layout);
		layout = nullptr;
	}
	for (WGPUBindGroupLayout& layout : m_tfx_texture_bind_group_layouts)
	{
		if (layout)
			wgpuBindGroupLayoutRelease(layout);
		layout = nullptr;
	}
	if (m_tfx_ubo_bind_group_layout)
	{
		wgpuBindGroupLayoutRelease(m_tfx_ubo_bind_group_layout);
		m_tfx_ubo_bind_group_layout = nullptr;
	}
	for (WGPUPipelineLayout& layout : m_utility_pipeline_layouts)
	{
		if (layout)
			wgpuPipelineLayoutRelease(layout);
		layout = nullptr;
	}
	for (WGPUBindGroupLayout& layout : m_utility_bind_group_layouts)
	{
		if (layout)
			wgpuBindGroupLayoutRelease(layout);
		layout = nullptr;
	}

	if (m_command_encoder)
	{
		wgpuCommandEncoderRelease(m_command_encoder);
		m_command_encoder = nullptr;
	}
	m_pending_submits.clear();
}

//////////////////////////////////////////////////////////////////////////
// Window / presentation
//////////////////////////////////////////////////////////////////////////

bool GSDeviceWebGPU::UpdateWindow()
{
	DestroySurface();

	if (!AcquireWindow(false))
		return false;

	if (m_window_info.type == WindowInfo::Type::Surfaceless)
		return true;

	WaitForGPUIdle();

	if (!CreateSurface() || !ConfigureSurface())
	{
		DestroySurface();
		return false;
	}

	InvalidateCachedState();
	return true;
}

void GSDeviceWebGPU::ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale)
{
	m_resize_requested = false;
	m_window_info.surface_scale = new_window_scale;

	if (!m_surface)
		return;

	if (m_window_info.surface_width == new_window_width && m_window_info.surface_height == new_window_height && m_surface_configured)
		return;

	WaitForGPUIdle();
	m_window_info.surface_width = new_window_width;
	m_window_info.surface_height = new_window_height;
	ConfigureSurface();
}

bool GSDeviceWebGPU::SupportsExclusiveFullscreen() const
{
	return false;
}

void GSDeviceWebGPU::DestroySurface()
{
	if (!m_surface)
		return;

	WaitForGPUIdle();
	m_surface_texture.reset();
	if (m_surface_configured)
	{
		wgpuSurfaceUnconfigure(m_surface);
		m_surface_configured = false;
	}
	wgpuSurfaceRelease(m_surface);
	m_surface = nullptr;
}

std::string GSDeviceWebGPU::GetDriverInfo() const
{
	static constexpr const char* backend_names[] = {"Undefined", "Null", "WebGPU", "D3D11", "D3D12", "Metal", "Vulkan", "OpenGL", "OpenGLES"};
	const u32 backend = static_cast<u32>(m_backend_type);
	return StringUtil::StdStringFromFormat("WebGPU (%s)\n%s\n%s", (backend < std::size(backend_names)) ? backend_names[backend] : "Unknown",
		m_adapter_name.c_str(), m_adapter_description.c_str());
}

void GSDeviceWebGPU::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_allow_present_throttle = allow_present_throttle;
	if (m_vsync_mode == mode)
		return;

	m_vsync_mode = mode;
	if (!m_surface)
		return;

	WaitForGPUIdle();
	ConfigureSurface();
}

GSDevice::PresentResult GSDeviceWebGPU::BeginPresent(bool frame_skip)
{
	EndRenderPass();

	if (m_device_lost)
		return PresentResult::DeviceLost;

	if (frame_skip)
		return PresentResult::FrameSkipped;

	if (!m_surface)
	{
		ExecuteCommandBuffer(false);
		return PresentResult::FrameSkipped;
	}

	if (m_resize_requested)
	{
		ResizeWindow(m_window_info.surface_width, m_window_info.surface_height, m_window_info.surface_scale);
		ImGuiManager::WindowResized();
	}

	WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
	wgpuSurfaceGetCurrentTexture(m_surface, &st);
	if (st.status == WGPUSurfaceGetCurrentTextureStatus_Outdated || st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
	{
		if (st.texture)
			wgpuTextureRelease(st.texture);

		WaitForGPUIdle();
		ConfigureSurface();
		ImGuiManager::WindowResized();
		st = WGPU_SURFACE_TEXTURE_INIT;
		wgpuSurfaceGetCurrentTexture(m_surface, &st);
	}

	if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal && st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
	{
		Console.Error("WebGPU: wgpuSurfaceGetCurrentTexture() failed: %u", static_cast<u32>(st.status));
		if (st.texture)
			wgpuTextureRelease(st.texture);
		ExecuteCommandBuffer(false);
		return PresentResult::FrameSkipped;
	}

	m_surface_texture = GSTextureWebGPU::Adopt(st.texture, GSTexture::RenderTarget, GSTexture::Format::Color,
		static_cast<int>(wgpuTextureGetWidth(st.texture)), static_cast<int>(wgpuTextureGetHeight(st.texture)), m_surface_format);
	wgpuTextureRelease(st.texture);
	if (!m_surface_texture)
	{
		ExecuteCommandBuffer(false);
		return PresentResult::FrameSkipped;
	}

	if (m_current)
		static_cast<GSTextureWebGPU*>(m_current)->CommitClear();

	RenderPassLoadOps ops;
	ops.rt_load = WGPULoadOp_Clear;
	ops.rt_clear = GSVector4(0.0f, 0.0f, 0.0f, 1.0f);
	BeginRenderPass(m_surface_texture.get(), nullptr, ops);

	const GSVector4i rc(0, 0, m_surface_texture->GetWidth(), m_surface_texture->GetHeight());
	m_viewport = rc;
	m_scissor = rc;
	wgpuRenderPassEncoderSetViewport(m_render_pass, 0.0f, 0.0f, static_cast<float>(rc.width()), static_cast<float>(rc.height()), 0.0f, 1.0f);
	wgpuRenderPassEncoderSetScissorRect(m_render_pass, 0, 0, rc.width(), rc.height());
	m_dirty_flags &= ~(DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);
	m_is_presenting = true;
	return PresentResult::OK;
}

void GSDeviceWebGPU::EndPresent()
{
	RenderImGui();

	EndRenderPass();
	m_is_presenting = false;
	g_perfmon.Put(GSPerfMon::RenderPasses, 1);

	SubmitCommandBuffer();
	MoveToNextCommandBuffer();

	if (m_surface)
	{
		const WGPUStatus status = wgpuSurfacePresent(m_surface);
		if (status != WGPUStatus_Success)
		{
			Console.Warning("WebGPU: wgpuSurfacePresent() failed, requesting resize");
			m_resize_requested = true;
		}
	}

	m_surface_texture.reset();
	InvalidateCachedState();
}

bool GSDeviceWebGPU::SetGPUTimingEnabled(bool enabled)
{
	return !enabled;
}

float GSDeviceWebGPU::GetAndResetAccumulatedGPUTime()
{
	return 0.0f;
}

bool GSDeviceWebGPU::SetGPUPipelineStatisticsEnabled(bool enabled)
{
	return !enabled;
}

GPUPipelineStatistics GSDeviceWebGPU::GetAndResetAccumulatedGPUPipelineStatistics()
{
	return {};
}

void GSDeviceWebGPU::PushDebugGroup(const char* fmt, ...)
{
#ifdef ENABLE_OGL_DEBUG
	if (!GSConfig.UseDebugDevice)
		return;

	std::va_list ap;
	va_start(ap, fmt);
	const std::string buf(StringUtil::StdStringFromFormatV(fmt, ap));
	va_end(ap);

	if (m_render_pass)
		wgpuRenderPassEncoderPushDebugGroup(m_render_pass, {buf.data(), buf.size()});
	else
		wgpuCommandEncoderPushDebugGroup(GetCommandEncoder(), {buf.data(), buf.size()});
	m_debug_group_in_pass.push_back(m_render_pass != nullptr);
#endif
}

void GSDeviceWebGPU::PopDebugGroup()
{
#ifdef ENABLE_OGL_DEBUG
	if (!GSConfig.UseDebugDevice || m_debug_group_in_pass.empty())
		return;

	const bool in_pass = m_debug_group_in_pass.back();
	m_debug_group_in_pass.pop_back();
	if (in_pass && m_render_pass)
		wgpuRenderPassEncoderPopDebugGroup(m_render_pass);
	else if (!in_pass && !m_render_pass && m_command_encoder)
		wgpuCommandEncoderPopDebugGroup(m_command_encoder);
#endif
}

void GSDeviceWebGPU::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
#ifdef ENABLE_OGL_DEBUG
	if (!GSConfig.UseDebugDevice)
		return;

	std::va_list ap;
	va_start(ap, fmt);
	const std::string buf(StringUtil::StdStringFromFormatV(fmt, ap));
	va_end(ap);

	if (buf.empty())
		return;

	if (m_render_pass)
		wgpuRenderPassEncoderInsertDebugMarker(m_render_pass, {buf.data(), buf.size()});
	else
		wgpuCommandEncoderInsertDebugMarker(GetCommandEncoder(), {buf.data(), buf.size()});
#endif
}

//////////////////////////////////////////////////////////////////////////
// Render passes
//////////////////////////////////////////////////////////////////////////

GSVector4 GSDeviceWebGPU::GetClearColorForTexture(const GSTextureWebGPU* tex)
{
	GSVector4 clear_color = tex->GetClearForFormat();
	if (tex->GetFormat() == GSTexture::Format::ColorClip && tex->GetWGPUFormat() == WGPUTextureFormat_RGBA16Unorm)
		clear_color *= GSVector4::cxpr(255.0f / 65535.0f, 255.0f / 65535.0f, 255.0f / 65535.0f, 1.0f);
	return clear_color;
}

GSDeviceWebGPU::RenderPassLoadOps GSDeviceWebGPU::GetLoadOpsForTargets(GSTextureWebGPU* rt, GSTextureWebGPU* ds, bool allow_discard_rt) const
{
	RenderPassLoadOps ops;
	if (rt)
	{
		switch (rt->GetState())
		{
			case GSTexture::State::Cleared:
				ops.rt_load = WGPULoadOp_Clear;
				ops.rt_clear = GetClearColorForTexture(rt);
				rt->SetState(GSTexture::State::Dirty);
				break;
			case GSTexture::State::Invalidated:
				ops.rt_load = WGPULoadOp_Clear;
				rt->SetState(GSTexture::State::Dirty);
				break;
			default:
				ops.rt_load = allow_discard_rt ? WGPULoadOp_Clear : WGPULoadOp_Load;
				break;
		}
	}
	if (ds)
	{
		switch (ds->GetState())
		{
			case GSTexture::State::Cleared:
				ops.ds_load = WGPULoadOp_Clear;
				ops.ds_clear = ds->GetClearDepth();
				ds->SetState(GSTexture::State::Dirty);
				break;
			case GSTexture::State::Invalidated:
				ops.ds_load = WGPULoadOp_Clear;
				ops.stencil_load = WGPULoadOp_Clear;
				ds->SetState(GSTexture::State::Dirty);
				break;
			default:
				break;
		}
	}
	return ops;
}

void GSDeviceWebGPU::BeginRenderPassWithViews(WGPUTextureView rt_view, WGPUTextureView ds_view, bool has_stencil, const RenderPassLoadOps& ops)
{
	EndRenderPass();

	WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	WGPURenderPassDepthStencilAttachment dsa = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
	WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	if (rt_view)
	{
		ca.view = rt_view;
		ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
		ca.loadOp = ops.rt_load;
		ca.storeOp = WGPUStoreOp_Store;
		ca.clearValue = {ops.rt_clear.x, ops.rt_clear.y, ops.rt_clear.z, ops.rt_clear.w};
		desc.colorAttachmentCount = 1;
		desc.colorAttachments = &ca;
	}
	if (ds_view)
	{
		dsa.view = ds_view;
		dsa.depthLoadOp = ops.ds_load;
		dsa.depthStoreOp = WGPUStoreOp_Store;
		dsa.depthClearValue = ops.ds_clear;
		if (has_stencil)
		{
			dsa.stencilLoadOp = ops.stencil_load;
			dsa.stencilStoreOp = WGPUStoreOp_Store;
			dsa.stencilClearValue = ops.stencil_clear;
		}
		desc.depthStencilAttachment = &dsa;
	}

	m_render_pass = wgpuCommandEncoderBeginRenderPass(GetCommandEncoder(), &desc);
	m_dirty_flags |= DIRTY_BASE_STATE | DIRTY_FLAG_TFX_TEXTURES | DIRTY_FLAG_TFX_UBO | DIRTY_FLAG_UTILITY_TEXTURE | DIRTY_FLAG_UTILITY_UNIFORM;
	m_current_pipeline_layout = PipelineLayout::Undefined;
}

void GSDeviceWebGPU::BeginRenderPass(GSTextureWebGPU* rt, GSTextureWebGPU* ds, const RenderPassLoadOps& ops)
{
	if (!rt && !ds)
	{
		EndRenderPass();
		Console.Warning("WebGPU: Render pass requested without any attachments.");
		return;
	}

	BeginRenderPassWithViews(rt ? rt->GetAttachmentView() : nullptr, ds ? ds->GetAttachmentView() : nullptr, ds && ds->HasStencil(), ops);
	m_pass_render_target = rt;
	m_pass_depth_target = ds;
	if (rt)
		rt->SetUseFenceCounter(m_current_fence_counter);
	if (ds)
		ds->SetUseFenceCounter(m_current_fence_counter);
}

void GSDeviceWebGPU::BeginRenderPassForTargets(GSTextureWebGPU* rt, GSTextureWebGPU* ds, bool clear_stencil_to_one)
{
	RenderPassLoadOps ops = GetLoadOpsForTargets(rt, ds, false);
	if (clear_stencil_to_one)
	{
		ops.stencil_load = WGPULoadOp_Clear;
		ops.stencil_clear = 1;
	}
	BeginRenderPass(rt, ds, ops);
}

void GSDeviceWebGPU::EndRenderPass()
{
	if (!m_render_pass)
		return;

	wgpuRenderPassEncoderEnd(m_render_pass);
	wgpuRenderPassEncoderRelease(m_render_pass);
	m_render_pass = nullptr;
	g_perfmon.Put(GSPerfMon::RenderPasses, 1);
}

void GSDeviceWebGPU::CommitClear(GSTextureWebGPU* tex)
{
	if (tex->GetState() != GSTexture::State::Cleared)
		return;

	pxAssert(tex->IsRenderTargetOrDepthStencil());
	EndRenderPass();

	GSTextureWebGPU* const rt = tex->IsDepthStencil() ? nullptr : tex;
	GSTextureWebGPU* const ds = tex->IsDepthStencil() ? tex : nullptr;
	const RenderPassLoadOps ops = GetLoadOpsForTargets(rt, ds, false);
	BeginRenderPass(rt, ds, ops);
	EndRenderPass();
	tex->SetState(GSTexture::State::Dirty);
}

void GSDeviceWebGPU::BeginRenderPassForStretchRect(GSTextureWebGPU* dTex, const GSVector4i& dtex_rc, const GSVector4i& dst_rc, bool allow_discard)
{
	pxAssert(dst_rc.x >= 0 && dst_rc.y >= 0 && dst_rc.z <= dTex->GetWidth() && dst_rc.w <= dTex->GetHeight());

	const bool discard = (allow_discard && dst_rc.eq(dtex_rc));
	GSTextureWebGPU* const rt = dTex->IsDepthStencil() ? nullptr : dTex;
	GSTextureWebGPU* const ds = dTex->IsDepthStencil() ? dTex : nullptr;
	RenderPassLoadOps ops = GetLoadOpsForTargets(rt, ds, discard);
	if (discard && ds)
		ops.ds_load = WGPULoadOp_Clear;
	dTex->SetState(GSTexture::State::Dirty);
	BeginRenderPass(rt, ds, ops);
}

void GSDeviceWebGPU::GenerateMipmaps(GSTextureWebGPU* tex)
{
	EndRenderPass();

	const UtilityPipeline& pipeline = m_convert[ShaderConvertSelector(ShaderConvert::COPY).Index()];
	for (int dst_level = 1; dst_level < tex->GetMipmapLevels(); dst_level++)
	{
		const int src_level = dst_level - 1;
		const int dst_width = std::max<int>(tex->GetWidth() >> dst_level, 1);
		const int dst_height = std::max<int>(tex->GetHeight() >> dst_level, 1);

		RenderPassLoadOps ops;
		ops.rt_load = WGPULoadOp_Clear;
		BeginRenderPassWithViews(tex->GetAttachmentView(dst_level), nullptr, false, ops);
		m_pass_render_target = nullptr;
		m_pass_depth_target = nullptr;
		m_current_render_target = nullptr;
		m_current_depth_target = nullptr;

		m_utility_texture_view_override = tex->GetLevelView(src_level);
		m_utility_texture = tex;
		m_utility_sampler = m_linear_sampler;
		m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
		SetPipeline(pipeline);
		SetViewport(GSVector4i(0, 0, dst_width, dst_height));
		SetScissor(GSVector4i(0, 0, dst_width, dst_height));
		DrawStretchRect(GSVector4(0.0f, 0.0f, 1.0f, 1.0f), GSVector4(0.0f, 0.0f, static_cast<float>(dst_width), static_cast<float>(dst_height)),
			GSVector2i(dst_width, dst_height));
		EndRenderPass();
	}

	m_utility_texture_view_override = nullptr;
	m_utility_texture = m_null_texture.get();
	m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	tex->SetUseFenceCounter(m_current_fence_counter);
}

//////////////////////////////////////////////////////////////////////////
// State
//////////////////////////////////////////////////////////////////////////

void GSDeviceWebGPU::InvalidateCachedState()
{
	EndRenderPass();
	m_dirty_flags = ALL_DIRTY_STATE;
	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		m_tfx_textures[i] = m_null_texture.get();
	m_utility_texture = m_null_texture.get();
	m_utility_texture_view_override = nullptr;
	m_current_render_target = nullptr;
	m_current_depth_target = nullptr;
	m_pass_render_target = nullptr;
	m_pass_depth_target = nullptr;
	m_current_pipeline_layout = PipelineLayout::Undefined;
	m_current_pipeline = nullptr;
}

void GSDeviceWebGPU::SetIndexBuffer(WGPUBuffer buffer)
{
	if (m_index_buffer == buffer)
		return;

	m_index_buffer = buffer;
	m_dirty_flags |= DIRTY_FLAG_INDEX_BUFFER;
}

void GSDeviceWebGPU::SetBlendConstants(u8 color)
{
	if (m_blend_constant_color == color)
		return;

	m_blend_constant_color = color;
	m_dirty_flags |= DIRTY_FLAG_BLEND_CONSTANTS;
}

void GSDeviceWebGPU::PSSetShaderResource(int i, GSTexture* sr, bool check_state)
{
	GSTextureWebGPU* tex = static_cast<GSTextureWebGPU*>(sr);
	if (tex)
	{
		if (check_state)
			tex->CommitClear();
		tex->SetUseFenceCounter(m_current_fence_counter);
	}
	else
	{
		tex = m_null_texture.get();
	}

	if (m_tfx_textures[i] == tex)
		return;

	m_tfx_textures[i] = tex;
	m_dirty_flags |= (DIRTY_FLAG_TFX_TEXTURE_0 << i);
}

void GSDeviceWebGPU::PSSetSampler(GSHWDrawConfig::SamplerSelector sel)
{
	if (m_tfx_sampler_sel == sel.key)
		return;

	m_tfx_sampler_sel = sel.key;
	m_tfx_sampler = GetSampler(sel);
	m_dirty_flags |= DIRTY_FLAG_TFX_TEXTURE_0;
}

void GSDeviceWebGPU::SetUtilityTexture(GSTexture* tex, WGPUSampler sampler)
{
	GSTextureWebGPU* wtex = static_cast<GSTextureWebGPU*>(tex);
	if (wtex)
	{
		wtex->CommitClear();
		wtex->SetUseFenceCounter(m_current_fence_counter);
	}
	else
	{
		wtex = m_null_texture.get();
	}

	if (m_utility_texture == wtex && m_utility_sampler == sampler && !m_utility_texture_view_override)
		return;

	m_utility_texture = wtex;
	m_utility_sampler = sampler;
	m_utility_texture_view_override = nullptr;
	m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
}

void GSDeviceWebGPU::SetUtilityPushConstants(const void* data, u32 size)
{
	pxAssert(size <= UTILITY_UNIFORM_SIZE);
	std::memcpy(m_utility_uniform_data, data, size);
	m_utility_uniform_size = size;
	m_dirty_flags |= DIRTY_FLAG_UTILITY_UNIFORM;
}

void GSDeviceWebGPU::SetViewport(const GSVector4i& viewport)
{
	if (m_viewport.eq(viewport))
		return;

	m_viewport = viewport;
	m_dirty_flags |= DIRTY_FLAG_VIEWPORT;
}

void GSDeviceWebGPU::SetScissor(const GSVector4i& scissor)
{
	if (m_scissor.eq(scissor))
		return;

	m_scissor = scissor;
	m_dirty_flags |= DIRTY_FLAG_SCISSOR;
}

void GSDeviceWebGPU::SetPipeline(WGPURenderPipeline pipeline)
{
	if (m_current_pipeline == pipeline)
		return;

	m_current_pipeline = pipeline;
	m_dirty_flags |= DIRTY_FLAG_PIPELINE;
}

void GSDeviceWebGPU::SetPipeline(const UtilityPipeline& pipeline)
{
	const u32 layout = pipeline.depth_input ? 1 : 0;
	if (m_utility_layout != layout)
	{
		m_utility_layout = layout;
		m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}

	SetPipeline(pipeline.pipeline);
}

void GSDeviceWebGPU::OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i& scissor)
{
	GSTextureWebGPU* wrt = static_cast<GSTextureWebGPU*>(rt);
	GSTextureWebGPU* wds = static_cast<GSTextureWebGPU*>(ds);

	if (m_current_render_target != wrt || m_current_depth_target != wds)
	{
		EndRenderPass();
	}
	else if (InRenderPass())
	{
		bool restart = false;
		if (wrt && wrt->GetState() != GSTexture::State::Dirty)
		{
			if (wrt->GetState() == GSTexture::State::Cleared)
				restart = true;
			else
				wrt->SetState(GSTexture::State::Dirty);
		}
		if (wds && wds->GetState() != GSTexture::State::Dirty)
		{
			if (wds->GetState() == GSTexture::State::Cleared)
				restart = true;
			else
				wds->SetState(GSTexture::State::Dirty);
		}
		if (restart)
			EndRenderPass();
	}

	m_current_render_target = wrt;
	m_current_depth_target = wds;
	if (wrt)
		wrt->SetUseFenceCounter(m_current_fence_counter);
	if (wds)
		wds->SetUseFenceCounter(m_current_fence_counter);

	const GSVector2i size = wrt ? wrt->GetSize() : (wds ? wds->GetSize() : GSVector2i(1, 1));
	SetViewport(GSVector4i(0, 0, size.x, size.y));
	SetScissor(scissor);
}

void GSDeviceWebGPU::SetVSConstantBuffer(const GSHWDrawConfig::VSConstantBuffer& cb)
{
	if (m_vs_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_VS_CONSTANT_BUFFER;
}

void GSDeviceWebGPU::SetPSConstantBuffer(const GSHWDrawConfig::PSConstantBuffer& cb)
{
	if (m_ps_cb_cache.Update(cb))
		m_dirty_flags |= DIRTY_FLAG_PS_CONSTANT_BUFFER;
}

bool GSDeviceWebGPU::SetVSPushConstants(u32 base_vertex, u32 base_index, bool force_update)
{
	GSHWDrawConfig::VSPushConstants pc;
	pc.base_vertex = base_vertex;
	pc.base_index = base_index;

	if (m_vs_pc_cache.Update(pc) || force_update)
		m_dirty_flags |= DIRTY_FLAG_VS_PUSH_CONSTANTS;

	return ApplyTFXState();
}

void GSDeviceWebGPU::UnbindTexture(GSTextureWebGPU* tex)
{
	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
	{
		if (m_tfx_textures[i] == tex)
		{
			m_tfx_textures[i] = m_null_texture.get();
			m_dirty_flags |= (DIRTY_FLAG_TFX_TEXTURE_0 << i);
		}
	}
	if (m_utility_texture == tex)
	{
		m_utility_texture = m_null_texture.get();
		m_utility_texture_view_override = nullptr;
		m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}
	if (m_current_render_target == tex || m_current_depth_target == tex || m_pass_render_target == tex || m_pass_depth_target == tex)
	{
		EndRenderPass();
		m_current_render_target = nullptr;
		m_current_depth_target = nullptr;
		m_pass_render_target = nullptr;
		m_pass_depth_target = nullptr;
	}

	const WGPUTextureView view = tex->GetView();
	if (!view)
		return;

	for (auto it = m_tfx_bind_groups.begin(); it != m_tfx_bind_groups.end();)
	{
		bool match = false;
		for (WGPUTextureView key_view : it->first.views)
			match |= (key_view == view);
		if (match)
		{
			wgpuBindGroupRelease(it->second);
			it = m_tfx_bind_groups.erase(it);
		}
		else
		{
			++it;
		}
	}
	for (auto it = m_utility_bind_groups.begin(); it != m_utility_bind_groups.end();)
	{
		bool match = (it->first.view == view);
		for (int level = 0; !match && level < tex->GetMipmapLevels(); level++)
			match |= (it->first.view == tex->GetLevelView(level));
		if (match)
		{
			wgpuBindGroupRelease(it->second);
			it = m_utility_bind_groups.erase(it);
		}
		else
		{
			++it;
		}
	}
}

WGPUBindGroup GSDeviceWebGPU::GetTFXTextureBindGroup()
{
	TFXBindGroupKey key;
	for (u32 i = 0; i < NUM_TFX_TEXTURES; i++)
		key.views[i] = m_tfx_textures[i]->GetView();
	key.sampler = (m_tfx_texture_layout != 0) ? m_point_sampler : m_tfx_sampler;
	key.layout = m_tfx_texture_layout;

	const auto it = m_tfx_bind_groups.find(key);
	if (it != m_tfx_bind_groups.end())
		return it->second;

	if (m_tfx_bind_groups.size() >= MAX_BIND_GROUP_CACHE_SIZE)
	{
		for (auto& bg : m_tfx_bind_groups)
			wgpuBindGroupRelease(bg.second);
		m_tfx_bind_groups.clear();
	}

	std::array<WGPUBindGroupEntry, 6> entries = {};
	for (WGPUBindGroupEntry& entry : entries)
		entry = WGPU_BIND_GROUP_ENTRY_INIT;
	entries[0].binding = 0;
	entries[0].textureView = key.views[TFX_TEXTURE_TEXTURE];
	entries[1].binding = 1;
	entries[1].sampler = key.sampler;
	entries[2].binding = 2;
	entries[2].textureView = key.views[TFX_TEXTURE_PALETTE];
	entries[3].binding = 3;
	entries[3].textureView = key.views[TFX_TEXTURE_RT];
	entries[4].binding = 4;
	entries[4].textureView = key.views[TFX_TEXTURE_PRIMID];
	entries[5].binding = 5;
	entries[5].textureView = key.views[TFX_TEXTURE_DEPTH];

	WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	desc.layout = m_tfx_texture_bind_group_layouts[key.layout];
	desc.entryCount = entries.size();
	desc.entries = entries.data();
	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(m_device, &desc);
	m_tfx_bind_groups.emplace(key, bg);
	return bg;
}

WGPUBindGroup GSDeviceWebGPU::GetUtilityBindGroup(WGPUTextureView view, WGPUSampler sampler, u32 layout)
{
	if (layout != 0)
		sampler = m_point_sampler;

	const UtilityBindGroupKey key = {view, sampler, layout};
	const auto it = m_utility_bind_groups.find(key);
	if (it != m_utility_bind_groups.end())
		return it->second;

	if (m_utility_bind_groups.size() >= MAX_BIND_GROUP_CACHE_SIZE)
	{
		for (auto& bg : m_utility_bind_groups)
			wgpuBindGroupRelease(bg.second);
		m_utility_bind_groups.clear();
	}

	std::array<WGPUBindGroupEntry, 3> entries = {};
	for (WGPUBindGroupEntry& entry : entries)
		entry = WGPU_BIND_GROUP_ENTRY_INIT;
	entries[0].binding = 0;
	entries[0].textureView = view;
	entries[1].binding = 1;
	entries[1].sampler = sampler;
	entries[2].binding = 2;
	entries[2].buffer = m_vertex_uniform_stream_buffer.GetBuffer();
	entries[2].offset = 0;
	entries[2].size = UTILITY_UNIFORM_SIZE;

	WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	desc.layout = m_utility_bind_group_layouts[layout];
	desc.entryCount = entries.size();
	desc.entries = entries.data();
	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(m_device, &desc);
	m_utility_bind_groups.emplace(key, bg);
	return bg;
}

void GSDeviceWebGPU::ApplyBaseState(u32 flags)
{
	if (flags & DIRTY_FLAG_PIPELINE)
		wgpuRenderPassEncoderSetPipeline(m_render_pass, m_current_pipeline);

	if (flags & DIRTY_FLAG_VERTEX_BUFFER)
		wgpuRenderPassEncoderSetVertexBuffer(m_render_pass, 0, m_vertex_stream_buffer.GetBuffer(), 0, WGPU_WHOLE_SIZE);

	if ((flags & DIRTY_FLAG_INDEX_BUFFER) && m_index_buffer)
		wgpuRenderPassEncoderSetIndexBuffer(m_render_pass, m_index_buffer, WGPUIndexFormat_Uint16, 0, WGPU_WHOLE_SIZE);

	const GSTextureWebGPU* const target = m_pass_render_target ? m_pass_render_target : m_pass_depth_target;
	const GSVector2i target_size = target ? target->GetSize() : (m_surface_texture ? m_surface_texture->GetSize() : GSVector2i(1, 1));

	if (flags & DIRTY_FLAG_VIEWPORT)
	{
		const GSVector4i vp = m_viewport.rintersect(GSVector4i(0, 0, target_size.x, target_size.y));
		wgpuRenderPassEncoderSetViewport(m_render_pass, static_cast<float>(vp.x), static_cast<float>(vp.y),
			static_cast<float>(std::max(vp.width(), 1)), static_cast<float>(std::max(vp.height(), 1)), 0.0f, 1.0f);
	}

	if (flags & DIRTY_FLAG_SCISSOR)
	{
		const GSVector4i sc = m_scissor.rintersect(GSVector4i(0, 0, target_size.x, target_size.y));
		if (sc.width() > 0 && sc.height() > 0)
			wgpuRenderPassEncoderSetScissorRect(m_render_pass, sc.x, sc.y, sc.width(), sc.height());
		else
			wgpuRenderPassEncoderSetScissorRect(m_render_pass, 0, 0, 0, 0);
	}

	if (flags & DIRTY_FLAG_BLEND_CONSTANTS)
	{
		const double col = static_cast<double>(m_blend_constant_color) / 128.0;
		const WGPUColor color = {col, col, col, col};
		wgpuRenderPassEncoderSetBlendConstant(m_render_pass, &color);
	}

	if (flags & DIRTY_FLAG_STENCIL_REF)
		wgpuRenderPassEncoderSetStencilReference(m_render_pass, 1);
}

bool GSDeviceWebGPU::ApplyTFXState(bool already_execed)
{
	if (m_current_pipeline_layout == PipelineLayout::TFX && m_dirty_flags == 0)
		return true;

	if (!m_render_pass)
		return false;

	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~(DIRTY_TFX_STATE | DIRTY_CONSTANT_BUFFER_STATE);

	if (flags & DIRTY_FLAG_VS_CONSTANT_BUFFER)
	{
		if (!m_vertex_uniform_stream_buffer.ReserveMemory(sizeof(m_vs_cb_cache), UNIFORM_OFFSET_ALIGNMENT))
		{
			if (already_execed)
			{
				Console.Error("WebGPU: Failed to reserve vertex uniform space");
				return false;
			}

			m_dirty_flags |= flags;
			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of vertex uniform space");
			return ApplyTFXState(true);
		}

		std::memcpy(m_vertex_uniform_stream_buffer.GetCurrentHostPointer(), &m_vs_cb_cache, sizeof(m_vs_cb_cache));
		m_tfx_dynamic_offsets[0] = m_vertex_uniform_stream_buffer.GetCurrentOffset();
		m_vertex_uniform_stream_buffer.CommitMemory(sizeof(m_vs_cb_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	if (flags & DIRTY_FLAG_PS_CONSTANT_BUFFER)
	{
		if (!m_fragment_uniform_stream_buffer.ReserveMemory(sizeof(m_ps_cb_cache), UNIFORM_OFFSET_ALIGNMENT))
		{
			if (already_execed)
			{
				Console.Error("WebGPU: Failed to reserve pixel uniform space");
				return false;
			}

			m_dirty_flags |= flags;
			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of pixel uniform space");
			return ApplyTFXState(true);
		}

		std::memcpy(m_fragment_uniform_stream_buffer.GetCurrentHostPointer(), &m_ps_cb_cache, sizeof(m_ps_cb_cache));
		m_tfx_dynamic_offsets[1] = m_fragment_uniform_stream_buffer.GetCurrentOffset();
		m_fragment_uniform_stream_buffer.CommitMemory(sizeof(m_ps_cb_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	if (flags & DIRTY_FLAG_VS_PUSH_CONSTANTS)
	{
		if (!m_vertex_uniform_stream_buffer.ReserveMemory(sizeof(m_vs_pc_cache), UNIFORM_OFFSET_ALIGNMENT))
		{
			if (already_execed)
			{
				Console.Error("WebGPU: Failed to reserve vertex push constant space");
				return false;
			}

			m_dirty_flags |= flags;
			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of vertex uniform space");
			return ApplyTFXState(true);
		}

		std::memcpy(m_vertex_uniform_stream_buffer.GetCurrentHostPointer(), &m_vs_pc_cache, sizeof(m_vs_pc_cache));
		m_tfx_dynamic_offsets[2] = m_vertex_uniform_stream_buffer.GetCurrentOffset();
		m_vertex_uniform_stream_buffer.CommitMemory(sizeof(m_vs_pc_cache));
		flags |= DIRTY_FLAG_TFX_UBO;
	}

	if (m_current_pipeline_layout != PipelineLayout::TFX)
	{
		m_current_pipeline_layout = PipelineLayout::TFX;
		flags |= DIRTY_FLAG_TFX_UBO | DIRTY_FLAG_TFX_TEXTURES;
	}

	if (flags & DIRTY_FLAG_TFX_UBO)
		wgpuRenderPassEncoderSetBindGroup(m_render_pass, 0, m_tfx_ubo_bind_group, NUM_TFX_DYNAMIC_OFFSETS, m_tfx_dynamic_offsets.data());

	if (flags & DIRTY_FLAG_TFX_TEXTURES)
		wgpuRenderPassEncoderSetBindGroup(m_render_pass, 1, GetTFXTextureBindGroup(), 0, nullptr);

	ApplyBaseState(flags);
	return true;
}

bool GSDeviceWebGPU::ApplyUtilityState(bool already_execed)
{
	if (m_current_pipeline_layout == PipelineLayout::Utility && m_dirty_flags == 0)
		return true;

	if (!m_render_pass)
		return false;

	u32 flags = m_dirty_flags;
	m_dirty_flags &= ~DIRTY_UTILITY_STATE;

	if (flags & DIRTY_FLAG_UTILITY_UNIFORM)
	{
		if (!m_vertex_uniform_stream_buffer.ReserveMemory(UTILITY_UNIFORM_SIZE, UNIFORM_OFFSET_ALIGNMENT))
		{
			if (already_execed)
			{
				Console.Error("WebGPU: Failed to reserve utility uniform space");
				return false;
			}

			m_dirty_flags |= flags;
			ExecuteCommandBufferAndRestartRenderPass(false, "Ran out of utility uniform space");
			return ApplyUtilityState(true);
		}

		std::memcpy(m_vertex_uniform_stream_buffer.GetCurrentHostPointer(), m_utility_uniform_data, UTILITY_UNIFORM_SIZE);
		m_utility_uniform_offset = m_vertex_uniform_stream_buffer.GetCurrentOffset();
		m_vertex_uniform_stream_buffer.CommitMemory(UTILITY_UNIFORM_SIZE);
		flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}

	if (m_current_pipeline_layout != PipelineLayout::Utility || (flags & DIRTY_FLAG_UTILITY_TEXTURE))
	{
		m_current_pipeline_layout = PipelineLayout::Utility;
		const WGPUTextureView view = m_utility_texture_view_override ? m_utility_texture_view_override : m_utility_texture->GetView();
		wgpuRenderPassEncoderSetBindGroup(m_render_pass, 0, GetUtilityBindGroup(view, m_utility_sampler, m_utility_layout), 1, &m_utility_uniform_offset);
	}

	ApplyBaseState(flags);
	return true;
}

//////////////////////////////////////////////////////////////////////////
// Draws
//////////////////////////////////////////////////////////////////////////

void GSDeviceWebGPU::DrawPrimitive()
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	wgpuRenderPassEncoderDraw(m_render_pass, m_vertex.count, 1, m_vertex.start, 0);
}

void GSDeviceWebGPU::DrawIndexedPrimitive()
{
	DrawIndexedPrimitive(0, m_index.count);
}

void GSDeviceWebGPU::DrawIndexedPrimitive(int offset, int count)
{
	pxAssert(offset + count <= (int)m_index.count);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	wgpuRenderPassEncoderDrawIndexed(m_render_pass, count, 1, m_index.start + offset, m_vertex.start, 0);
}

void GSDeviceWebGPU::DrawIndexedPrimitiveVSExpand(int offset, int count, bool vs_indexing, int vs_indexing_expansion)
{
	pxAssert(offset + count <= (int)m_index.count);

	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	if (vs_indexing)
	{
		if (!SetVSPushConstants(m_vertex.start, m_index.start + offset))
			return;
		wgpuRenderPassEncoderDraw(m_render_pass, count * vs_indexing_expansion, 1, 0, 0);
	}
	else
	{
		if (!SetVSPushConstants(m_vertex.start))
			return;
		wgpuRenderPassEncoderDrawIndexed(m_render_pass, count, 1, m_index.start + offset, 0, 0);
	}
}

void GSDeviceWebGPU::Draw(const GSHWDrawConfig& config, int offset, int count)
{
	if (!InRenderPass())
	{
		if (!m_current_render_target && !m_current_depth_target)
			return;

		BeginRenderPassForTargets(m_current_render_target, m_current_depth_target, false);
	}

	if (!ApplyTFXState())
		return;

	if (config.vs.expand != GSHWDrawConfig::VSExpand::None)
	{
		const bool vs_indexing = config.vs.UseVSExpandIndexBuffer();
		const u32 vs_indexing_expansion = GetExpansionFactor(config.vs.expand);
		DrawIndexedPrimitiveVSExpand(offset, count, vs_indexing, vs_indexing_expansion);
	}
	else
	{
		DrawIndexedPrimitive(offset, count);
	}
}

void GSDeviceWebGPU::Draw(const GSHWDrawConfig& config)
{
	Draw(config, 0, m_index.count);
}

void GSDeviceWebGPU::IASetVertexBuffer(const void* vertex, size_t stride, size_t count, size_t align_multiplier)
{
	const u32 size = static_cast<u32>(stride) * static_cast<u32>(count);
	if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride) * align_multiplier))
	{
		ExecuteCommandBufferAndRestartRenderPass(false, "Uploading bytes to vertex buffer");
		if (!m_vertex_stream_buffer.ReserveMemory(size, static_cast<u32>(stride) * align_multiplier))
			pxFailRel("Failed to reserve space for vertices");
	}

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / stride;
	m_vertex.count = count;

	std::memcpy(m_vertex_stream_buffer.GetCurrentHostPointer(), vertex, size);
	m_vertex_stream_buffer.CommitMemory(size);
}

void GSDeviceWebGPU::UploadIndices(WebGPUStreamBuffer& buffer, const void* index, size_t count)
{
	const u32 size = sizeof(u16) * static_cast<u32>(count);
	if (!buffer.ReserveMemory(size, sizeof(u32)))
	{
		ExecuteCommandBufferAndRestartRenderPass(false, "Uploading bytes to index buffer");
		if (!buffer.ReserveMemory(size, sizeof(u32)))
			pxFailRel("Failed to reserve space for indices");
	}

	m_index.start = buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = count;

	std::memcpy(buffer.GetCurrentHostPointer(), index, size);
	buffer.CommitMemory(size);
}

void GSDeviceWebGPU::IASetIndexBuffer(const void* index, size_t count)
{
	UploadIndices(m_index_stream_buffer, index, count);
	SetIndexBuffer(m_index_stream_buffer.GetBuffer());
}

void GSDeviceWebGPU::VSSetIndexBuffer(const void* index, size_t count)
{
	UploadIndices(m_expand_index_stream_buffer, index, count);
}

//////////////////////////////////////////////////////////////////////////
// Textures and copies
//////////////////////////////////////////////////////////////////////////

GSTexture* GSDeviceWebGPU::CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format)
{
	std::unique_ptr<GSTexture> tex = GSTextureWebGPU::Create(usage, format, width, height, levels);
	if (!tex)
	{
		PurgePool();
		ExecuteCommandBufferAndRestartRenderPass(true, "Couldn't allocate texture.");
		tex = GSTextureWebGPU::Create(usage, format, width, height, levels);
	}

	return tex.release();
}

std::unique_ptr<GSDownloadTexture> GSDeviceWebGPU::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTextureWebGPU::Create(width, height, format);
}

void GSDeviceWebGPU::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	if (r.rempty())
	{
		GL_INS("WebGPU: CopyRect rect empty.");
		return;
	}

	GSTextureWebGPU* const src = static_cast<GSTextureWebGPU*>(sTex);
	GSTextureWebGPU* const dst = static_cast<GSTextureWebGPU*>(dTex);
	const GSVector4i dst_rect(0, 0, dst->GetWidth(), dst->GetHeight());
	const bool full_draw_copy = dst_rect.eq(r);

	if (src->GetState() == GSTexture::State::Cleared)
	{
		if (dst->IsRenderTargetOrDepthStencil() && ProcessClearsBeforeCopy(sTex, dTex, full_draw_copy))
			return;

		src->CommitClear();
	}

	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	if (dst->GetState() == GSTexture::State::Cleared && !full_draw_copy)
		dst->CommitClear();

	EndRenderPass();

	src->SetUseFenceCounter(m_current_fence_counter);
	dst->SetUseFenceCounter(m_current_fence_counter);

	const WGPUExtent3D extent = {static_cast<u32>(r.width()), static_cast<u32>(r.height()), 1u};
	if (src == dst)
	{
		GSTextureWebGPU* const tmp = static_cast<GSTextureWebGPU*>(CreateTexture(r.width(), r.height(), 1, src->GetFormat(), true));
		if (!tmp)
		{
			Console.Error("WebGPU: Failed to allocate temporary texture for self copy.");
			return;
		}

		WGPUTexelCopyTextureInfo copy_src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		copy_src.texture = src->GetTexture();
		copy_src.origin = {static_cast<u32>(r.left), static_cast<u32>(r.top), 0u};
		copy_src.aspect = WGPUTextureAspect_All;
		WGPUTexelCopyTextureInfo copy_tmp = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		copy_tmp.texture = tmp->GetTexture();
		copy_tmp.aspect = WGPUTextureAspect_All;
		wgpuCommandEncoderCopyTextureToTexture(GetCommandEncoder(), &copy_src, &copy_tmp, &extent);

		WGPUTexelCopyTextureInfo copy_dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		copy_dst.texture = dst->GetTexture();
		copy_dst.origin = {destX, destY, 0u};
		copy_dst.aspect = WGPUTextureAspect_All;
		wgpuCommandEncoderCopyTextureToTexture(GetCommandEncoder(), &copy_tmp, &copy_dst, &extent);
		tmp->SetUseFenceCounter(m_current_fence_counter);
		Recycle(tmp);
	}
	else
	{
		WGPUTexelCopyTextureInfo copy_src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		copy_src.texture = src->GetTexture();
		copy_src.origin = {static_cast<u32>(r.left), static_cast<u32>(r.top), 0u};
		copy_src.aspect = WGPUTextureAspect_All;
		WGPUTexelCopyTextureInfo copy_dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		copy_dst.texture = dst->GetTexture();
		copy_dst.origin = {destX, destY, 0u};
		copy_dst.aspect = WGPUTextureAspect_All;
		wgpuCommandEncoderCopyTextureToTexture(GetCommandEncoder(), &copy_src, &copy_dst, &extent);
	}

	dst->SetState(GSTexture::State::Dirty);
}

void GSDeviceWebGPU::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvertSelector shader, Filter filter)
{
	pxAssert(dTex);
	filter = shader.SupportsBilinear() ? Nearest : filter;
	const bool allow_discard = (shader.Mask() == 0xf);
	DoStretchRect(static_cast<GSTextureWebGPU*>(sTex), sRect, static_cast<GSTextureWebGPU*>(dTex), dRect,
		m_convert[shader.Index()], filter, allow_discard);
}

void GSDeviceWebGPU::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, const GSVector4& dRect,
	PresentShader shader, Filter filter)
{
	DoStretchRect(static_cast<GSTextureWebGPU*>(sTex), sRect, nullptr, dRect, m_present[static_cast<u32>(shader)], filter, true);
}

void GSDeviceWebGPU::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, Filter filter)
{
	DisplayConstantBuffer cb;
	cb.SetSource(sRect, sTex->GetSize());
	cb.SetTarget(dRect, dTex ? dTex->GetSize() : GSVector2i(GetWindowWidth(), GetWindowHeight()));
	cb.SetTime(shaderTime);
	SetUtilityPushConstants(&cb, sizeof(cb));

	const UtilityPipeline& pipeline = dTex ? m_present_offscreen[static_cast<int>(shader)] : m_present[static_cast<int>(shader)];
	DoStretchRect(static_cast<GSTextureWebGPU*>(sTex), sRect, static_cast<GSTextureWebGPU*>(dTex), dRect, pipeline, filter, true);
}

void GSDeviceWebGPU::DrawMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvertSelector shader)
{
	GSTexture* last_tex = rects[0].src;
	Filter last_filter = rects[0].filter;
	u8 last_wmask = rects[0].wmask.wrgba;

	u32 first = 0;
	u32 count = 1;

	for (u32 i = 0; i < num_rects; i++)
		static_cast<GSTextureWebGPU*>(rects[i].src)->CommitClear();

	for (u32 i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].filter == last_filter && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		DoMultiStretchRects(rects + first, count, static_cast<GSTextureWebGPU*>(dTex), shader);
		last_tex = rects[i].src;
		last_filter = rects[i].filter;
		last_wmask = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	DoMultiStretchRects(rects + first, count, static_cast<GSTextureWebGPU*>(dTex), shader);
}

void GSDeviceWebGPU::DoMultiStretchRects(
	const MultiStretchRect* rects, u32 num_rects, GSTextureWebGPU* dTex, ShaderConvertSelector shader)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	const u32 vertex_reserve_size = num_rects * 4 * sizeof(GSVertexPT1);
	const u32 index_reserve_size = num_rects * 6 * sizeof(u16);
	if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
		!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u32)))
	{
		ExecuteCommandBufferAndRestartRenderPass(false, "Uploading bytes to vertex buffer");
		if (!m_vertex_stream_buffer.ReserveMemory(vertex_reserve_size, sizeof(GSVertexPT1)) ||
			!m_index_stream_buffer.ReserveMemory(index_reserve_size, sizeof(u32)))
		{
			pxFailRel("Failed to reserve space for vertices");
		}
	}

	const GSVector2 ds(static_cast<float>(dTex->GetWidth()), static_cast<float>(dTex->GetHeight()));
	GSVertexPT1* verts = reinterpret_cast<GSVertexPT1*>(m_vertex_stream_buffer.GetCurrentHostPointer());
	u16* idx = reinterpret_cast<u16*>(m_index_stream_buffer.GetCurrentHostPointer());
	u32 icount = 0;
	u32 vcount = 0;
	for (u32 i = 0; i < num_rects; i++)
	{
		const GSVector4& sRect = rects[i].src_rect;
		const GSVector4& dRect = rects[i].dst_rect;

		const float inv_x = 2.0f / ds.x;
		const float inv_y = 2.0f / ds.y;

		const float left = dRect.x * inv_x - 1.0f;
		const float right = dRect.z * inv_x - 1.0f;
		const float top = 1.0f - dRect.y * inv_y;
		const float bottom = 1.0f - dRect.w * inv_y;

		const u32 vstart = vcount;
		verts[vcount++] = {GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)};
		verts[vcount++] = {GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)};
		verts[vcount++] = {GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)};
		verts[vcount++] = {GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)};

		if (i > 0)
			idx[icount++] = vstart;

		idx[icount++] = vstart;
		idx[icount++] = vstart + 1;
		idx[icount++] = vstart + 2;
		idx[icount++] = vstart + 3;
		idx[icount++] = vstart + 3;
	};

	m_vertex.start = m_vertex_stream_buffer.GetCurrentOffset() / sizeof(GSVertexPT1);
	m_vertex.count = vcount;
	m_index.start = m_index_stream_buffer.GetCurrentOffset() / sizeof(u16);
	m_index.count = icount;
	m_vertex_stream_buffer.CommitMemory(vcount * sizeof(GSVertexPT1));
	m_index_stream_buffer.CommitMemory(Common::AlignUpPow2(icount * sizeof(u16), 4));
	SetIndexBuffer(m_index_stream_buffer.GetBuffer());

	const GSVector4i rc(dTex->GetRect());
	OMSetRenderTargets(dTex->IsRenderTarget() ? dTex : nullptr, dTex->IsDepthStencil() ? dTex : nullptr, rc);
	if (!InRenderPass())
		BeginRenderPassForStretchRect(dTex, rc, rc, false);
	SetUtilityTexture(rects[0].src, rects[0].filter == Biln ? m_linear_sampler : m_point_sampler);
	SetPipeline(m_convert[shader.SetMask(rects[0].wmask.wrgba).Index()]);

	if (ApplyUtilityState())
		DrawIndexedPrimitive();
}

void GSDeviceWebGPU::DoStretchRect(GSTextureWebGPU* sTex, const GSVector4& sRect, GSTextureWebGPU* dTex, const GSVector4& dRect,
	const UtilityPipeline& pipeline, Filter filter, bool allow_discard)
{
	SetUtilityTexture(sTex, filter == Biln ? m_linear_sampler : m_point_sampler);
	SetPipeline(pipeline);

	const bool is_present = (!dTex);
	const bool depth = (dTex && dTex->IsDepthStencil());
	const GSVector2i size(is_present ? GSVector2i(GetWindowWidth(), GetWindowHeight()) : dTex->GetSize());
	const GSVector4i dtex_rc(0, 0, size.x, size.y);
	const GSVector4i dst_rc(GSVector4i(dRect).rintersect(dtex_rc));

	if (!is_present)
	{
		OMSetRenderTargets(depth ? nullptr : dTex, depth ? dTex : nullptr, dst_rc);
		if (InRenderPass() && dTex->GetState() == GSTexture::State::Cleared)
			EndRenderPass();
	}
	else
	{
		m_dirty_flags &= ~(DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);
	}

	if (!is_present && !InRenderPass())
		BeginRenderPassForStretchRect(dTex, dtex_rc, dst_rc, allow_discard);

	DrawStretchRect(sRect, dRect, size);
}

void GSDeviceWebGPU::DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	const float inv_x = 2.0f / ds.x;
	const float inv_y = 2.0f / ds.y;

	const float left = dRect.x * inv_x - 1.0f;
	const float right = dRect.z * inv_x - 1.0f;
	const float top = 1.0f - dRect.y * inv_y;
	const float bottom = 1.0f - dRect.w * inv_y;

	const GSVertexPT1 vertices[] = {
		{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
		{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
		{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
		{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
	};
	IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));

	if (ApplyUtilityState())
		DrawPrimitive();
}

void GSDeviceWebGPU::UpdateCLUTTexture(
	GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
{
	struct alignas(16) Uniforms
	{
		u32 offsetX, offsetY, dOffset, pad1;
		float scale;
		float pad2[3];
	};

	const Uniforms uniforms = {offsetX, offsetY, dOffset, 0, sScale, {}};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const GSVector4 dRect(0, 0, dSize, 1);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	DoStretchRect(static_cast<GSTextureWebGPU*>(sTex), GSVector4::zero(), static_cast<GSTextureWebGPU*>(dTex), dRect,
		m_convert[ShaderConvertSelector(shader).Index()], Nearest, true);
}

void GSDeviceWebGPU::ConvertToIndexedTexture(
	GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	struct alignas(16) Uniforms
	{
		u32 SBW;
		u32 DBW;
		u32 PSM;
		u32 pad1[1];
		float ScaleFactor;
		float pad2[3];
	};

	const Uniforms uniforms = {SBW, DBW, SPSM, {}, sScale, {}};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	const ShaderConvert shader = ((SPSM & 0xE) == 0) ? ShaderConvert::RGBA_TO_8I : ShaderConvert::RGB5A1_TO_8I;
	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	DoStretchRect(static_cast<GSTextureWebGPU*>(sTex), GSVector4::zero(), static_cast<GSTextureWebGPU*>(dTex), dRect,
		m_convert[ShaderConvertSelector(shader).Index()], Nearest, true);
}

void GSDeviceWebGPU::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect)
{
	struct alignas(16) Uniforms
	{
		GSVector2i clamp_min;
		int downsample_factor;
		int pad0;
		float weight;
		float step_multiplier;
		float pad1[2];
	};

	const Uniforms uniforms = {
		clamp_min, static_cast<int>(downsample_factor), 0, static_cast<float>(downsample_factor * downsample_factor), (GSConfig.UserHacks_NativeScaling > GSNativeScaling::Aggressive) ? 2.0f : 1.0f};
	SetUtilityPushConstants(&uniforms, sizeof(uniforms));

	DoStretchRect(static_cast<GSTextureWebGPU*>(sTex), GSVector4::zero(), static_cast<GSTextureWebGPU*>(dTex), dRect,
		m_convert[ShaderConvertSelector(ShaderConvert::DOWNSAMPLE_COPY).Index()], Nearest, true);
}

void GSDeviceWebGPU::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter)
{
	GL_PUSH("DoMerge");

	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const u32 yuv_constants[4] = {EXTBUF.EMODA, EXTBUF.EMODC};
	const GSVector4 bg_color = GSVector4::unorm8(c);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;
	WGPUSampler sampler = (filter == Biln) ? m_linear_sampler : m_point_sampler;
	GSTextureWebGPU* const dst = static_cast<GSTextureWebGPU*>(dTex);
	const UtilityPipeline& copy_pipeline = m_convert[ShaderConvertSelector(ShaderConvert::COPY).Index()];
	const UtilityPipeline& yuv_pipeline = m_convert[ShaderConvertSelector(ShaderConvert::YUV).Index()];

	EndRenderPass();

	const bool has_input_0 = (sTex[0] &&
		(sTex[0]->GetState() == GSTexture::State::Dirty || (sTex[0]->GetState() == GSTexture::State::Cleared || sTex[0]->GetClearColor() != 0)));
	const bool has_input_1 = (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg) && sTex[1] &&
		(sTex[1]->GetState() == GSTexture::State::Dirty || (sTex[1]->GetState() == GSTexture::State::Cleared || sTex[1]->GetClearColor() != 0));
	if (has_input_0)
		static_cast<GSTextureWebGPU*>(sTex[0])->CommitClear();
	if (has_input_1)
		static_cast<GSTextureWebGPU*>(sTex[1])->CommitClear();

	const GSVector2i dsize(dTex->GetSize());
	const GSVector4i darea(0, 0, dsize.x, dsize.y);
	bool dcleared = false;
	if (sTex[1] && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		if (sTex[1]->GetState() == GSTexture::State::Dirty)
		{
			OMSetRenderTargets(dTex, nullptr, darea);
			SetUtilityTexture(sTex[1], sampler);
			RenderPassLoadOps ops;
			ops.rt_load = WGPULoadOp_Clear;
			ops.rt_clear = GSVector4::unorm8(c);
			BeginRenderPass(dst, nullptr, ops);
			SetPipeline(copy_pipeline);
			DrawStretchRect(sRect[1], PMODE.SLBG ? dRect[2] : dRect[1], dsize);
			dTex->SetState(GSTexture::State::Dirty);
			dcleared = true;
		}
	}

	const GSVector2i fbsize(sTex[2] ? sTex[2]->GetSize() : GSVector2i(0, 0));
	const GSVector4i fbarea(0, 0, fbsize.x, fbsize.y);
	if (feedback_write_2)
	{
		EndRenderPass();
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		if (dcleared)
			SetUtilityTexture(dTex, sampler);
		BeginRenderPassForStretchRect(static_cast<GSTextureWebGPU*>(sTex[2]), fbarea, GSVector4i(dRect[2]));
		if (dcleared)
		{
			SetPipeline(yuv_pipeline);
			SetUtilityPushConstants(yuv_constants, sizeof(yuv_constants));
			DrawStretchRect(full_r, dRect[2], fbsize);
		}
		EndRenderPass();
	}

	if (feedback_write_2_but_blend_bg || !dcleared)
	{
		EndRenderPass();
		OMSetRenderTargets(dTex, nullptr, darea);
		RenderPassLoadOps ops;
		ops.rt_load = WGPULoadOp_Clear;
		ops.rt_clear = GSVector4::unorm8(c);
		BeginRenderPass(dst, nullptr, ops);
		dTex->SetState(GSTexture::State::Dirty);
	}
	else if (!InRenderPass())
	{
		OMSetRenderTargets(dTex, nullptr, darea);
		BeginRenderPass(dst, nullptr, RenderPassLoadOps());
	}

	if (sTex[0] && sTex[0]->GetState() == GSTexture::State::Dirty)
	{
		SetUtilityTexture(sTex[0], sampler);
		SetPipeline(m_merge[PMODE.MMOD]);
		SetUtilityPushConstants(&bg_color, sizeof(bg_color));
		DrawStretchRect(sRect[0], dRect[0], dTex->GetSize());
	}

	if (feedback_write_1)
	{
		EndRenderPass();
		SetPipeline(yuv_pipeline);
		SetUtilityTexture(dTex, sampler);
		SetUtilityPushConstants(yuv_constants, sizeof(yuv_constants));
		OMSetRenderTargets(sTex[2], nullptr, fbarea);
		BeginRenderPass(static_cast<GSTextureWebGPU*>(sTex[2]), nullptr, RenderPassLoadOps());
		DrawStretchRect(full_r, dRect[2], dsize);
	}

	EndRenderPass();

	dst->CommitClear();
}

void GSDeviceWebGPU::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb)
{
	const GSVector4i rc = GSVector4i(dRect);
	const GSVector4i dtex_rc = dTex->GetRect();
	const GSVector4i clamped_rc = rc.rintersect(dtex_rc);
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, clamped_rc);
	SetUtilityTexture(sTex, filter == Biln ? m_linear_sampler : m_point_sampler);
	BeginRenderPassForStretchRect(static_cast<GSTextureWebGPU*>(dTex), dTex->GetRect(), clamped_rc, false);
	SetPipeline(m_interlace[static_cast<int>(shader)]);
	SetUtilityPushConstants(&cb, sizeof(cb));
	DrawStretchRect(sRect, dRect, dTex->GetSize());
	EndRenderPass();
}

void GSDeviceWebGPU::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
	const GSVector4 sRect = GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
	const GSVector4i dRect = dTex->GetRect();
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, dRect);
	SetUtilityTexture(sTex, m_point_sampler);
	RenderPassLoadOps ops;
	ops.rt_load = WGPULoadOp_Clear;
	BeginRenderPass(static_cast<GSTextureWebGPU*>(dTex), nullptr, ops);
	dTex->SetState(GSTexture::State::Dirty);
	SetPipeline(m_shadeboost_pipeline);
	SetUtilityPushConstants(params, sizeof(float) * 4);
	DrawStretchRect(sRect, GSVector4(dRect), dTex->GetSize());
	EndRenderPass();
}

void GSDeviceWebGPU::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
	const GSVector4 sRect = GSVector4(0.0f, 0.0f, 1.0f, 1.0f);
	const GSVector4i dRect = dTex->GetRect();
	EndRenderPass();
	OMSetRenderTargets(dTex, nullptr, dRect);
	SetUtilityTexture(sTex, m_linear_sampler);
	RenderPassLoadOps ops;
	ops.rt_load = WGPULoadOp_Clear;
	BeginRenderPass(static_cast<GSTextureWebGPU*>(dTex), nullptr, ops);
	dTex->SetState(GSTexture::State::Dirty);
	SetPipeline(m_fxaa_pipeline);
	DrawStretchRect(sRect, GSVector4(dRect), dTex->GetSize());
	EndRenderPass();
}

bool GSDeviceWebGPU::DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only, const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	return false;
}

void GSDeviceWebGPU::RenderImGui()
{
	ImGui::Render();
	const ImDrawData* draw_data = ImGui::GetDrawData();
	if (draw_data->CmdListsCount == 0)
		return;

	UpdateImGuiTextures();

	if (!InRenderPass())
		return;

	const GSVector4 uniforms(
		2.0f / static_cast<float>(m_window_info.surface_width),
		2.0f / static_cast<float>(m_window_info.surface_height),
		-1.0f,
		-1.0f);

	SetUtilityPushConstants(&uniforms, sizeof(uniforms));
	SetPipeline(m_imgui_pipeline);

	if (m_utility_sampler != m_linear_sampler)
	{
		m_utility_sampler = m_linear_sampler;
		m_dirty_flags |= DIRTY_FLAG_UTILITY_TEXTURE;
	}

	m_dirty_flags &= ~(DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR);

	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];

		u32 vertex_offset;
		{
			const u32 size = sizeof(ImDrawVert) * static_cast<u32>(cmd_list->VtxBuffer.Size);
			if (!m_vertex_stream_buffer.ReserveMemory(size, sizeof(ImDrawVert)))
			{
				Console.Warning("WebGPU: Skipping ImGui draw because of no vertex buffer space");
				return;
			}

			vertex_offset = m_vertex_stream_buffer.GetCurrentOffset() / sizeof(ImDrawVert);
			std::memcpy(m_vertex_stream_buffer.GetCurrentHostPointer(), cmd_list->VtxBuffer.Data, size);
			m_vertex_stream_buffer.CommitMemory(size);
		}

		static_assert(sizeof(ImDrawIdx) == sizeof(u16));
		IASetIndexBuffer(cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size);

		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
		{
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
			pxAssert(!pcmd->UserCallback);

			const GSVector4 clip = GSVector4::load<false>(&pcmd->ClipRect);
			if ((clip.zwzw() <= clip.xyxy()).mask() != 0)
				continue;

			SetScissor(GSVector4i(clip).max_i32(GSVector4i::zero()));

			GSTextureWebGPU* tex = reinterpret_cast<GSTextureWebGPU*>(pcmd->GetTexID());
			if (tex)
				SetUtilityTexture(tex, m_linear_sampler);

			if (ApplyUtilityState())
			{
				wgpuRenderPassEncoderDrawIndexed(m_render_pass, pcmd->ElemCount, 1, m_index.start + pcmd->IdxOffset,
					vertex_offset + pcmd->VtxOffset, 0);
			}
		}

		g_perfmon.Put(GSPerfMon::DrawCalls, cmd_list->CmdBuffer.Size);
	}
}

//////////////////////////////////////////////////////////////////////////
// Hardware renderer draws
//////////////////////////////////////////////////////////////////////////

bool GSDeviceWebGPU::BindDrawPipeline(const PipelineSelector& p)
{
	WGPURenderPipeline pipeline = GetTFXPipeline(p);
	if (!pipeline)
		return false;

	if (m_tfx_texture_layout != p.tex_depth)
	{
		m_tfx_texture_layout = p.tex_depth;
		m_dirty_flags |= DIRTY_FLAG_TFX_TEXTURES;
	}

	SetPipeline(pipeline);

	if (!InRenderPass())
	{
		if (!m_current_render_target && !m_current_depth_target)
			return false;

		BeginRenderPassForTargets(m_current_render_target, m_current_depth_target, false);
	}

	return ApplyTFXState();
}

void GSDeviceWebGPU::SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	GL_PUSH("SetupDATE {%d,%d} %dx%d", bbox.left, bbox.top, bbox.width(), bbox.height());

	const GSVector2i size(ds->GetSize());
	const GSVector4 src = GSVector4(bbox) / GSVector4(size).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};

	EndRenderPass();
	SetUtilityTexture(rt, m_point_sampler);
	OMSetRenderTargets(nullptr, ds, bbox);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), 4);
	SetPipeline(m_convert[ShaderConvertSelector(SetDATMShader(datm)).Index()]);

	GSTextureWebGPU* const wds = static_cast<GSTextureWebGPU*>(ds);
	RenderPassLoadOps ops = GetLoadOpsForTargets(nullptr, wds, false);
	ops.stencil_load = WGPULoadOp_Clear;
	ops.stencil_clear = 0;
	BeginRenderPass(nullptr, wds, ops);
	if (ApplyUtilityState())
		DrawPrimitive();

	EndRenderPass();
}

GSTextureWebGPU* GSDeviceWebGPU::SetupPrimitiveTrackingDATE(GSHWDrawConfig& config)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	GL_INS("Setup DATE Primitive ID Image for {%d,%d}-{%d,%d}", config.drawarea.left, config.drawarea.top,
		config.drawarea.right, config.drawarea.bottom);

	const GSVector2i rtsize(config.rt->GetSize());
	GSTextureWebGPU* image =
		static_cast<GSTextureWebGPU*>(CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false));
	if (!image)
		return nullptr;

	EndRenderPass();

	SetUtilityTexture(config.rt, m_point_sampler);
	OMSetRenderTargets(image, nullptr, config.drawarea);

	RenderPassLoadOps ops;
	ops.rt_load = WGPULoadOp_Clear;
	BeginRenderPass(image, nullptr, ops);
	image->SetState(GSTexture::State::Dirty);

	const GSVector4 src = GSVector4(config.drawarea) / GSVector4(rtsize).xyxy();
	const GSVector4 dst = src * 2.0f - 1.0f;
	const GSVertexPT1 vertices[] = {
		{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
		{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
		{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
		{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
	};
	SetPipeline(m_primid_image_setup_pipelines[static_cast<u8>(config.datm)]);
	IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));
	if (ApplyUtilityState())
		DrawPrimitive();

	EndRenderPass();

	UploadHWDrawVerticesAndIndices(config);

	PSSetShaderResource(TFX_TEXTURE_PRIMID, nullptr, false);

	PipelineSelector& pipe = m_pipeline_selector;
	UpdateHWPipelineSelector(config, pipe);
	pipe.dss.zwe = false;
	pipe.cms.wrgba = 0;
	pipe.bs = {};
	pipe.rt = true;
	pipe.ps.blend_a = pipe.ps.blend_b = pipe.ps.blend_c = pipe.ps.blend_d = false;
	pipe.ps.no_color = false;
	pipe.ps.no_color1 = true;

	OMSetRenderTargets(image, config.ds, config.scissor);
	if (BindDrawPipeline(pipe))
		Draw(config);

	EndRenderPass();

	config.ps.date = 3;
	config.alpha_second_pass.ps.date = 3;

	PSSetShaderResource(TFX_TEXTURE_PRIMID, image, false);
	return image;
}

void GSDeviceWebGPU::UpdateHWPipelineSelector(GSHWDrawConfig& config, PipelineSelector& pipe)
{
	pipe.vs.key = config.vs.key;
	pipe.ps.key_hi = config.ps.key_hi;
	pipe.ps.key_lo = config.ps.key_lo;
	pipe.dss.key = config.depth.key;
	pipe.bs.key = config.blend.key;
	pipe.bs.constant = 0;
	pipe.cms.key = config.colormask.key;
	pipe.topology = static_cast<u32>(config.topology);
	pipe.rt = config.rt != nullptr;
	pipe.ds = config.ds != nullptr;
	pipe.tex_depth = (config.tex != nullptr && (config.tex->GetFormat() == GSTexture::Format::DepthStencil ||
	                                             config.tex->GetFormat() == GSTexture::Format::DepthColor));
	pipe.vs.point_size |= (config.topology == GSHWDrawConfig::Topology::Point);
}

void GSDeviceWebGPU::UploadHWDrawVerticesAndIndices(const GSHWDrawConfig& config)
{
	IASetVertexBuffer(config.verts, sizeof(GSVertex), config.nverts, GetVertexAlignment(config.vs.expand));

	if (config.vs.UseFixedExpandIndexBuffer())
	{
		m_index.start = 0;
		m_index.count = config.nindices;
		SetIndexBuffer(m_expand_index_buffer);
	}
	else if (config.vs.UseVSExpandIndexBuffer())
	{
		VSSetIndexBuffer(config.indices, config.nindices);
		SetVSConstantBuffer(config.cb_vs);
	}
	else
	{
		IASetIndexBuffer(config.indices, config.nindices);
	}
}

void GSDeviceWebGPU::FeedbackCopyAndBind(const GSHWDrawConfig& config, GSTextureWebGPU* rt, GSTextureWebGPU* rt_clone,
	GSTextureWebGPU* ds, GSTextureWebGPU* ds_clone, const GSVector4i& copyarea)
{
	if (rt_clone)
	{
		CopyRect(rt, rt_clone, copyarea, copyarea.left, copyarea.top);
		PSSetShaderResource(TFX_TEXTURE_RT, rt_clone, false);
		if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_RT)
			PSSetShaderResource(TFX_TEXTURE_TEXTURE, rt_clone, false);
	}
	if (ds_clone)
	{
		CopyRect(ds, ds_clone, copyarea, copyarea.left, copyarea.top);
		PSSetShaderResource(TFX_TEXTURE_DEPTH, ds_clone, false);
		if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_DEPTH)
			PSSetShaderResource(TFX_TEXTURE_TEXTURE, ds_clone, false);
	}
}

void GSDeviceWebGPU::FeedbackCopyAndBind(const GSHWDrawConfig& config, GSTextureWebGPU* rt, GSTextureWebGPU* rt_clone,
	GSTextureWebGPU* ds, GSTextureWebGPU* ds_clone, const GSVector4i& copyarea, const GSVector4i& samplearea)
{
	const GSVector4i rtsize = (rt ? rt : ds)->GetRect();

	if (config.tex_hazard != GSHWDrawConfig::TEX_HAZARD_NONE)
	{
		const GSVector4i union_rect = config.drawarea.runion(config.samplearea);
		const u32 size_union = union_rect.width() * union_rect.height();
		const u32 size_indiv = config.drawarea.width() * config.drawarea.height() +
		                       config.samplearea.width() * config.samplearea.height();

		if (size_union > size_indiv)
		{
			FeedbackCopyAndBind(config, rt, rt_clone, ds, ds_clone, ProcessCopyArea(rtsize, config.drawarea));
			FeedbackCopyAndBind(config, rt, rt_clone, ds, ds_clone, ProcessCopyArea(rtsize, config.samplearea));
		}
		else
		{
			FeedbackCopyAndBind(config, rt, rt_clone, ds, ds_clone, ProcessCopyArea(rtsize, union_rect));
		}
	}
	else
	{
		FeedbackCopyAndBind(config, rt, rt_clone, ds, ds_clone, ProcessCopyArea(rtsize, config.drawarea));
	}
}

void GSDeviceWebGPU::SendHWDraw(const GSHWDrawConfig& config, GSTextureWebGPU* draw_rt_clone, GSTextureWebGPU* draw_rt,
	GSTextureWebGPU* draw_ds_clone, GSTextureWebGPU* draw_ds, bool one_barrier, bool full_barrier)
{
#ifdef PCSX2_DEVBUILD
	if ((one_barrier || full_barrier) && !(config.IsFeedbackLoopRT(m_pipeline_selector.ps) || config.IsFeedbackLoopDepth(m_pipeline_selector.ps))) [[unlikely]]
		Console.Warning("WebGPU: Possible unnecessary copy detected.");
#endif

	if (full_barrier && (draw_rt_clone || draw_ds_clone))
	{
		pxAssert(config.drawlist && !config.drawlist->empty());
		const u32 draw_list_size = static_cast<u32>(config.drawlist->size());
		const u32 indices_per_prim = config.indices_per_prim;
		pxAssert(config.drawlist_bbox && static_cast<u32>(config.drawlist_bbox->size()) == draw_list_size);

		GL_PUSH("Split the draw");
		g_perfmon.Put(GSPerfMon::Barriers, draw_list_size);

		if (config.tex_hazard != GSHWDrawConfig::TEX_HAZARD_NONE)
			FeedbackCopyAndBind(config, draw_rt, draw_rt_clone, draw_ds, draw_ds_clone, config.samplearea);

		for (u32 n = 0, p = 0; n < draw_list_size; n++)
		{
			const u32 count = config.drawlist->at(n) * indices_per_prim;
			const GSVector4i bbox = config.drawlist_bbox->at(n).rintersect(config.drawarea);
			FeedbackCopyAndBind(config, draw_rt, draw_rt_clone, draw_ds, draw_ds_clone, bbox);
			Draw(config, p, count);
			p += count;
		}

		return;
	}

	if (one_barrier && (draw_rt_clone || draw_ds_clone))
	{
		g_perfmon.Put(GSPerfMon::Barriers, 1);
		FeedbackCopyAndBind(config, draw_rt, draw_rt_clone, draw_ds, draw_ds_clone, config.drawarea, config.samplearea);
	}

	Draw(config);
}

void GSDeviceWebGPU::RenderHW(GSHWDrawConfig& config)
{
	const GSVector2i rtsize(config.rt ? config.rt->GetSize() : config.ds->GetSize());
	GSTextureWebGPU* draw_rt = static_cast<GSTextureWebGPU*>(config.rt);
	GSTextureWebGPU* draw_ds = static_cast<GSTextureWebGPU*>(config.ds);
	GSTextureWebGPU* draw_rt_clone = nullptr;
	GSTextureWebGPU* draw_ds_clone = nullptr;
	GSTextureWebGPU* date_image = nullptr;
	GSTextureWebGPU* colclip_rt = static_cast<GSTextureWebGPU*>(GetColorClipTexture());

	ScopedGuard recycle_temp_textures([&]() {
		if (draw_rt_clone)
			Recycle(draw_rt_clone);
		if (draw_ds_clone)
			Recycle(draw_ds_clone);
		if (date_image)
			Recycle(date_image);
	});

	SetVSConstantBuffer(config.cb_vs);
	SetPSConstantBuffer(config.cb_ps);

	if (config.tex)
	{
		PSSetShaderResource(TFX_TEXTURE_TEXTURE, config.tex, config.tex != config.rt && config.tex != config.ds);
		PSSetSampler(config.sampler);
	}
	if (config.pal)
		PSSetShaderResource(TFX_TEXTURE_PALETTE, config.pal, true);

	if (config.blend.constant_enable)
		SetBlendConstants(config.blend.constant);

	if (colclip_rt)
	{
		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::EarlyResolve)
		{
			GL_PUSH("Blit ColorClip back to RT");

			const GSVector4 dRect(config.colclip_update_area);
			const GSVector4 sRect = dRect / GSVector4(rtsize).xyxy();
			StretchRect(colclip_rt, sRect, config.rt, dRect, ShaderConvert::COLCLIP_RESOLVE, Nearest);
			Recycle(colclip_rt);
			SetColorClipTexture(nullptr);
			colclip_rt = nullptr;
		}
		else
		{
			config.ps.colclip_hw = 1;
		}
	}

	if (config.ps.colclip_hw)
	{
		if (!colclip_rt)
		{
			config.colclip_update_area = config.drawarea;
			colclip_rt = static_cast<GSTextureWebGPU*>(CreateFeedbackTarget(rtsize.x, rtsize.y, GSTexture::Format::ColorClip, false));
			if (!colclip_rt)
			{
				Console.Warning("WebGPU: Failed to allocate ColorClip render target, aborting draw.");
				return;
			}

			SetColorClipTexture(colclip_rt);

			if (draw_rt->GetState() == GSTexture::State::Cleared)
			{
				colclip_rt->SetClearColor(draw_rt->GetClearColor());
			}
			else if (draw_rt->GetState() == GSTexture::State::Dirty)
			{
				GL_PUSH("ColorClip Render Target Setup");
				const GSVector4 dRect = GSVector4((config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly) ? GSVector4i::loadh(rtsize) : config.drawarea);
				const GSVector4 sRect = dRect / GSVector4(rtsize).xyxy();
				StretchRect(config.rt, sRect, colclip_rt, dRect, ShaderConvert::COLCLIP_INIT, Nearest);
			}
			else
			{
				colclip_rt->SetState(GSTexture::State::Invalidated);
			}
		}

		draw_rt = colclip_rt;
	}

	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		GSTexture* const backup_rt = config.rt;
		config.rt = draw_rt;
		date_image = SetupPrimitiveTrackingDATE(config);
		config.rt = backup_rt;
		if (!date_image)
		{
			Console.Warning("WebGPU: Failed to allocate DATE image, aborting draw.");
			return;
		}
	}

	PipelineSelector& pipe = m_pipeline_selector;
	UpdateHWPipelineSelector(config, pipe);
	pipe.ps.colclip_hw = config.ps.colclip_hw;

	const bool need_barrier = config.require_one_barrier || (config.require_full_barrier && m_features.multidraw_fb_copy);
	switch (config.destination_alpha)
	{
		case GSHWDrawConfig::DestinationAlphaMode::Off:
		case GSHWDrawConfig::DestinationAlphaMode::Full:
		case GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking:
			break;
		case GSHWDrawConfig::DestinationAlphaMode::StencilOne:
			if (!need_barrier)
			{
				SetupDATE(draw_rt, config.ds, config.datm, config.drawarea);
				config.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Stencil;
			}
			break;
		case GSHWDrawConfig::DestinationAlphaMode::Stencil:
			SetupDATE(draw_rt, config.ds, config.datm, config.drawarea);
			break;
	}

	if (!config.tex && ((config.rt && static_cast<GSTextureWebGPU*>(config.rt) == m_tfx_textures[TFX_TEXTURE_TEXTURE]) ||
						   (config.ds && static_cast<GSTextureWebGPU*>(config.ds) == m_tfx_textures[TFX_TEXTURE_TEXTURE])))
	{
		PSSetShaderResource(TFX_TEXTURE_TEXTURE, nullptr, false);
	}

	if (InRenderPass() && ((draw_rt && m_current_render_target == draw_rt) || (draw_ds && m_current_depth_target == draw_ds)))
	{
		if (!draw_rt && m_current_render_target && config.tex != m_current_render_target &&
			draw_ds && m_current_render_target->GetSize() == draw_ds->GetSize())
		{
			draw_rt = m_current_render_target;
			pipe.rt = true;
		}
		else if (!draw_ds && m_current_depth_target && config.tex != m_current_depth_target &&
				 draw_rt && m_current_depth_target->GetSize() == draw_rt->GetSize())
		{
			draw_ds = m_current_depth_target;
			pipe.ds = true;
		}
	}

	const bool rt_feedbackloop_pass1 = config.IsFeedbackLoopRT(config.ps);
	const bool rt_feedbackloop_pass2 = config.alpha_second_pass.enable && config.IsFeedbackLoopRT(config.alpha_second_pass.ps);
	if (draw_rt && need_barrier && (rt_feedbackloop_pass1 || rt_feedbackloop_pass2))
	{
		draw_rt_clone = static_cast<GSTextureWebGPU*>(CreateTexture(rtsize.x, rtsize.y, 1, draw_rt->GetFormat(), true));
		if (!draw_rt_clone)
			Console.Warning("WebGPU: Failed to allocate temp texture for RT copy.");
	}

	const bool ds_feedbackloop_pass1 = config.IsFeedbackLoopDepth(config.ps);
	const bool ds_feedbackloop_pass2 = config.alpha_second_pass.enable && config.IsFeedbackLoopDepth(config.alpha_second_pass.ps);
	if (draw_ds && need_barrier && (ds_feedbackloop_pass1 || ds_feedbackloop_pass2))
	{
		draw_ds_clone = static_cast<GSTextureWebGPU*>(CreateTexture(rtsize.x, rtsize.y, 1, draw_ds->GetFormat(), true));
		if (!draw_ds_clone)
			Console.Warning("WebGPU: Failed to allocate temp texture for DS copy.");
	}

	if (!draw_rt_clone)
		PSSetShaderResource(TFX_TEXTURE_RT, nullptr, false);
	if (!draw_ds_clone)
		PSSetShaderResource(TFX_TEXTURE_DEPTH, nullptr, false);

	OMSetRenderTargets(draw_rt, draw_ds, config.scissor);

	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne)
	{
		EndRenderPass();
		BeginRenderPassForTargets(draw_rt, draw_ds, true);
	}
	else if (!InRenderPass())
	{
		BeginRenderPassForTargets(draw_rt, draw_ds, false);
	}

	UploadHWDrawVerticesAndIndices(config);

	if (BindDrawPipeline(pipe))
	{
		SendHWDraw(config, rt_feedbackloop_pass1 ? draw_rt_clone : nullptr, draw_rt, ds_feedbackloop_pass1 ? draw_ds_clone : nullptr, draw_ds,
			config.require_one_barrier, config.require_full_barrier);
	}

	if (config.blend_multi_pass.enable)
	{
		if (config.blend_multi_pass.blend.constant_enable)
			SetBlendConstants(config.blend_multi_pass.blend.constant);

		pipe.bs = config.blend_multi_pass.blend;
		pipe.bs.constant = 0;
		pipe.ps.no_color1 = config.blend_multi_pass.no_color1;
		pipe.ps.blend_hw = config.blend_multi_pass.blend_hw;
		pipe.ps.dither = config.blend_multi_pass.dither;
		if (BindDrawPipeline(pipe))
			Draw(config);
	}

	if (config.alpha_second_pass.enable)
	{
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			SetPSConstantBuffer(config.cb_ps);
		}

		pipe.ps = config.alpha_second_pass.ps;
		pipe.ps.colclip_hw = config.ps.colclip_hw;
		pipe.cms = config.alpha_second_pass.colormask;
		pipe.dss = config.alpha_second_pass.depth;
		pipe.bs = config.blend;
		pipe.bs.constant = 0;
		if (BindDrawPipeline(pipe))
		{
			const bool one_barrier = config.alpha_second_pass.require_one_barrier && m_features.multidraw_fb_copy;
			SendHWDraw(config, rt_feedbackloop_pass2 ? draw_rt_clone : nullptr, draw_rt, ds_feedbackloop_pass2 ? draw_ds_clone : nullptr, draw_ds,
				one_barrier, config.alpha_second_pass.require_full_barrier);
		}
	}

	if (colclip_rt)
	{
		config.colclip_update_area = config.colclip_update_area.runion(config.drawarea);

		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly || config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve)
		{
			GL_PUSH("Blit ColorClip back to RT");

			const GSVector4 dRect(config.colclip_update_area);
			const GSVector4 sRect = dRect / GSVector4(rtsize).xyxy();
			StretchRect(colclip_rt, sRect, config.rt, dRect, ShaderConvert::COLCLIP_RESOLVE, Nearest);
			Recycle(colclip_rt);
			SetColorClipTexture(nullptr);
		}
	}

	config.colclip_mode = GSHWDrawConfig::ColClipMode::NoModify;
}
