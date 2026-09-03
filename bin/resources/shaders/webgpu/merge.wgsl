// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifdef VERTEX_SHADER

struct VSInput
{
	@location(0) pos: vec4f,
	@location(1) tex: vec2f,
};

struct VSOutput
{
	@builtin(position) p: vec4f,
	@location(0) tex: vec2f,
};

@vertex fn vs_main(input: VSInput) -> VSOutput
{
	var output: VSOutput;
	output.p = input.pos;
	output.tex = input.tex;
	return output;
}

#endif

#ifdef FRAGMENT_SHADER

struct PSInput
{
	@builtin(position) p: vec4f,
	@location(0) tex: vec2f,
};

struct MergeUniforms
{
	BGColor: vec4f,
};

@group(0) @binding(0) var samp0: texture_2d<f32>;
@group(0) @binding(1) var samp0_s: sampler;
@group(0) @binding(2) var<uniform> cb: MergeUniforms;

#ifdef ps_main0
@fragment fn ps_main0(input: PSInput) -> @location(0) vec4f
{
	var c = textureSampleLevel(samp0, samp0_s, input.tex, 0.0);
	c.a *= 2.0;
	return c;
}
#endif

#ifdef ps_main1
@fragment fn ps_main1(input: PSInput) -> @location(0) vec4f
{
	var c = textureSampleLevel(samp0, samp0_s, input.tex, 0.0);
	c.a = cb.BGColor.a;
	return c;
}
#endif

#endif
