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
	output.p = vec4f(input.pos.x, input.pos.y, input.pos.z, input.pos.w);
	output.tex = input.tex;
	return output;
}

#endif

#ifdef FRAGMENT_SHADER

#ifndef HAS_BILN
#define HAS_BILN 0
#define HAS_STENCIL_OUTPUT 0
#define HAS_INTEGER_OUTPUT 0
#define HAS_DEPTH_OUTPUT 0
#define HAS_FLOAT32_INPUT 0
#define HAS_FLOAT32_OUTPUT 0
#endif

struct PSInput
{
	@builtin(position) p: vec4f,
	@location(0) tex: vec2f,
};

#if HAS_INTEGER_OUTPUT
struct PSOutput
{
	@location(0) c: u32,
};
#elif HAS_DEPTH_OUTPUT
struct PSOutput
{
	@builtin(frag_depth) c: f32,
};
#elif HAS_FLOAT32_OUTPUT
struct PSOutput
{
	@location(0) c: f32,
};
#elif HAS_STENCIL_OUTPUT
#else
struct PSOutput
{
	@location(0) c: vec4f,
};
#endif

@group(0) @binding(0) var samp0: texture_2d<f32>;
@group(0) @binding(1) var samp0_s: sampler;

#if HAS_FLOAT32_INPUT

fn sample_c(uv: vec2f) -> f32
{
	return textureSampleLevel(samp0, samp0_s, uv, 0.0).r;
}

#else

fn sample_c(uv: vec2f) -> vec4f
{
	return textureSampleLevel(samp0, samp0_s, uv, 0.0);
}

#endif

fn rgba8_to_uint(c: vec4f) -> u32
{
	let i = vec4u(c * 255.5) & vec4u(0xFFu);
	return i.r | (i.g << 8u) | (i.b << 16u) | (i.a << 24u);
}

fn rgb5a1_to_uint(c: vec4f) -> u32
{
	let i = vec4u(c * 255.5) & vec4u(0xF8u, 0xF8u, 0xF8u, 0x80u);
	return (i.r >> 3u) | (i.g << 2u) | (i.b << 7u) | (i.a << 8u);
}

fn depth_to_uint(d: f32) -> u32
{
	return u32(d * exp2(32.0));
}

fn uint_to_rgba8(i: u32) -> vec4f
{
	return vec4f(f32(i & 0xFFu), f32((i >> 8u) & 0xFFu), f32((i >> 16u) & 0xFFu), f32((i >> 24u) & 0xFFu)) / 255.0;
}

fn uint_to_rgb5a1(i: u32) -> vec4f
{
	return vec4f(vec4u(i << 3u, i >> 2u, i >> 7u, i >> 8u) & vec4u(0xF8u, 0xF8u, 0xF8u, 0x80u)) / 255.0;
}

fn uint_to_depth32(i: u32) -> f32
{
	return f32(i) * exp2(-32.0);
}

fn uint_to_depth24(i: u32) -> f32
{
	return f32(i & 0xFFFFFFu) * exp2(-32.0);
}

fn uint_to_depth16(i: u32) -> f32
{
	return f32(i & 0xFFFFu) * exp2(-32.0);
}

fn rgba8_to_depth32(val: vec4f) -> f32
{
	return uint_to_depth32(rgba8_to_uint(val));
}

fn rgba8_to_depth24(val: vec4f) -> f32
{
	return uint_to_depth24(rgba8_to_uint(val));
}

fn rgba8_to_depth16(val: vec4f) -> f32
{
	return uint_to_depth16(rgba8_to_uint(val));
}

fn rgb5a1_to_depth16(val: vec4f) -> f32
{
	return uint_to_depth16(rgb5a1_to_uint(val));
}

fn depth32_to_rgba8(d: f32) -> vec4f
{
	return uint_to_rgba8(depth_to_uint(d));
}

fn depth16_to_rgb5a1(d: f32) -> vec4f
{
	return uint_to_rgb5a1(depth_to_uint(d));
}

fn depth32_to_depth24(d: f32) -> f32
{
	return uint_to_depth24(depth_to_uint(d));
}

#ifdef ps_copy
@fragment fn ps_copy(input: PSInput) -> PSOutput
{
	var o: PSOutput;
	o.c = sample_c(input.tex);
	return o;
}
#endif

#ifdef ps_depth_copy
@fragment fn ps_depth_copy(input: PSInput) -> PSOutput
{
	var o: PSOutput;
	o.c = sample_c(input.tex);
	return o;
}
#endif

