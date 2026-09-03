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

struct DisplayUniforms
{
	u_source_rect: vec4f,
	u_target_rect: vec4f,
	u_source_size: vec2f,
	u_target_size: vec2f,
	u_target_resolution: vec2f,
	u_rcp_target_resolution: vec2f,
	u_source_resolution: vec2f,
	u_rcp_source_resolution: vec2f,
	u_time: vec4f,
};

struct PSInput
{
	@builtin(position) p: vec4f,
	@location(0) tex: vec2f,
};

@group(0) @binding(0) var samp0: texture_2d<f32>;
@group(0) @binding(1) var samp0_s: sampler;
@group(0) @binding(2) var<uniform> cb: DisplayUniforms;

fn sample_c(uv: vec2f) -> vec4f
{
	return textureSampleLevel(samp0, samp0_s, uv, 0.0);
}

fn ps_crt(uv: vec2f, i: u32) -> vec4f
{
	var mask = array<vec4f, 4>(
		vec4f(1.0, 0.0, 0.0, 0.0),
		vec4f(0.0, 1.0, 0.0, 0.0),
		vec4f(0.0, 0.0, 1.0, 0.0),
		vec4f(1.0, 1.0, 1.0, 0.0));
	return sample_c(uv) * clamp((mask[i] + 0.5), vec4f(0.0), vec4f(1.0));
}

fn ps_scanlines(uv: vec2f, i: u32) -> vec4f
{
	var mask = array<vec4f, 2>(
		vec4f(1.0, 1.0, 1.0, 0.0),
		vec4f(0.0, 0.0, 0.0, 0.0));
	return sample_c(uv) * clamp((mask[i] + 0.5), vec4f(0.0), vec4f(1.0));
}

#ifdef ps_copy
@fragment fn ps_copy(input: PSInput) -> @location(0) vec4f
{
	return sample_c(input.tex);
}
#endif

#ifdef ps_filter_scanlines
@fragment fn ps_filter_scanlines(input: PSInput) -> @location(0) vec4f
{
	let p = vec4u(input.p);
	return ps_scanlines(input.tex, p.y % 2u);
}
#endif

#ifdef ps_filter_diagonal
@fragment fn ps_filter_diagonal(input: PSInput) -> @location(0) vec4f
{
	let p = vec4u(input.p);
	return ps_crt(input.tex, (p.x + (p.y % 3u)) % 3u);
}
#endif

#ifdef ps_filter_triangular
@fragment fn ps_filter_triangular(input: PSInput) -> @location(0) vec4f
{
	let p = vec4u(input.p);
	return ps_crt(input.tex, ((p.x + ((p.y >> 1u) & 1u) * 3u) >> 1u) % 3u);
}
#endif

#ifdef ps_filter_complex
@fragment fn ps_filter_complex(input: PSInput) -> @location(0) vec4f
{
	let PI = 3.14159265359;
	let texdim = vec2f(textureDimensions(samp0, 0));

	return (0.9 - 0.4 * cos(2.0 * PI * input.tex.y * texdim.y)) * sample_c(vec2f(input.tex.x, (floor(input.tex.y * texdim.y) + 0.5) / texdim.y));
}
#endif

#ifdef ps_filter_lottes

#define MaskingType 4
#define ScanBrightness -8.00
#define FilterCRTAmount -3.00
#define HorizontalWarp 0.00
#define VerticalWarp 0.00
#define MaskAmountDark 0.50
#define MaskAmountLight 1.50
#define BloomPixel -1.50
#define BloomScanLine -2.0
#define BloomAmount 0.15
#define Shape 2.0
#define UseShadowMask 1

fn ToLinear1(c: f32) -> f32
{
	return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
}

fn ToLinear(c: vec3f) -> vec3f
{
	return vec3f(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b));
}

fn ToSrgb1(c: f32) -> f32
{
	return select(1.055 * pow(c, 0.41666) - 0.055, c * 12.92, c < 0.0031308);
}

fn ToSrgb(c: vec3f) -> vec3f
{
	return vec3f(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b));
}

fn Fetch(pos_in: vec2f, off: vec2f) -> vec3f
{
	let pos = (floor(pos_in * cb.u_target_size + off) + vec2f(0.5, 0.5)) / cb.u_target_size;
	if (max(abs(pos.x - 0.5), abs(pos.y - 0.5)) > 0.5)
	{
		return vec3f(0.0, 0.0, 0.0);
	}
	else
	{
		return ToLinear(sample_c(pos.xy).rgb);
	}
}

