// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/GS/Renderers/WebGPU/GSDeviceWebGPU.h"
#include "pcsx2/GS/Renderers/WebGPU/WGSLPreprocessor.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <gtest/gtest.h>
#include <webgpu/webgpu.h>

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace
{
	// The full PSSelector cross product is astronomically large. The rule used here is:
	//  1. every field swept through all of its values against the all-zero baseline,
	//  2. curated cross products of the fields which interact in the shader source
	//     (texture formats, blend equation, alpha/depth test, shuffles, channel fetch,
	//     lod/anisotropy, dither/colclip, DATE, AA1),
	//  3. N pseudo-random keys from a fixed xorshift seed (default 1000, override with
	//     PCSX2_WEBGPU_SHADER_TEST_SAMPLES) with every field masked to its valid range.
	// Every permutation is compiled with and without the optional dual_source_blending /
	// primitive_index extensions so both device classes are covered on one machine.

	struct TestDevice
	{
		WGPUInstance instance = nullptr;
		WGPUAdapter adapter = nullptr;
		WGPUDevice device = nullptr;
		bool dual_source_blending = false;
		bool primitive_index = false;

		~TestDevice()
		{
			if (device)
				wgpuDeviceRelease(device);
			if (adapter)
				wgpuAdapterRelease(adapter);
			if (instance)
				wgpuInstanceRelease(instance);
		}
	};

	void WaitFor(WGPUInstance instance, WGPUFuture future)
	{
		WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;
		info.future = future;
		ASSERT_EQ(wgpuInstanceWaitAny(instance, 1, &info, UINT64_MAX), WGPUWaitStatus_Success);
	}

	void AdapterCallback(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1, void* userdata2)
	{
		if (status == WGPURequestAdapterStatus_Success)
			*static_cast<WGPUAdapter*>(userdata1) = adapter;
	}

	void DeviceCallback(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void* userdata2)
	{
		if (status == WGPURequestDeviceStatus_Success)
			*static_cast<WGPUDevice*>(userdata1) = device;
	}

	void ErrorCallback(WGPUDevice const* device, WGPUErrorType type, WGPUStringView message, void* userdata1, void* userdata2)
	{
		std::string* const errors = static_cast<std::string*>(userdata1);
		errors->append(message.data, (message.length == WGPU_STRLEN) ? std::strlen(message.data) : message.length);
		errors->push_back('\n');
	}

	WGPUAdapter RequestAdapter(WGPUInstance instance, WGPUBackendType backend)
	{
		WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
		options.featureLevel = WGPUFeatureLevel_Core;
		options.backendType = backend;
		WGPUAdapter adapter = nullptr;
		WGPURequestAdapterCallbackInfo cbi = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
		cbi.mode = WGPUCallbackMode_WaitAnyOnly;
		cbi.callback = &AdapterCallback;
		cbi.userdata1 = &adapter;
		WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;
		info.future = wgpuInstanceRequestAdapter(instance, &options, cbi);
		if (wgpuInstanceWaitAny(instance, 1, &info, UINT64_MAX) != WGPUWaitStatus_Success)
			return nullptr;
		return adapter;
	}

	std::string s_uncaptured_errors;

	std::unique_ptr<TestDevice> CreateTestDevice()
	{
		std::unique_ptr<TestDevice> td = std::make_unique<TestDevice>();

		WGPUInstanceDescriptor idesc = WGPU_INSTANCE_DESCRIPTOR_INIT;
		static constexpr WGPUInstanceFeatureName features[] = {WGPUInstanceFeatureName_TimedWaitAny};
		idesc.requiredFeatureCount = 1;
		idesc.requiredFeatures = features;
		td->instance = wgpuCreateInstance(&idesc);
		if (!td->instance)
			return {};

		WGPUBackendType backend = WGPUBackendType_Undefined;
		if (const char* env = std::getenv("PCSX2_WEBGPU_BACKEND"); env && std::strcmp(env, "null") == 0)
			backend = WGPUBackendType_Null;

		td->adapter = RequestAdapter(td->instance, backend);
		if (!td->adapter && backend != WGPUBackendType_Null)
			td->adapter = RequestAdapter(td->instance, WGPUBackendType_Null);
		if (!td->adapter)
			return {};

		WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
		if (wgpuAdapterGetInfo(td->adapter, &info) == WGPUStatus_Success)
		{
			std::printf("WebGPU adapter: %.*s (backend %u)\n", static_cast<int>(info.device.length == WGPU_STRLEN ? std::strlen(info.device.data) : info.device.length),
				info.device.data, static_cast<unsigned>(info.backendType));
			wgpuAdapterInfoFreeMembers(info);
		}

		td->dual_source_blending = wgpuAdapterHasFeature(td->adapter, WGPUFeatureName_DualSourceBlending);
		td->primitive_index = wgpuAdapterHasFeature(td->adapter, WGPUFeatureName_PrimitiveIndex);
		std::printf("dual-source-blending: %d, primitive-index: %d\n", td->dual_source_blending, td->primitive_index);

		std::vector<WGPUFeatureName> required;
		if (td->dual_source_blending)
			required.push_back(WGPUFeatureName_DualSourceBlending);
		if (td->primitive_index)
			required.push_back(WGPUFeatureName_PrimitiveIndex);

		WGPUDeviceDescriptor ddesc = WGPU_DEVICE_DESCRIPTOR_INIT;
		ddesc.requiredFeatureCount = required.size();
		ddesc.requiredFeatures = required.data();
		ddesc.uncapturedErrorCallbackInfo.callback = &ErrorCallback;
		ddesc.uncapturedErrorCallbackInfo.userdata1 = &s_uncaptured_errors;
		ddesc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;

		WGPURequestDeviceCallbackInfo cbi = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
		cbi.mode = WGPUCallbackMode_WaitAnyOnly;
		cbi.callback = &DeviceCallback;
		cbi.userdata1 = &td->device;
		WGPUFutureWaitInfo winfo = WGPU_FUTURE_WAIT_INFO_INIT;
		winfo.future = wgpuAdapterRequestDevice(td->adapter, &ddesc, cbi);
		if (wgpuInstanceWaitAny(td->instance, 1, &winfo, UINT64_MAX) != WGPUWaitStatus_Success || !td->device)
			return {};

		return td;
	}

	struct CompileResult
	{
		bool ok;
		std::string messages;
	};

	void CompilationInfoCallback(WGPUCompilationInfoRequestStatus status, WGPUCompilationInfo const* info, void* userdata1, void* userdata2)
	{
		CompileResult* const result = static_cast<CompileResult*>(userdata1);
		if (status != WGPUCompilationInfoRequestStatus_Success || !info)
			return;
		for (size_t i = 0; i < info->messageCount; i++)
		{
			const WGPUCompilationMessage& msg = info->messages[i];
			if (msg.type != WGPUCompilationMessageType_Error)
				continue;
			result->ok = false;
			result->messages.append(std::to_string(msg.lineNum) + ":" + std::to_string(msg.linePos) + ": ");
			result->messages.append(msg.message.data, (msg.message.length == WGPU_STRLEN) ? std::strlen(msg.message.data) : msg.message.length);
			result->messages.push_back('\n');
		}
	}

	void PopErrorScopeCallback(WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void* userdata1, void* userdata2)
	{
		CompileResult* const result = static_cast<CompileResult*>(userdata1);
		if (type != WGPUErrorType_NoError)
		{
			result->ok = false;
			result->messages.append(message.data, (message.length == WGPU_STRLEN) ? std::strlen(message.data) : message.length);
			result->messages.push_back('\n');
		}
	}

	CompileResult CompileWGSL(TestDevice& td, const std::string& source, const char* label)
	{
		CompileResult result{true, {}};

		wgpuDevicePushErrorScope(td.device, WGPUErrorFilter_Validation);

		WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
		wgsl.code = {source.data(), source.size()};
		WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		desc.nextInChain = &wgsl.chain;
		desc.label = {label, WGPU_STRLEN};
		WGPUShaderModule mod = wgpuDeviceCreateShaderModule(td.device, &desc);
		if (mod)
		{
			WGPUCompilationInfoCallbackInfo cbi = WGPU_COMPILATION_INFO_CALLBACK_INFO_INIT;
			cbi.mode = WGPUCallbackMode_WaitAnyOnly;
			cbi.callback = &CompilationInfoCallback;
			cbi.userdata1 = &result;
			WaitFor(td.instance, wgpuShaderModuleGetCompilationInfo(mod, cbi));
			wgpuShaderModuleRelease(mod);
		}
		else
		{
			result.ok = false;
			result.messages = "wgpuDeviceCreateShaderModule returned null";
		}

		WGPUPopErrorScopeCallbackInfo pcbi = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
		pcbi.mode = WGPUCallbackMode_WaitAnyOnly;
		pcbi.callback = &PopErrorScopeCallback;
		pcbi.userdata1 = &result;
		WaitFor(td.instance, wgpuDevicePopErrorScope(td.device, pcbi));
		return result;
	}

	std::string ShaderPath(const char* name)
	{
		return Path::Combine(WEBGPU_SHADER_DIR, name);
	}

	std::optional<std::string> ReadShader(const char* name)
	{
		return FileSystem::ReadFileToString(ShaderPath(name).c_str());
	}

	std::string Preprocess(const WGSLPreprocessor& pp, const std::string& source)
	{
		std::string out;
		std::string error;
		EXPECT_TRUE(pp.Process(source, &out, &error)) << error;
		return out;
	}

	std::string DescribePS(const GSHWDrawConfig::PSSelector& sel)
	{
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%016llX_%016llX", static_cast<unsigned long long>(sel.key_hi), static_cast<unsigned long long>(sel.key_lo));
		return buf;
	}

	struct Rng
	{
		u64 state;
		u64 Next()
		{
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			return state;
		}
		u32 Range(u32 n) { return static_cast<u32>(Next() % n); }
	};

	GSHWDrawConfig::PSSelector RandomPS(Rng& rng)
	{
		static constexpr u32 aniso_values[] = {0, 2, 4, 8, 16};
		GSHWDrawConfig::PSSelector s;
		s.aem_fmt = rng.Range(3);
		s.pal_fmt = rng.Range(4);
		s.dst_fmt = rng.Range(3);
		s.depth_fmt = rng.Range(4);
		s.aem = rng.Range(2);
		s.fba = rng.Range(2);
		s.fog = rng.Range(2);
		s.iip = rng.Range(2);
		s.date = rng.Range(8);
		s.atst = static_cast<GSShader::PS_ATST>(rng.Range(5));
		s.afail = static_cast<GSShader::PS_AFAIL>(rng.Range(6));
		s.ztst = rng.Range(4);
		s.fst = rng.Range(2);
		s.tfx = rng.Range(5);
		s.tcc = rng.Range(2);
		s.wms = rng.Range(4);
		s.wmt = rng.Range(4);
		s.adjs = rng.Range(2);
		s.adjt = rng.Range(2);
		s.ltf = rng.Range(2);
		s.shuffle = rng.Range(2);
		s.shuffle_same = rng.Range(2);
		s.real16src = rng.Range(2);
		s.process_ba = rng.Range(4);
		s.process_rg = rng.Range(4);
		s.shuffle_across = rng.Range(2);
		s.write_rg = rng.Range(2);
		s.fbmask = rng.Range(2);
		s.blend_a = rng.Range(3);
		s.blend_b = rng.Range(3);
		s.blend_c = rng.Range(3);
		s.blend_d = rng.Range(3);
		s.fixed_one_a = rng.Range(2);
		s.blend_hw = rng.Range(7);
		s.a_masked = rng.Range(2);
		s.colclip_hw = rng.Range(2);
		s.rta_correction = rng.Range(2);
		s.rta_source_correction = rng.Range(2);
		s.colclip = rng.Range(2);
		s.blend_mix = rng.Range(3);
		s.round_inv = rng.Range(2);
		s.pabe = rng.Range(2);
		s.no_color = rng.Range(2);
		s.no_color1 = rng.Range(2);
		s.channel = rng.Range(7);
		s.dither = rng.Range(4);
		s.dither_adjust = rng.Range(2);
		s.zclamp = rng.Range(2);
		s.zfloor = rng.Range(2);
		s.tcoffsethack = rng.Range(2);
		s.urban_chaos_hle = rng.Range(2);
		s.tales_of_abyss_hle = rng.Range(2);
		s.tex_is_fb = rng.Range(2);
		s.automatic_lod = rng.Range(2);
		s.manual_lod = rng.Range(2);
		s.point_sampler = rng.Range(2);
		s.region_rect = rng.Range(2);
		s.scanmsk = rng.Range(4);
		s.aa1 = static_cast<GSShader::PS_AA1>(rng.Range(4));
		s.abe = rng.Range(2);
		s.sw_aniso = aniso_values[rng.Range(5)];
		return s;
	}

	std::vector<GSHWDrawConfig::PSSelector> EnumeratePSSelectors()
	{
		std::vector<GSHWDrawConfig::PSSelector> out;
		using PS = GSHWDrawConfig::PSSelector;

		const auto add = [&out](const PS& s) { out.push_back(s); };

		PS base;
		add(base);
		PS notex;
		notex.tfx = 4;
		add(notex);

		for (u32 v = 0; v < 3; v++) { PS s; s.aem_fmt = v; add(s); }
		for (u32 v = 0; v < 4; v++) { PS s; s.pal_fmt = v; add(s); }
		for (u32 v = 0; v < 3; v++) { PS s; s.dst_fmt = v; add(s); }
		for (u32 v = 0; v < 4; v++) { PS s; s.depth_fmt = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.aem = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.fba = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.fog = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.iip = v; add(s); }
		for (u32 v = 0; v < 8; v++) { PS s; s.date = v; add(s); }
		for (u32 v = 0; v < 5; v++) { PS s; s.atst = static_cast<GSShader::PS_ATST>(v); add(s); }
		for (u32 v = 0; v < 6; v++) { PS s; s.afail = static_cast<GSShader::PS_AFAIL>(v); s.atst = GSShader::PS_ATST::GEQUAL; add(s); }
		for (u32 v = 0; v < 4; v++) { PS s; s.ztst = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.fst = v; add(s); }
		for (u32 v = 0; v < 5; v++) { PS s; s.tfx = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.tcc = v; add(s); }
		for (u32 v = 0; v < 4; v++) { PS s; s.wms = v; add(s); }
		for (u32 v = 0; v < 4; v++) { PS s; s.wmt = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.adjs = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.adjt = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.ltf = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.shuffle = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.fbmask = v; add(s); }
		for (u32 v = 0; v < 7; v++) { PS s; s.blend_hw = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.a_masked = v; s.blend_c = 1; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.colclip_hw = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.rta_correction = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.rta_source_correction = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.colclip = v; add(s); }
		for (u32 v = 0; v < 3; v++) { PS s; s.blend_mix = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.round_inv = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.pabe = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.no_color = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.no_color1 = v; add(s); }
		for (u32 v = 0; v < 7; v++) { PS s; s.channel = v; add(s); }
		for (u32 v = 0; v < 4; v++) { PS s; s.dither = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.dither_adjust = v; s.dither = 1; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.zclamp = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.zfloor = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.tcoffsethack = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.urban_chaos_hle = v; s.depth_fmt = 1; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.tales_of_abyss_hle = v; s.depth_fmt = 1; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.tex_is_fb = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.automatic_lod = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.manual_lod = v; add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.region_rect = v; add(s); }
		for (u32 v = 0; v < 4; v++) { PS s; s.scanmsk = v; add(s); }
		for (u32 v = 0; v < 4; v++) { PS s; s.aa1 = static_cast<GSShader::PS_AA1>(v); add(s); }
		for (u32 v = 0; v < 2; v++) { PS s; s.abe = v; s.aa1 = GSShader::PS_AA1::TRIANGLE; add(s); }
		for (u32 v : {0u, 2u, 4u, 8u, 16u}) { PS s; s.sw_aniso = v; add(s); }

		for (u32 aem_fmt = 0; aem_fmt < 3; aem_fmt++)
			for (u32 pal_fmt = 0; pal_fmt < 4; pal_fmt++)
				for (u32 dst_fmt = 0; dst_fmt < 3; dst_fmt++)
					for (u32 depth_fmt = 0; depth_fmt < 4; depth_fmt++)
					{
						PS s;
						s.aem_fmt = aem_fmt;
						s.pal_fmt = pal_fmt;
						s.dst_fmt = dst_fmt;
						s.depth_fmt = depth_fmt;
						s.ltf = (aem_fmt + pal_fmt) & 1;
						s.fst = (dst_fmt + depth_fmt) & 1;
						s.aem = pal_fmt & 1;
						add(s);
					}

		for (u32 a = 0; a < 3; a++)
			for (u32 b = 0; b < 3; b++)
				for (u32 c = 0; c < 3; c++)
					for (u32 d = 0; d < 3; d++)
						for (u32 colclip = 0; colclip < 2; colclip++)
						{
							PS s;
							s.blend_a = a;
							s.blend_b = b;
							s.blend_c = c;
							s.blend_d = d;
							s.colclip = colclip;
							s.colclip_hw = colclip ^ (a & 1);
							s.blend_mix = (a + b) % 3;
							s.blend_hw = (c + d) % 7;
							s.pabe = (a == 0 && c == 0) ? 1 : 0;
							s.round_inv = b & 1;
							s.rta_correction = d & 1;
							s.a_masked = (c == 1) ? 1 : 0;
							s.no_color1 = 0;
							add(s);
						}

		for (u32 atst = 0; atst < 5; atst++)
			for (u32 afail = 0; afail < 6; afail++)
				for (u32 ztst = 0; ztst < 4; ztst++)
				{
					PS s;
					s.atst = static_cast<GSShader::PS_ATST>(atst);
					s.afail = static_cast<GSShader::PS_AFAIL>(afail);
					s.ztst = ztst;
					s.zclamp = ztst & 1;
					s.zfloor = (ztst >> 1) & 1;
					s.no_color1 = (afail == 4) ? 0 : 1;
					add(s);
				}

		for (u32 ba = 0; ba < 4; ba++)
			for (u32 rg = 0; rg < 4; rg++)
				for (u32 across = 0; across < 2; across++)
					for (u32 extra = 0; extra < 4; extra++)
					{
						PS s;
						s.shuffle = 1;
						s.process_ba = ba;
						s.process_rg = rg;
						s.shuffle_across = across;
						s.shuffle_same = extra & 1;
						s.real16src = (extra >> 1) & 1;
						s.write_rg = (ba + rg) & 1;
						s.fbmask = across;
						s.blend_a = across;
						s.blend_b = 1;
						s.blend_d = 1;
						s.date = (extra == 3) ? 5 : 0;
						add(s);
					}

		for (u32 channel = 0; channel < 7; channel++)
			for (u32 depth_fmt = 0; depth_fmt < 4; depth_fmt++)
			{
				PS s;
				s.channel = channel;
				s.depth_fmt = depth_fmt;
				s.tex_is_fb = channel & 1;
				add(s);
			}

		for (u32 wms = 0; wms < 4; wms++)
			for (u32 wmt = 0; wmt < 4; wmt++)
				for (u32 flags = 0; flags < 8; flags++)
				{
					PS s;
					s.wms = wms;
					s.wmt = wmt;
					s.region_rect = flags & 1;
					s.adjs = (flags >> 1) & 1;
					s.adjt = (flags >> 2) & 1;
					s.fst = wms & 1;
					s.ltf = wmt & 1;
					s.depth_fmt = (flags == 7) ? 2 : 0;
					add(s);
				}

		for (u32 aniso : {0u, 2u, 16u})
			for (u32 lod = 0; lod < 4; lod++)
				for (u32 ltf = 0; ltf < 2; ltf++)
				{
					PS s;
					s.sw_aniso = aniso;
					s.automatic_lod = lod & 1;
					s.manual_lod = (lod >> 1) & 1;
					s.ltf = ltf;
					s.fst = 0;
					add(s);
				}

		for (u32 dither = 0; dither < 4; dither++)
			for (u32 flags = 0; flags < 8; flags++)
			{
				PS s;
				s.dither = dither;
				s.dither_adjust = flags & 1;
				s.round_inv = (flags >> 1) & 1;
				s.dst_fmt = ((flags >> 2) & 1) ? 2 : 0;
				s.blend_c = flags & 3;
				s.blend_a = 1;
				s.blend_d = 2;
				s.fbmask = flags >> 2;
				add(s);
			}

		for (u32 date = 0; date < 8; date++)
			for (u32 flags = 0; flags < 4; flags++)
			{
				PS s;
				s.date = date;
				s.rta_correction = flags & 1;
				s.write_rg = (flags >> 1) & 1;
				s.no_color1 = 1;
				add(s);
			}

		for (u32 aa1 = 0; aa1 < 4; aa1++)
			for (u32 flags = 0; flags < 4; flags++)
			{
				PS s;
				s.aa1 = static_cast<GSShader::PS_AA1>(aa1);
				s.abe = flags & 1;
				s.fixed_one_a = (flags >> 1) & 1;
				s.blend_a = flags & 1;
				s.blend_b = 1;
				s.blend_d = 1;
				add(s);
			}

		u32 samples = 1000;
		if (const char* env = std::getenv("PCSX2_WEBGPU_SHADER_TEST_SAMPLES"))
			samples = static_cast<u32>(std::strtoul(env, nullptr, 10));
		Rng rng{0x50435358u};
		for (u32 i = 0; i < samples; i++)
			add(RandomPS(rng));

		return out;
	}
} // namespace

