// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/GSVector.h"
#include "GS/Renderers/WebGPU/GSTextureWebGPU.h"
#include "GS/Renderers/WebGPU/WGSLPreprocessor.h"
#include "GS/Renderers/WebGPU/WebGPUStreamBuffer.h"

#include "common/HashCombine.h"

#include <webgpu/webgpu.h>

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class GSDeviceWebGPU final : public GSDevice
{
public:
	enum : u32
	{
		NUM_TFX_DYNAMIC_OFFSETS = 3,
		UTILITY_UNIFORM_SIZE = 128,
		TEXEL_COPY_ROW_ALIGNMENT = 256,
		UNIFORM_OFFSET_ALIGNMENT = 256,
		NUM_TFX_TEXTURE_LAYOUTS = 2,
		NUM_UTILITY_LAYOUTS = 2,
	};

	struct UtilityPipeline
	{
		WGPURenderPipeline pipeline = nullptr;
		bool depth_input = false;
	};

	enum TFX_TEXTURES : u32
	{
		TFX_TEXTURE_TEXTURE = 0,
		TFX_TEXTURE_PALETTE,
		TFX_TEXTURE_RT,
		TFX_TEXTURE_PRIMID,
		TFX_TEXTURE_DEPTH,

		NUM_TFX_TEXTURES
	};

	enum TFX_UBO_BINDINGS : u32
	{
		TFX_UBO_BINDING_VS = 0,
		TFX_UBO_BINDING_PS = 1,
		TFX_UBO_BINDING_VERTEX_STORAGE = 2,
		TFX_UBO_BINDING_INDEX_STORAGE = 3,
		TFX_UBO_BINDING_VS_PUSH = 4,
	};

	struct alignas(8) PipelineSelector
	{
		GSHWDrawConfig::PSSelector ps;

		union
		{
			struct
			{
				u32 topology : 2;
				u32 rt : 1;
				u32 ds : 1;
				u32 tex_depth : 1;
			};

			u32 key;
		};

		GSHWDrawConfig::BlendState bs;
		GSHWDrawConfig::VSSelector vs;
		GSHWDrawConfig::DepthStencilSelector dss;
		GSHWDrawConfig::ColorMaskSelector cms;
		u8 pad;

		__fi bool operator==(const PipelineSelector& p) const { return BitEqual(*this, p); }
		__fi bool operator!=(const PipelineSelector& p) const { return !BitEqual(*this, p); }

		__fi PipelineSelector() { std::memset(this, 0, sizeof(*this)); }
	};
	static_assert(sizeof(PipelineSelector) == 32, "Pipeline selector is 32 bytes");

	struct PipelineSelectorHash
	{
		std::size_t operator()(const PipelineSelector& e) const noexcept
		{
			std::size_t hash = 0;
			HashCombine(hash, e.vs.key, e.ps.key_hi, e.ps.key_lo, e.dss.key, e.cms.key, e.bs.key, e.key);
			return hash;
		}
	};

	struct DeviceFeatures
	{
		bool depth32float_stencil8 : 1;
		bool dual_source_blending : 1;
		bool primitive_index : 1;
		bool texture_formats_tier1 : 1;
		bool float32_blendable : 1;
		bool float32_filterable : 1;
		bool texture_compression_bc : 1;
		bool timestamp_query : 1;
	};

	GSDeviceWebGPU();
	~GSDeviceWebGPU() override;

	__fi static GSDeviceWebGPU* GetInstance() { return static_cast<GSDeviceWebGPU*>(g_gs_device.get()); }

	static std::vector<GSAdapterInfo> GetAdapterInfo();

	__fi WGPUInstance GetWGPUInstance() const { return m_instance; }
	__fi WGPUAdapter GetAdapter() const { return m_adapter; }
	__fi WGPUDevice GetDevice() const { return m_device; }
	__fi WGPUQueue GetQueue() const { return m_queue; }
	__fi const DeviceFeatures& GetDeviceFeatures() const { return m_device_features; }
	__fi WebGPUStreamBuffer& GetTextureUploadBuffer() { return m_texture_stream_buffer; }

	__fi u64 GetCurrentFenceCounter() const { return m_current_fence_counter; }
	__fi u64 GetCompletedFenceCounter() const { return m_completed_fence_counter; }
	/// True when the instance cannot block in wgpuInstanceWaitAny() (emdawnwebgpu without Asyncify):
	/// futures complete from the thread's event loop, so readbacks and device creation are callback driven.
	__fi bool IsEventLoopDriven() const { return !m_timed_wait_any; }
	void WaitForFenceCounter(u64 fence_counter);
	void WaitForGPUIdle();
	bool WaitForFuture(WGPUFuture future);
	void ProcessEvents();

	WGPUCommandEncoder GetCommandEncoder();
	bool InRenderPass() const { return (m_render_pass != nullptr); }
	void EndRenderPass();
	__fi bool IsPresenting() const { return m_is_presenting; }

	void ExecuteCommandBuffer(bool wait_for_completion);
	void ExecuteCommandBuffer(bool wait_for_completion, const char* reason, ...);
	void ExecuteCommandBufferAndRestartRenderPass(bool wait_for_completion, const char* reason);
	void ExecuteCommandBufferForReadback();

	WGPUTextureFormat LookupNativeFormat(GSTexture::Format format) const;
	static u32 GetBytesPerTexel(GSTexture::Format format);

	void CommitClear(GSTextureWebGPU* tex);
	void GenerateMipmaps(GSTextureWebGPU* tex);
	void UnbindTexture(GSTextureWebGPU* tex);

	RenderAPI GetRenderAPI() const override;
	bool HasSurface() const override;

	bool Create(GSVSyncMode vsync_mode, bool allow_present_throttle) override;
	void Destroy() override;
	bool IsCreatePending() const override;
	bool HasPendingAsyncWork() const override { return (m_pending_async_maps > 0); }
	__fi void AddPendingAsyncMap(s32 delta) { m_pending_async_maps += delta; }

	bool UpdateWindow() override;
	void ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale) override;
	bool SupportsExclusiveFullscreen() const override;
	void DestroySurface() override;
	std::string GetDriverInfo() const override;

	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle) override;

	PresentResult BeginPresent(bool frame_skip) override;
	void EndPresent() override;

	bool SetGPUTimingEnabled(bool enabled) override;
	float GetAndResetAccumulatedGPUTime() override;

	bool SetGPUPipelineStatisticsEnabled(bool enabled) override;
	GPUPipelineStatistics GetAndResetAccumulatedGPUPipelineStatistics() override;

	void PushDebugGroup(const char* fmt, ...) override;
	void PopDebugGroup() override;
	void InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...) override;

	std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) override;

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) override;

	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		PresentShader shader, float shaderTime, Filter filter) override;
	void DrawMultiStretchRects(
		const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvertSelector shader) override;

	void UpdateCLUTTexture(
		GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize) override;
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM,
		GSTexture* dTex, u32 DBW, u32 DPSM) override;
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor, const GSVector2i& clamp_min, const GSVector4& dRect) override;

	void RenderHW(GSHWDrawConfig& config) override;

	void ClearSamplerCache() override;

	static void AddTFXVertexShaderMacros(WGSLPreprocessor& pp, GSHWDrawConfig::VSSelector sel, bool provoking_vertex_last);
	static void AddTFXFragmentShaderMacros(WGSLPreprocessor& pp, const GSHWDrawConfig::PSSelector& sel);
	static void AddConvertShaderMacros(WGSLPreprocessor& pp, ShaderConvertSelector sel);

	WGPUShaderModule CreateShaderModule(const std::string& source, const char* label);
	WGPUShaderModule GetTFXVertexShader(GSHWDrawConfig::VSSelector sel);
	WGPUShaderModule GetTFXFragmentShader(const GSHWDrawConfig::PSSelector& sel);
	WGPURenderPipeline CreateTFXPipeline(const PipelineSelector& p);
	WGPURenderPipeline GetTFXPipeline(const PipelineSelector& p);