#ifdef ps_downsample_copy
struct DownsampleUniforms
{
	ClampMin: vec2i,
	DownsampleFactor: i32,
	pad0: i32,
	Weight: f32,
	step_multiplier: f32,
	pad1: vec2f,
};
@group(0) @binding(2) var<uniform> cb: DownsampleUniforms;

@fragment fn ps_downsample_copy(input: PSInput) -> PSOutput
{
	let coord = max(vec2i(input.p.xy) * cb.DownsampleFactor, cb.ClampMin);
	var result = vec4f(0.0);
	for (var yoff = 0; yoff < cb.DownsampleFactor; yoff++)
	{
		for (var xoff = 0; xoff < cb.DownsampleFactor; xoff++)
		{
			result += textureLoad(samp0, coord + vec2i(i32(f32(xoff) * cb.step_multiplier), i32(f32(yoff) * cb.step_multiplier)), 0);
		}
	}
	var o: PSOutput;
	o.c = result / cb.Weight;
	return o;
}
#endif

#ifdef ps_filter_transparency
@fragment fn ps_filter_transparency(input: PSInput) -> PSOutput
{
	let c = sample_c(input.tex);
	var o: PSOutput;
	o.c = vec4f(c.rgb, 1.0);
	return o;
}
#endif

#ifdef ps_convert_rgb5a1_16bits
@fragment fn ps_convert_rgb5a1_16bits(input: PSInput) -> PSOutput
{
	var o: PSOutput;
	o.c = rgb5a1_to_uint(sample_c(input.tex));
	return o;
}
#endif

#ifdef ps_datm1
@fragment fn ps_datm1(input: PSInput)
{
	if (sample_c(input.tex).a < (127.5 / 255.0))
	{
		discard;
	}
}
#endif

#ifdef ps_datm0
@fragment fn ps_datm0(input: PSInput)
{
	if ((127.5 / 255.0) < sample_c(input.tex).a)
	{
		discard;
	}
}
#endif

#ifdef ps_datm1_rta_correction
@fragment fn ps_datm1_rta_correction(input: PSInput)
{
	if (sample_c(input.tex).a < (254.5 / 255.0))
	{
		discard;
	}
}
#endif

#ifdef ps_datm0_rta_correction
@fragment fn ps_datm0_rta_correction(input: PSInput)
{
	if ((254.5 / 255.0) < sample_c(input.tex).a)
	{
		discard;
	}
}
#endif

#ifdef ps_rta_correction
@fragment fn ps_rta_correction(input: PSInput) -> PSOutput
{
	let value = sample_c(input.tex);
	var o: PSOutput;
	o.c = vec4f(value.rgb, value.a / (128.25 / 255.0));
	return o;
}
#endif

#ifdef ps_rta_decorrection
@fragment fn ps_rta_decorrection(input: PSInput) -> PSOutput
{
	let value = sample_c(input.tex);
	var o: PSOutput;
	o.c = vec4f(value.rgb, value.a * (128.25 / 255.0));
	return o;
}
#endif

#ifdef ps_colclip_init
@fragment fn ps_colclip_init(input: PSInput) -> PSOutput
{
	let value = sample_c(input.tex);
	var o: PSOutput;
	o.c = vec4f(round(value.rgb * 255.0) / 65535.0, value.a);
	return o;
}
#endif

#ifdef ps_colclip_resolve
@fragment fn ps_colclip_resolve(input: PSInput) -> PSOutput
{
	let value = sample_c(input.tex);
	var o: PSOutput;
	o.c = vec4f(vec3f(vec3u(value.rgb * 65535.5) & vec3u(255u)) / 255.0, value.a);
	return o;
}
#endif

#ifdef ps_convert_depth32_32bits
@fragment fn ps_convert_depth32_32bits(input: PSInput) -> PSOutput
{
	var o: PSOutput;
	o.c = depth_to_uint(sample_c(input.tex));
	return o;
}
#endif

#ifdef ps_convert_depth32_rgba8
@fragment fn ps_convert_depth32_rgba8(input: PSInput) -> PSOutput
{
	var o: PSOutput;
	o.c = depth32_to_rgba8(sample_c(input.tex));
	return o;
}
#endif

#ifdef ps_convert_depth16_rgb5a1
@fragment fn ps_convert_depth16_rgb5a1(input: PSInput) -> PSOutput
{
	var o: PSOutput;
	o.c = depth16_to_rgb5a1(sample_c(input.tex));
	return o;
}
#endif