class WebGPUShaderCompile : public ::testing::Test
{
protected:
	static void SetUpTestSuite()
	{
		s_device = CreateTestDevice();
	}

	static void TearDownTestSuite()
	{
		s_device.reset();
	}

	void SetUp() override
	{
		if (!s_device)
			GTEST_SKIP() << "No WebGPU adapter available";
		s_uncaptured_errors.clear();
	}

	void TearDown() override
	{
		EXPECT_EQ(s_uncaptured_errors, "");
	}

	static std::unique_ptr<TestDevice> s_device;
};

std::unique_ptr<TestDevice> WebGPUShaderCompile::s_device;

TEST_F(WebGPUShaderCompile, TFXVertexShaders)
{
	const std::optional<std::string> source = ReadShader("tfx.wgsl");
	ASSERT_TRUE(source.has_value());

	u32 count = 0;
	for (u32 key = 0; key < 128; key++)
	{
		GSHWDrawConfig::VSSelector sel(static_cast<u8>(key));
		if (static_cast<u32>(sel.expand) > 5)
			continue;

		WGSLPreprocessor pp;
		GSDeviceWebGPU::AddTFXVertexShaderMacros(pp, sel, false);
		const std::string processed = Preprocess(pp, *source);
		const CompileResult result = CompileWGSL(*s_device, processed, "tfx vs");
		EXPECT_TRUE(result.ok) << "VS " << std::hex << key << ":\n" << result.messages;
		count++;
	}
	std::printf("Compiled %u TFX vertex shader permutations\n", count);
}