protected:
	using GSDevice::DoStretchRect;
	void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvertSelector shader, Filter filter) override;
	void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, const GSVector4& dRect,
		PresentShader shader, Filter filter) override;

private:
	enum DIRTY_FLAG : u32
	{
		DIRTY_FLAG_TFX_TEXTURE_0 = (1 << 0),
		DIRTY_FLAG_TFX_UBO = (1 << 5),
		DIRTY_FLAG_UTILITY_TEXTURE = (1 << 6),
		DIRTY_FLAG_UTILITY_UNIFORM = (1 << 7),
		DIRTY_FLAG_BLEND_CONSTANTS = (1 << 8),
		DIRTY_FLAG_STENCIL_REF = (1 << 9),
		DIRTY_FLAG_VERTEX_BUFFER = (1 << 10),
		DIRTY_FLAG_INDEX_BUFFER = (1 << 11),
		DIRTY_FLAG_VIEWPORT = (1 << 12),
		DIRTY_FLAG_SCISSOR = (1 << 13),
		DIRTY_FLAG_PIPELINE = (1 << 14),
		DIRTY_FLAG_VS_CONSTANT_BUFFER = (1 << 15),
		DIRTY_FLAG_PS_CONSTANT_BUFFER = (1 << 16),
		DIRTY_FLAG_VS_PUSH_CONSTANTS = (1 << 17),

		DIRTY_FLAG_TFX_TEXTURES = (DIRTY_FLAG_TFX_TEXTURE_0 << 0) | (DIRTY_FLAG_TFX_TEXTURE_0 << 1) |
		                          (DIRTY_FLAG_TFX_TEXTURE_0 << 2) | (DIRTY_FLAG_TFX_TEXTURE_0 << 3) |
		                          (DIRTY_FLAG_TFX_TEXTURE_0 << 4),

		DIRTY_BASE_STATE = DIRTY_FLAG_VERTEX_BUFFER | DIRTY_FLAG_INDEX_BUFFER | DIRTY_FLAG_PIPELINE |
		                   DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR | DIRTY_FLAG_BLEND_CONSTANTS |
		                   DIRTY_FLAG_STENCIL_REF,
		DIRTY_TFX_STATE = DIRTY_BASE_STATE | DIRTY_FLAG_TFX_TEXTURES | DIRTY_FLAG_TFX_UBO,
		DIRTY_UTILITY_STATE = DIRTY_BASE_STATE | DIRTY_FLAG_UTILITY_TEXTURE | DIRTY_FLAG_UTILITY_UNIFORM,
		DIRTY_CONSTANT_BUFFER_STATE = DIRTY_FLAG_VS_CONSTANT_BUFFER | DIRTY_FLAG_PS_CONSTANT_BUFFER | DIRTY_FLAG_VS_PUSH_CONSTANTS,
		ALL_DIRTY_STATE = DIRTY_BASE_STATE | DIRTY_TFX_STATE | DIRTY_UTILITY_STATE | DIRTY_CONSTANT_BUFFER_STATE,
	};

	enum class PipelineLayout
	{
		Undefined,
		TFX,
		Utility
	};

	struct TFXBindGroupKey
	{
		std::array<WGPUTextureView, NUM_TFX_TEXTURES> views;
		WGPUSampler sampler;
		u32 layout;

		bool operator==(const TFXBindGroupKey& rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
	};

	struct TFXBindGroupKeyHash
	{
		std::size_t operator()(const TFXBindGroupKey& k) const noexcept
		{
			std::size_t hash = 0;
			for (WGPUTextureView view : k.views)
				HashCombine(hash, reinterpret_cast<uintptr_t>(view));
			HashCombine(hash, reinterpret_cast<uintptr_t>(k.sampler), k.layout);
			return hash;
		}
	};

	struct UtilityBindGroupKey
	{
		WGPUTextureView view;
		WGPUSampler sampler;
		u32 layout;

		bool operator==(const UtilityBindGroupKey& rhs) const { return view == rhs.view && sampler == rhs.sampler && layout == rhs.layout; }
	};

	struct UtilityBindGroupKeyHash
	{
		std::size_t operator()(const UtilityBindGroupKey& k) const noexcept
		{
			std::size_t hash = 0;
			HashCombine(hash, reinterpret_cast<uintptr_t>(k.view), reinterpret_cast<uintptr_t>(k.sampler), k.layout);
			return hash;
		}
	};

	struct RenderPassLoadOps
	{
		WGPULoadOp rt_load = WGPULoadOp_Load;
		WGPULoadOp ds_load = WGPULoadOp_Load;
		WGPULoadOp stencil_load = WGPULoadOp_Load;
		GSVector4 rt_clear = GSVector4::zero();
		float ds_clear = 0.0f;
		u32 stencil_clear = 0;
	};

	static void RequestAdapterCallback(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1, void* userdata2);
	static void RequestDeviceCallback(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void* userdata2);
	static void DeviceLostCallback(WGPUDevice const* device, WGPUDeviceLostReason reason, WGPUStringView message, void* userdata1, void* userdata2);
	static void UncapturedErrorCallback(WGPUDevice const* device, WGPUErrorType type, WGPUStringView message, void* userdata1, void* userdata2);
	static void QueueWorkDoneCallback(WGPUQueueWorkDoneStatus status, WGPUStringView message, void* userdata1, void* userdata2);
	static void CompilationInfoCallback(WGPUCompilationInfoRequestStatus status, WGPUCompilationInfo const* info, void* userdata1, void* userdata2);

	static WGPUInstance CreateWGPUInstance();
	static WGPURequestAdapterOptions GetRequestAdapterOptions(WGPUSurface surface);
	static WGPUAdapter RequestAdapter(WGPUInstance instance, WGPUSurface surface);

	bool CreateDeviceAndSurface();
	bool CreateInstanceAndSurface();
	bool QueryAdapter();
	WGPUDeviceDescriptor GetDeviceDescriptor();
	bool OnDeviceCreated();
	bool CreateResources();
#ifdef __EMSCRIPTEN__
	enum class CreateState : u8
	{
		None,
		Pending,
		Ready,
		Failed,
	};

	static void RequestAdapterCallbackAsync(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1, void* userdata2);
	static void RequestDeviceCallbackAsync(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void* userdata2);
	static void CompilationInfoCallbackAsync(WGPUCompilationInfoRequestStatus status, WGPUCompilationInfo const* info, void* userdata1, void* userdata2);
	bool BeginCreateAsync();
	void RequestAdapterAsync();
	void ContinueCreateWithAdapter();
	void ContinueCreateWithDevice();
	void CompleteCreate(bool success);
#endif
	bool CreateSurface();
	bool ConfigureSurface();
	bool CheckFeatures();
	bool CreateNullTexture();
	bool CreateBuffers();
	bool CreateLayouts();
	bool CreateSamplers();

	bool CompileConvertPipelines();
	bool CompilePresentPipelines();
	bool CompileInterlacePipelines();
	bool CompileMergePipelines();
	bool CompilePostProcessingPipelines();
	bool CompileImGuiPipeline();

	WGPUShaderModule GetUtilityVertexShader(const std::string& source, const char* file_label);
	WGPUShaderModule GetUtilityFragmentShader(const std::string& source, const char* entry_point, const char* file_label, const WGSLPreprocessor* extra_defines = nullptr);

	UtilityPipeline CreateUtilityPipeline(WGPUShaderModule vs, WGPUShaderModule fs, const char* fs_entry, WGPUTextureFormat color_format,
		u32 color_write_mask, const WGPUBlendState* blend, WGPUTextureFormat depth_format, bool depth_write, bool stencil_write,
		WGPUPrimitiveTopology topology, bool imgui_vertex, bool depth_input, const char* label);

	void RenderImGui();

	void DestroyResources();

	void SubmitCommandBuffer();
	void MoveToNextCommandBuffer();
	void FlushStreamBuffers();

	void BeginRenderPassWithViews(WGPUTextureView rt_view, WGPUTextureView ds_view, bool has_stencil, const RenderPassLoadOps& ops);
	void BeginRenderPass(GSTextureWebGPU* rt, GSTextureWebGPU* ds, const RenderPassLoadOps& ops);
	void BeginRenderPassForTargets(GSTextureWebGPU* rt, GSTextureWebGPU* ds, bool clear_stencil_to_one);
	RenderPassLoadOps GetLoadOpsForTargets(GSTextureWebGPU* rt, GSTextureWebGPU* ds, bool allow_discard_rt) const;
	static GSVector4 GetClearColorForTexture(const GSTextureWebGPU* tex);

	void BeginRenderPassForStretchRect(GSTextureWebGPU* dTex, const GSVector4i& dtex_rc, const GSVector4i& dst_rc, bool allow_discard = true);
	void DoStretchRect(GSTextureWebGPU* sTex, const GSVector4& sRect, GSTextureWebGPU* dTex, const GSVector4& dRect,
		const UtilityPipeline& pipeline, Filter filter, bool allow_discard);
	void DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds);
	void DoMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTextureWebGPU* dTex, ShaderConvertSelector shader);

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE,
		const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter) override;
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb) override;
	void DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4]) override;
	void DoFXAA(GSTexture* sTex, GSTexture* dTex) override;
	bool DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only, const std::array<u32, NUM_CAS_CONSTANTS>& constants) override;

	GSTexture* CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format) override;

	WGPUSampler GetSampler(GSHWDrawConfig::SamplerSelector ss);

	void DrawPrimitive();
	void DrawIndexedPrimitive();
	void DrawIndexedPrimitive(int offset, int count);
	void DrawIndexedPrimitiveVSExpand(int offset, int count, bool vs_indexing, int vs_indexing_expansion);
	void Draw(const GSHWDrawConfig& config);
	void Draw(const GSHWDrawConfig& config, int offset, int count);

	void IASetVertexBuffer(const void* vertex, size_t stride, size_t count, size_t align_multiplier = 1);
	void UploadIndices(WebGPUStreamBuffer& buffer, const void* index, size_t count);
	void IASetIndexBuffer(const void* index, size_t count);
	void VSSetIndexBuffer(const void* index, size_t count);

	void PSSetShaderResource(int i, GSTexture* sr, bool check_state);
	void PSSetSampler(GSHWDrawConfig::SamplerSelector sel);

	void OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i& scissor);

	void SetVSConstantBuffer(const GSHWDrawConfig::VSConstantBuffer& cb);
	void SetPSConstantBuffer(const GSHWDrawConfig::PSConstantBuffer& cb);
	bool SetVSPushConstants(u32 base_vertex, u32 base_index = 0, bool force_update = false);
	bool BindDrawPipeline(const PipelineSelector& p);

	void SetupDATE(GSTexture* rt, GSTexture* ds, SetDATM datm, const GSVector4i& bbox);
	GSTextureWebGPU* SetupPrimitiveTrackingDATE(GSHWDrawConfig& config);
	void UpdateHWPipelineSelector(GSHWDrawConfig& config, PipelineSelector& pipe);
	void UploadHWDrawVerticesAndIndices(const GSHWDrawConfig& config);
	void FeedbackCopyAndBind(const GSHWDrawConfig& config, GSTextureWebGPU* rt, GSTextureWebGPU* rt_clone,
		GSTextureWebGPU* ds, GSTextureWebGPU* ds_clone, const GSVector4i& copyarea);
	void FeedbackCopyAndBind(const GSHWDrawConfig& config, GSTextureWebGPU* rt, GSTextureWebGPU* rt_clone,
		GSTextureWebGPU* ds, GSTextureWebGPU* ds_clone, const GSVector4i& copyarea, const GSVector4i& samplearea);
	void SendHWDraw(const GSHWDrawConfig& config, GSTextureWebGPU* draw_rt_clone, GSTextureWebGPU* draw_rt,
		GSTextureWebGPU* draw_ds_clone, GSTextureWebGPU* draw_ds, bool one_barrier, bool full_barrier);

	void InvalidateCachedState();
	bool ApplyUtilityState(bool already_execed = false);
	bool ApplyTFXState(bool already_execed = false);
	void ApplyBaseState(u32 flags);

	void SetIndexBuffer(WGPUBuffer buffer);
	void SetBlendConstants(u8 color);
	void SetUtilityTexture(GSTexture* tex, WGPUSampler sampler);
	void SetUtilityPushConstants(const void* data, u32 size);
	void SetViewport(const GSVector4i& viewport);
	void SetScissor(const GSVector4i& scissor);
	void SetPipeline(WGPURenderPipeline pipeline);
	void SetPipeline(const UtilityPipeline& pipeline);

	WGPUBindGroup GetTFXTextureBindGroup();
	WGPUBindGroup GetUtilityBindGroup(WGPUTextureView view, WGPUSampler sampler, u32 layout);

	WGPUInstance m_instance = nullptr;
	WGPUAdapter m_adapter = nullptr;
	WGPUDevice m_device = nullptr;
	WGPUQueue m_queue = nullptr;
	WGPUSurface m_surface = nullptr;
	WGPUTextureFormat m_surface_format = WGPUTextureFormat_RGBA8Unorm;
	WGPUPresentMode m_present_mode = WGPUPresentMode_Fifo;
	std::unique_ptr<GSTextureWebGPU> m_surface_texture;
	bool m_surface_configured = false;
	bool m_resize_requested = false;
	bool m_is_presenting = false;
	bool m_device_lost = false;
	bool m_timed_wait_any = false;

	DeviceFeatures m_device_features = {};
	WGPULimits m_limits = WGPU_LIMITS_INIT;
	WGPULimits m_required_limits = WGPU_LIMITS_INIT;
	std::vector<WGPUFeatureName> m_required_features;
	s32 m_pending_async_maps = 0;