#ifdef ps_convert_depth32_depth24
@fragment fn ps_convert_depth32_depth24(input: PSInput) -> PSOutput
{
	var o: PSOutput;
	o.c = depth32_to_depth24(sample_c(input.tex));
	return o;
}
#endif

#if HAS_BILN
struct BilnSamples
{
	c: array<vec4f, 4>,
	mix_vals: vec2f,
};

fn biln_fetch(uv: vec2f) -> BilnSamples
{
	let dims = vec2i(textureDimensions(samp0, 0));
	let top_left_f = uv * vec2f(dims) - 0.5;
	let top_left = vec2i(floor(top_left_f));
	let coords = clamp(vec4i(top_left, top_left + 1), vec4i(0), dims.xyxy - 1);
	var s: BilnSamples;
	s.mix_vals = fract(top_left_f);
	s.c[0] = textureLoad(samp0, coords.xy, 0);
	s.c[1] = textureLoad(samp0, coords.zy, 0);
	s.c[2] = textureLoad(samp0, coords.xw, 0);
	s.c[3] = textureLoad(samp0, coords.zw, 0);
	return s;
}
#endif

#ifdef ps_convert_rgba8_depth32
@fragment fn ps_convert_rgba8_depth32(input: PSInput) -> PSOutput
{
	var o: PSOutput;
#if HAS_BILN
	let s = biln_fetch(input.tex);
	o.c = mix(mix(rgba8_to_depth32(s.c[0]), rgba8_to_depth32(s.c[1]), s.mix_vals.x), mix(rgba8_to_depth32(s.c[2]), rgba8_to_depth32(s.c[3]), s.mix_vals.x), s.mix_vals.y);
#else
	o.c = rgba8_to_depth32(sample_c(input.tex));
#endif
	return o;
}
#endif

#ifdef ps_convert_rgba8_depth24
@fragment fn ps_convert_rgba8_depth24(input: PSInput) -> PSOutput
{
	var o: PSOutput;
#if HAS_BILN
	let s = biln_fetch(input.tex);
	o.c = mix(mix(rgba8_to_depth24(s.c[0]), rgba8_to_depth24(s.c[1]), s.mix_vals.x), mix(rgba8_to_depth24(s.c[2]), rgba8_to_depth24(s.c[3]), s.mix_vals.x), s.mix_vals.y);
#else
	o.c = rgba8_to_depth24(sample_c(input.tex));
#endif
	return o;
}
#endif

#ifdef ps_convert_rgba8_depth16
@fragment fn ps_convert_rgba8_depth16(input: PSInput) -> PSOutput
{
	var o: PSOutput;
#if HAS_BILN
	let s = biln_fetch(input.tex);
	o.c = mix(mix(rgba8_to_depth16(s.c[0]), rgba8_to_depth16(s.c[1]), s.mix_vals.x), mix(rgba8_to_depth16(s.c[2]), rgba8_to_depth16(s.c[3]), s.mix_vals.x), s.mix_vals.y);
#else
	o.c = rgba8_to_depth16(sample_c(input.tex));
#endif
	return o;
}
#endif

#ifdef ps_convert_rgb5a1_depth16
@fragment fn ps_convert_rgb5a1_depth16(input: PSInput) -> PSOutput
{
	var o: PSOutput;
#if HAS_BILN
	let s = biln_fetch(input.tex);
	o.c = mix(mix(rgb5a1_to_depth16(s.c[0]), rgb5a1_to_depth16(s.c[1]), s.mix_vals.x), mix(rgb5a1_to_depth16(s.c[2]), rgb5a1_to_depth16(s.c[3]), s.mix_vals.x), s.mix_vals.y);
#else
	o.c = rgb5a1_to_depth16(sample_c(input.tex));
#endif
	return o;
}
#endif

#if defined(ps_convert_rgb5a1_8i) || defined(ps_convert_rgba_8i)
struct IndexedUniforms
{
	SBW: u32,
	DBW: u32,
	PSM: u32,
	cb_pad1: f32,
	ScaleFactor: f32,
	cb_pad2: vec3f,
};
@group(0) @binding(2) var<uniform> cb: IndexedUniforms;
#endif