TEST_F(WebGPUShaderCompile, TFXFragmentShaders)
{
	const std::optional<std::string> source = ReadShader("tfx.wgsl");
	ASSERT_TRUE(source.has_value());

	const std::vector<GSHWDrawConfig::PSSelector> selectors = EnumeratePSSelectors();
	u32 count = 0;
	u32 failures = 0;
	for (const GSHWDrawConfig::PSSelector& sel : selectors)
	{
		for (u32 extensions = 0; extensions < 2; extensions++)
		{
			const bool dsb = (extensions != 0) && s_device->dual_source_blending;
			const bool primid = (extensions != 0) && s_device->primitive_index;
			if (extensions != 0 && !dsb && !primid)
				continue;

			WGSLPreprocessor pp;
			GSDeviceWebGPU::AddTFXFragmentShaderMacros(pp, sel);
			pp.Define("HAS_DUAL_SOURCE_BLEND", static_cast<s64>(dsb));
			pp.Define("HAS_PRIMITIVE_INDEX", static_cast<s64>(primid));
			const std::string processed = Preprocess(pp, *source);
			const CompileResult result = CompileWGSL(*s_device, processed, "tfx ps");
			if (!result.ok)
			{
				failures++;
				if (failures <= 20)
					ADD_FAILURE() << "PS " << DescribePS(sel) << " (dsb=" << dsb << " primid=" << primid << "):\n" << result.messages;
			}
			count++;
		}
	}
	EXPECT_EQ(failures, 0u);
	std::printf("Compiled %u TFX fragment shader permutations (%zu selectors), %u failures\n", count, selectors.size(), failures);
}