#ifdef __EMSCRIPTEN__
	CreateState m_create_state = CreateState::None;
	u32 m_adapter_attempts = 0;
#endif
	std::string m_adapter_name;
	std::string m_adapter_description;
	WGPUBackendType m_backend_type = WGPUBackendType_Undefined;

	WGPUCommandEncoder m_command_encoder = nullptr;
	WGPURenderPassEncoder m_render_pass = nullptr;
	u64 m_current_fence_counter = 1;
	u64 m_completed_fence_counter = 0;
	std::deque<std::pair<u64, WGPUFuture>> m_pending_submits;

	std::array<WGPUBindGroupLayout, NUM_UTILITY_LAYOUTS> m_utility_bind_group_layouts = {};
	std::array<WGPUPipelineLayout, NUM_UTILITY_LAYOUTS> m_utility_pipeline_layouts = {};
	WGPUBindGroupLayout m_tfx_ubo_bind_group_layout = nullptr;
	std::array<WGPUBindGroupLayout, NUM_TFX_TEXTURE_LAYOUTS> m_tfx_texture_bind_group_layouts = {};
	std::array<WGPUPipelineLayout, NUM_TFX_TEXTURE_LAYOUTS> m_tfx_pipeline_layouts = {};
	WGPUBindGroup m_tfx_ubo_bind_group = nullptr;

	WebGPUStreamBuffer m_vertex_stream_buffer;
	WebGPUStreamBuffer m_index_stream_buffer;
	WebGPUStreamBuffer m_expand_index_stream_buffer;
	WebGPUStreamBuffer m_vertex_uniform_stream_buffer;
	WebGPUStreamBuffer m_fragment_uniform_stream_buffer;
	WebGPUStreamBuffer m_texture_stream_buffer;
	WGPUBuffer m_expand_index_buffer = nullptr;

	WGPUSampler m_point_sampler = nullptr;
	WGPUSampler m_linear_sampler = nullptr;
	std::unordered_map<u32, WGPUSampler> m_samplers;

	std::vector<UtilityPipeline> m_convert;
	std::array<UtilityPipeline, static_cast<int>(PresentShader::Count)> m_present = {};
	std::array<UtilityPipeline, static_cast<int>(PresentShader::Count)> m_present_offscreen = {};
	std::array<UtilityPipeline, 2> m_merge = {};
	std::array<UtilityPipeline, NUM_INTERLACE_SHADERS> m_interlace = {};
	std::array<UtilityPipeline, 4> m_primid_image_setup_pipelines = {};
	UtilityPipeline m_fxaa_pipeline;
	UtilityPipeline m_shadeboost_pipeline;
	UtilityPipeline m_imgui_pipeline;

	std::unordered_map<u32, WGPUShaderModule> m_tfx_vertex_shaders;
	std::unordered_map<GSHWDrawConfig::PSSelector, WGPUShaderModule, GSHWDrawConfig::PSSelectorHash> m_tfx_fragment_shaders;
	std::unordered_map<PipelineSelector, WGPURenderPipeline, PipelineSelectorHash> m_tfx_pipelines;
	std::unordered_map<TFXBindGroupKey, WGPUBindGroup, TFXBindGroupKeyHash> m_tfx_bind_groups;
	std::unordered_map<UtilityBindGroupKey, WGPUBindGroup, UtilityBindGroupKeyHash> m_utility_bind_groups;

	GSHWDrawConfig::VSConstantBuffer m_vs_cb_cache;
	GSHWDrawConfig::PSConstantBuffer m_ps_cb_cache;
	GSHWDrawConfig::VSPushConstants m_vs_pc_cache;

	std::string m_tfx_source;

	u32 m_dirty_flags = 0;
	WGPUBuffer m_index_buffer = nullptr;

	GSTextureWebGPU* m_current_render_target = nullptr;
	GSTextureWebGPU* m_current_depth_target = nullptr;
	GSTextureWebGPU* m_pass_render_target = nullptr;
	GSTextureWebGPU* m_pass_depth_target = nullptr;

	GSVector4i m_scissor = GSVector4i::zero();
	GSVector4i m_viewport = GSVector4i::zero();
	u8 m_blend_constant_color = 0;

	std::array<GSTextureWebGPU*, NUM_TFX_TEXTURES> m_tfx_textures = {};
	WGPUSampler m_tfx_sampler = nullptr;
	u32 m_tfx_sampler_sel = 0;
	u32 m_tfx_texture_layout = 0;
	std::array<u32, NUM_TFX_DYNAMIC_OFFSETS> m_tfx_dynamic_offsets = {};

	GSTextureWebGPU* m_utility_texture = nullptr;
	WGPUTextureView m_utility_texture_view_override = nullptr;
	WGPUSampler m_utility_sampler = nullptr;
	u32 m_utility_layout = 0;
	u32 m_utility_uniform_offset = 0;
	alignas(16) u8 m_utility_uniform_data[UTILITY_UNIFORM_SIZE] = {};
	u32 m_utility_uniform_size = 0;

	PipelineLayout m_current_pipeline_layout = PipelineLayout::Undefined;
	WGPURenderPipeline m_current_pipeline = nullptr;

	std::unique_ptr<GSTextureWebGPU> m_null_texture;

	PipelineSelector m_pipeline_selector = {};

#ifdef ENABLE_OGL_DEBUG
	std::vector<bool> m_debug_group_in_pass;
#endif
};