#ifdef ps_convert_rgb5a1_8i
@fragment fn ps_convert_rgb5a1_8i(input: PSInput) -> PSOutput
{
	let pos = vec2u(input.p.xy);

	var column = (pos & ~vec2u(0u, 3u)) / vec2u(1u, 2u);
	let subcolumn = (pos & vec2u(0u, 1u));
	column.x -= (column.x / 128u) * 64u;
	column.y += (column.y / 32u) * 32u;

	if ((cb.PSM & 0x8u) != 0u)
	{
		if ((pos.x & 32u) != 0u)
		{
			column.y += 32u;
			column.x &= ~32u;
		}

		if ((pos.x & 64u) != 0u)
		{
			column.x -= 32u;
		}

		if (((pos.x & 16u) != 0u) != ((pos.y & 16u) != 0u))
		{
			column.x ^= 16u;
			column.y ^= 8u;
		}

		if ((cb.PSM & 0x30u) != 0u)
		{
			column.x ^= 32u;
			column.y ^= 16u;
		}
	}
	else
	{
		if ((pos.y & 32u) != 0u)
		{
			column.y -= 16u;
			column.x += 32u;
		}

		if ((pos.x & 96u) != 0u)
		{
			let multi = (pos.x & 96u) / 32u;
			column.y += 16u * multi;
			column.x -= (pos.x & 96u);
		}

		if (((pos.x & 16u) != 0u) != ((pos.y & 16u) != 0u))
		{
			column.x ^= 16u;
			column.y ^= 8u;
		}

		if ((cb.PSM & 0x30u) != 0u)
		{
			column.x ^= 32u;
			column.y ^= 32u;
		}
	}
	var coord = column | subcolumn;

	let block_xy = coord / vec2u(64u, 64u);
	let block_num = (block_xy.y * (cb.DBW / 128u)) + block_xy.x;
	let block_offset = vec2u((block_num % (cb.SBW / 64u)) * 64u, (block_num / (cb.SBW / 64u)) * 64u);
	coord = (coord % vec2u(64u, 64u)) + block_offset;

	let is_col23 = pos.y & 4u;
	let is_col13 = pos.y & 2u;
	let is_col12 = is_col23 ^ (is_col13 << 1u);
	coord.x ^= is_col12;

	if (floor(cb.ScaleFactor) != cb.ScaleFactor)
	{
		coord = vec2u(vec2f(coord) * cb.ScaleFactor);
	}
	else
	{
		coord *= vec2u(u32(cb.ScaleFactor));
	}

	let pixel = textureLoad(samp0, vec2i(coord), 0);

	let denorm_c = vec4u(pixel * 255.5);
	var o: PSOutput;
	if ((pos.y & 2u) == 0u)
	{
		let red = (denorm_c.r >> 3u) & 0x1Fu;
		let green = (denorm_c.g >> 3u) & 0x1Fu;

		o.c = vec4f(f32(((green << 5u) | red) & 0xFFu) / 255.0);
	}
	else
	{
		let green = (denorm_c.g >> 3u) & 0x1Fu;
		let blue = (denorm_c.b >> 3u) & 0x1Fu;
		let alpha = denorm_c.a & 0x80u;

		o.c = vec4f(f32((alpha | (blue << 2u) | (green >> 3u)) & 0xFFu) / 255.0);
	}
	return o;
}
#endif

#ifdef ps_convert_rgba_8i
@fragment fn ps_convert_rgba_8i(input: PSInput) -> PSOutput
{
	let pos = vec2u(input.p.xy);

	let block = (pos & ~vec2u(15u, 3u)) >> vec2u(1u);
	let subblock = pos & vec2u(7u, 1u);
	var coord = block | subblock;

	let block_xy = coord / vec2u(64u, 32u);
	let block_num = (block_xy.y * (cb.DBW / 128u)) + block_xy.x;
	let block_offset = vec2u((block_num % (cb.SBW / 64u)) * 64u, (block_num / (cb.SBW / 64u)) * 32u);
	coord = (coord % vec2u(64u, 32u)) + block_offset;

	let is_col23 = pos.y & 4u;
	let is_col13 = pos.y & 2u;
	let is_col12 = is_col23 ^ (is_col13 << 1u);
	coord.x ^= is_col12;

	if (floor(cb.ScaleFactor) != cb.ScaleFactor)
	{
		coord = vec2u(vec2f(coord) * cb.ScaleFactor);
	}
	else
	{
		coord *= vec2u(u32(cb.ScaleFactor));
	}

	let pixel = textureLoad(samp0, vec2i(coord), 0);
	let sel0 = select(pixel.ga, pixel.rb, (pos.y & 2u) == 0u);
	let sel1 = select(sel0.y, sel0.x, (pos.x & 8u) == 0u);
	var o: PSOutput;
	o.c = vec4f(sel1);
	return o;
}
#endif