TEST_F(WebGPUShaderCompile, ConvertShaders)
{
	const std::optional<std::string> source = ReadShader("convert.wgsl");
	ASSERT_TRUE(source.has_value());

	{
		WGSLPreprocessor pp;
		pp.Define("VERTEX_SHADER", 1);
		const CompileResult result = CompileWGSL(*s_device, Preprocess(pp, *source), "convert vs");
		EXPECT_TRUE(result.ok) << result.messages;
	}

	u32 count = 0;
	for (u32 i = 0; i < ShaderConvertSelector::NUM_TOTAL_SHADERS; i++)
	{
		const ShaderConvertSelector shader = ShaderConvertSelector::Get(i);
		WGSLPreprocessor pp;
		GSDeviceWebGPU::AddConvertShaderMacros(pp, shader);
		pp.Define("FRAGMENT_SHADER", 1);
		pp.Define(shader.EntryPoint(), "");
		const CompileResult result = CompileWGSL(*s_device, Preprocess(pp, *source), shader.EntryPoint());
		EXPECT_TRUE(result.ok) << shader.EntryPoint() << " (mask=" << static_cast<u32>(shader.Mask()) << " depth=" << shader.DepthOutput()
							   << " biln=" << shader.Biln() << "):\n" << result.messages;
		count++;
	}
	for (u32 datm = 0; datm < 4; datm++)
	{
		const std::string entry = "ps_primid_image_init_" + std::to_string(datm);
		WGSLPreprocessor pp;
		pp.Define("FRAGMENT_SHADER", 1);
		pp.Define(entry, "");
		const CompileResult result = CompileWGSL(*s_device, Preprocess(pp, *source), entry.c_str());
		EXPECT_TRUE(result.ok) << entry << ":\n" << result.messages;
		count++;
	}
	std::printf("Compiled %u convert shader entry points\n", count);
}

