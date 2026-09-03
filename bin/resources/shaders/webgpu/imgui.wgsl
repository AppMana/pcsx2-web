// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

struct ImGuiUniforms
{
	uScale: vec2f,
	uTranslate: vec2f,
};

@group(0) @binding(2) var<uniform> cb: ImGuiUniforms;

#ifdef VERTEX_SHADER

struct VSInput
{
	@location(0) Position: vec2f,
	@location(1) UV: vec2f,
	@location(2) Color: vec4f,
};

struct VSOutput
{
	@builtin(position) p: vec4f,
	@location(0) Frag_UV: vec2f,
	@location(1) Frag_Color: vec4f,
};

@vertex fn vs_main(input: VSInput) -> VSOutput
{
	var output: VSOutput;
	output.Frag_UV = input.UV;
	output.Frag_Color = input.Color;
	output.p = vec4f(input.Position * cb.uScale + cb.uTranslate, 0.0, 1.0);
	return output;
}

#endif

#ifdef FRAGMENT_SHADER

@group(0) @binding(0) var Texture: texture_2d<f32>;
@group(0) @binding(1) var TextureSampler: sampler;

struct PSInput
{
	@builtin(position) p: vec4f,
	@location(0) Frag_UV: vec2f,
	@location(1) Frag_Color: vec4f,
};

@fragment fn ps_main(input: PSInput) -> @location(0) vec4f
{
	return input.Frag_Color * textureSampleLevel(Texture, TextureSampler, input.Frag_UV, 0.0);
}

#endif