#if defined(ps_convert_clut_4) || defined(ps_convert_clut_8)
struct CLUTUniforms
{
	offset: vec2u,
	doffset: u32,
	cb_pad1: u32,
	scale: f32,
	cb_pad2: vec3f,
};
@group(0) @binding(2) var<uniform> cb: CLUTUniforms;
#endif

#ifdef ps_convert_clut_4
@fragment fn ps_convert_clut_4(input: PSInput) -> PSOutput
{
	let index = u32(input.p.x) + cb.doffset;
	let pos = vec2u(index % 8u, index / 8u);

	let coord = vec2i(floor(vec2f(cb.offset + pos) * vec2f(cb.scale)));
	var o: PSOutput;
	o.c = textureLoad(samp0, coord, 0);
	return o;
}
#endif

#ifdef ps_convert_clut_8
@fragment fn ps_convert_clut_8(input: PSInput) -> PSOutput
{
	let index = min(u32(input.p.x) + cb.doffset, 255u);

	let subgroup = (index / 8u) % 4u;
	var pos: vec2u;
	pos.x = (index % 8u) + select(0u, 8u, subgroup >= 2u);
	pos.y = ((index / 32u) * 2u) + (subgroup % 2u);

	let coord = vec2i(floor(vec2f(cb.offset + pos) * vec2f(cb.scale)));
	var o: PSOutput;
	o.c = textureLoad(samp0, coord, 0);
	return o;
}
#endif

#ifdef ps_yuv
struct YUVUniforms
{
	EMODA: i32,
	EMODC: i32,
};
@group(0) @binding(2) var<uniform> cb: YUVUniforms;

@fragment fn ps_yuv(input: PSInput) -> PSOutput
{
	let i = sample_c(input.tex);
	var o = vec4f(0.0);

	let rgb2yuv = mat3x3f(vec3f(0.587, -0.311, -0.419), vec3f(0.114, 0.500, -0.081), vec3f(0.299, -0.169, 0.500));

	let yuv = rgb2yuv * i.gbr;

	let Y = f32(0xDB) / 255.0 * yuv.x + f32(0x10) / 255.0;
	let Cr = f32(0xE0) / 255.0 * yuv.y + f32(0x80) / 255.0;
	let Cb = f32(0xE0) / 255.0 * yuv.z + f32(0x80) / 255.0;

	switch (cb.EMODA)
	{
		case 0: { o.a = i.a; }
		case 1: { o.a = Y; }
		case 2: { o.a = Y / 2.0; }
		case 3: { o.a = 0.0; }
		default: {}
	}

	switch (cb.EMODC)
	{
		case 0: { o = vec4f(i.rgb, o.a); }
		case 1: { o = vec4f(vec3f(Y), o.a); }
		case 2: { o = vec4f(Y, Cb, Cr, o.a); }
		case 3: { o = vec4f(vec3f(i.a), o.a); }
		default: {}
	}

	var out: PSOutput;
	out.c = o;
	return out;
}
#endif

#if defined(ps_primid_image_init_0) || defined(ps_primid_image_init_1) || defined(ps_primid_image_init_2) || defined(ps_primid_image_init_3)

#ifdef ps_primid_image_init_0
@fragment fn ps_primid_image_init_0(input: PSInput) -> PSOutput
#endif
#ifdef ps_primid_image_init_1
@fragment fn ps_primid_image_init_1(input: PSInput) -> PSOutput
#endif
#ifdef ps_primid_image_init_2
@fragment fn ps_primid_image_init_2(input: PSInput) -> PSOutput
#endif
#ifdef ps_primid_image_init_3
@fragment fn ps_primid_image_init_3(input: PSInput) -> PSOutput
#endif
{
	var o: PSOutput;
	o.c = vec4f(2147483648.0);

#ifdef ps_primid_image_init_0
	if ((127.5 / 255.0) < sample_c(input.tex).a)
	{
		o.c = vec4f(-1.0);
	}
#endif
#ifdef ps_primid_image_init_1
	if (sample_c(input.tex).a < (127.5 / 255.0))
	{
		o.c = vec4f(-1.0);
	}
#endif
#ifdef ps_primid_image_init_2
	if ((254.5 / 255.0) < sample_c(input.tex).a)
	{
		o.c = vec4f(-1.0);
	}
#endif
#ifdef ps_primid_image_init_3
	if (sample_c(input.tex).a < (254.5 / 255.0))
	{
		o.c = vec4f(-1.0);
	}
#endif
	return o;
}
#endif

#endif