TEST_F(WebGPUShaderCompile, UtilityShaders)
{
	struct Entry
	{
		const char* file;
		std::vector<const char*> fragment_entries;
	};
	const Entry entries[] = {
		{"present.wgsl", {"ps_copy", "ps_filter_scanlines", "ps_filter_diagonal", "ps_filter_triangular", "ps_filter_complex", "ps_filter_lottes", "ps_4x_rgss", "ps_automagical_supersampling"}},
		{"interlace.wgsl", {"ps_main0", "ps_main1", "ps_main2", "ps_main3", "ps_main4"}},
		{"merge.wgsl", {"ps_main0", "ps_main1"}},
		{"shadeboost.wgsl", {"ps_main"}},
		{"fxaa.wgsl", {"ps_main"}},
		{"imgui.wgsl", {"ps_main"}},
	};

	for (PresentShader i = PresentShader::COPY; i < PresentShader::Count; i = static_cast<PresentShader>(static_cast<int>(i) + 1))
		EXPECT_EQ(std::string(ShaderEntryPoint(i)), entries[0].fragment_entries[static_cast<int>(i)]);

	u32 count = 0;
	for (const Entry& entry : entries)
	{
		const std::optional<std::string> source = ReadShader(entry.file);
		ASSERT_TRUE(source.has_value()) << entry.file;

		{
			WGSLPreprocessor pp;
			pp.Define("VERTEX_SHADER", 1);
			const CompileResult result = CompileWGSL(*s_device, Preprocess(pp, *source), entry.file);
			EXPECT_TRUE(result.ok) << entry.file << " (vs):\n" << result.messages;
			count++;
		}

		for (const char* fs : entry.fragment_entries)
		{
			WGSLPreprocessor pp;
			pp.Define("FRAGMENT_SHADER", 1);
			pp.Define(fs, "");
			const CompileResult result = CompileWGSL(*s_device, Preprocess(pp, *source), fs);
			EXPECT_TRUE(result.ok) << entry.file << " (" << fs << "):\n" << result.messages;
			count++;
		}
	}
	std::printf("Compiled %u utility shader entry points\n", count);
}