fn Dist(pos_in: vec2f) -> vec2f
{
	let pos = pos_in * vec2f(640.0, 480.0);
	return -((pos - floor(pos)) - vec2f(0.5, 0.5));
}

fn Gaus(pos: f32, scale: f32) -> f32
{
	return exp2(scale * pow(abs(pos), Shape));
}

fn Horz3(pos: vec2f, off: f32) -> vec3f
{
	let b = Fetch(pos, vec2f(-1.0, off));
	let c = Fetch(pos, vec2f(0.0, off));
	let d = Fetch(pos, vec2f(1.0, off));
	let dst = Dist(pos).x;

	let scale = FilterCRTAmount;
	let wb = Gaus(dst - 1.0, scale);
	let wc = Gaus(dst + 0.0, scale);
	let wd = Gaus(dst + 1.0, scale);

	return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

fn Horz5(pos: vec2f, off: f32) -> vec3f
{
	let a = Fetch(pos, vec2f(-2.0, off));
	let b = Fetch(pos, vec2f(-1.0, off));
	let c = Fetch(pos, vec2f(0.0, off));
	let d = Fetch(pos, vec2f(1.0, off));
	let e = Fetch(pos, vec2f(2.0, off));
	let dst = Dist(pos).x;

	let scale = FilterCRTAmount;

	let wa = Gaus(dst - 2.0, scale);
	let wb = Gaus(dst - 1.0, scale);
	let wc = Gaus(dst + 0.0, scale);
	let wd = Gaus(dst + 1.0, scale);
	let we = Gaus(dst + 2.0, scale);

	return (a * wa + b * wb + c * wc + d * wd + e * we) / (wa + wb + wc + wd + we);
}

fn Horz7(pos: vec2f, off: f32) -> vec3f
{
	let a = Fetch(pos, vec2f(-3.0, off));
	let b = Fetch(pos, vec2f(-2.0, off));
	let c = Fetch(pos, vec2f(-1.0, off));
	let d = Fetch(pos, vec2f(0.0, off));
	let e = Fetch(pos, vec2f(1.0, off));
	let f = Fetch(pos, vec2f(2.0, off));
	let g = Fetch(pos, vec2f(3.0, off));

	let dst = Dist(pos).x;
	let scale = BloomPixel;
	let wa = Gaus(dst - 3.0, scale);
	let wb = Gaus(dst - 2.0, scale);
	let wc = Gaus(dst - 1.0, scale);
	let wd = Gaus(dst + 0.0, scale);
	let we = Gaus(dst + 1.0, scale);
	let wf = Gaus(dst + 2.0, scale);
	let wg = Gaus(dst + 3.0, scale);

	return (a * wa + b * wb + c * wc + d * wd + e * we + f * wf + g * wg) / (wa + wb + wc + wd + we + wf + wg);
}

fn Scan(pos: vec2f, off: f32) -> f32
{
	let dst = Dist(pos).y;
	return Gaus(dst + off, ScanBrightness);
}

fn BloomScan(pos: vec2f, off: f32) -> f32
{
	let dst = Dist(pos).y;
	return Gaus(dst + off, BloomScanLine);
}

fn Tri(pos: vec2f) -> vec3f
{
	let a = Horz3(pos, -1.0);
	let b = Horz5(pos, 0.0);
	let c = Horz3(pos, 1.0);

	let wa = Scan(pos, -1.0);
	let wb = Scan(pos, 0.0);
	let wc = Scan(pos, 1.0);

	return (a * wa) + (b * wb) + (c * wc);
}

fn Bloom(pos: vec2f) -> vec3f
{
	let a = Horz5(pos, -2.0);
	let b = Horz7(pos, -1.0);
	let c = Horz7(pos, 0.0);
	let d = Horz7(pos, 1.0);
	let e = Horz5(pos, 2.0);

	let wa = BloomScan(pos, -2.0);
	let wb = BloomScan(pos, -1.0);
	let wc = BloomScan(pos, 0.0);
	let wd = BloomScan(pos, 1.0);
	let we = BloomScan(pos, 2.0);

	return a * wa + b * wb + c * wc + d * wd + e * we;
}

fn Warp(pos_in: vec2f) -> vec2f
{
	var pos = pos_in * 2.0 - 1.0;
	pos *= vec2f(1.0 + (pos.y * pos.y) * HorizontalWarp, 1.0 + (pos.x * pos.x) * VerticalWarp);
	return pos * 0.5 + 0.5;
}

fn Mask(pos_in: vec2f) -> vec3f
{
	var pos = pos_in;
#if MaskingType == 1
	var lines = MaskAmountLight;
	var odd = 0.0;

	if (fract(pos.x / 6.0) < 0.5)
	{
		odd = 1.0;
	}
	if (fract((pos.y + odd) / 2.0) < 0.5)
	{
		lines = MaskAmountDark;
	}
	pos.x = fract(pos.x / 3.0);
	var mask = vec3f(MaskAmountDark, MaskAmountDark, MaskAmountDark);

	if (pos.x < 0.333)
	{
		mask.r = MaskAmountLight;
	}
	else if (pos.x < 0.666)
	{
		mask.g = MaskAmountLight;
	}
	else
	{
		mask.b = MaskAmountLight;
	}

	mask *= lines;

	return mask;

#elif MaskingType == 2
	pos.x = fract(pos.x / 3.0);
	var mask = vec3f(MaskAmountDark, MaskAmountDark, MaskAmountDark);

	if (pos.x < 0.333)
	{
		mask.r = MaskAmountLight;
	}
	else if (pos.x < 0.666)
	{
		mask.g = MaskAmountLight;
	}
	else
	{
		mask.b = MaskAmountLight;
	}

	return mask;

#elif MaskingType == 3
	pos.x += pos.y * 3.0;
	var mask = vec3f(MaskAmountDark, MaskAmountDark, MaskAmountDark);
	pos.x = fract(pos.x / 6.0);

	if (pos.x < 0.333)
	{
		mask.r = MaskAmountLight;
	}
	else if (pos.x < 0.666)
	{
		mask.g = MaskAmountLight;
	}
	else
	{
		mask.b = MaskAmountLight;
	}

	return mask;

#else
	pos = floor(pos.xy * vec2f(1.0, 0.5));
	pos.x += pos.y * 3.0;

	var mask = vec3f(MaskAmountDark, MaskAmountDark, MaskAmountDark);
	pos.x = fract(pos.x / 6.0);

	if (pos.x < 0.333)
	{
		mask.r = MaskAmountLight;
	}
	else if (pos.x < 0.666)
	{
		mask.g = MaskAmountLight;
	}
	else
	{
		mask.b = MaskAmountLight;
	}
	return mask;
#endif
}

fn LottesCRTPass(input: PSInput) -> vec4f
{
	var color: vec4f;
	let fragcoord = input.p - cb.u_target_rect;
	let inSize = cb.u_target_resolution - (2.0 * cb.u_target_rect.xy);

	let pos = Warp(fragcoord.xy / inSize);
	var rgb = Tri(pos);
	rgb += Bloom(pos) * BloomAmount;
#if UseShadowMask
	rgb *= Mask(fragcoord.xy);
#endif
	rgb = ToSrgb(rgb);

	return vec4f(rgb, 1.0);
}

@fragment fn ps_filter_lottes(input: PSInput) -> @location(0) vec4f
{
	return LottesCRTPass(input);
}

#endif

#ifdef ps_4x_rgss
@fragment fn ps_4x_rgss(input: PSInput) -> @location(0) vec4f
{
	let dxy = vec2f(dpdx(input.tex.x), dpdy(input.tex.y));
	var color = vec3f(0.0);

	let s = 1.0 / 8.0;
	let l = 3.0 / 8.0;

	color += sample_c(input.tex + vec2f(s, l) * dxy).rgb;
	color += sample_c(input.tex + vec2f(l, -s) * dxy).rgb;
	color += sample_c(input.tex + vec2f(-s, -l) * dxy).rgb;
	color += sample_c(input.tex + vec2f(-l, s) * dxy).rgb;

	return vec4f(color * 0.25, 1.0);
}
#endif

#ifdef ps_automagical_supersampling
@fragment fn ps_automagical_supersampling(input: PSInput) -> @location(0) vec4f
{
	let ratio = (cb.u_source_size / cb.u_target_size) * 0.5;
	let steps = floor(ratio);
	var col = sample_c(input.tex).rgb;
	var div = 1.0;

	for (var y = 0.0; y < steps.y; y += 1.0)
	{
		for (var x = 0.0; x < steps.x; x += 1.0)
		{
			let offset = vec2f(x, y) - ratio * 0.5;
			col += sample_c(input.tex + offset * cb.u_rcp_source_resolution * 2.0).rgb;
			div += 1.0;
		}
	}

	return vec4f(col / div, 1.0);
}
#endif

#endif
