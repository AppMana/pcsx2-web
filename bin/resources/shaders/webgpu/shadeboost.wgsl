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

struct ShadeBoostUniforms
{
	params: vec4f,
};

@group(0) @binding(0) var samp0: texture_2d<f32>;
@group(0) @binding(1) var samp0_s: sampler;
@group(0) @binding(2) var<uniform> cb: ShadeBoostUniforms;

fn ContrastSaturationBrightness(color: vec4f) -> vec4f
{
	let brt = cb.params.x;
	let con = cb.params.y;
	let sat = cb.params.z;
	let gam = cb.params.w;

	let AvgLumin = vec3f(0.5, 0.5, 0.5);
	let LumCoeff = vec3f(0.2125, 0.7154, 0.0721);

	let brtColor = color.rgb * brt;
	let dot_intensity = dot(brtColor, LumCoeff);
	let intensity = vec3f(dot_intensity, dot_intensity, dot_intensity);
	let satColor = mix(intensity, brtColor, sat);
	let conColor = mix(AvgLumin, satColor, con);

	let csb = pow(conColor, vec3f(1.0 / gam));
	return vec4f(csb, color.a);
}

@fragment fn ps_main(input: PSInput) -> @location(0) vec4f
{
	let c = textureSampleLevel(samp0, samp0_s, input.tex, 0.0);
	return ContrastSaturationBrightness(c);
}

#endif
