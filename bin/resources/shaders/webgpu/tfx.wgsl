// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifndef VS_EXPAND_NONE
#define VS_EXPAND_NONE 0
#define VS_EXPAND_POINT 1
#define VS_EXPAND_LINE 2
#define VS_EXPAND_SPRITE 3
#define VS_EXPAND_LINE_AA1 4
#define VS_EXPAND_TRIANGLE_AA1 5
#endif

#ifndef HAS_DUAL_SOURCE_BLEND
#define HAS_DUAL_SOURCE_BLEND 0
#endif

#ifndef HAS_PRIMITIVE_INDEX
#define HAS_PRIMITIVE_INDEX 0
#endif

#ifdef FRAGMENT_SHADER

#define FMT_32 0
#define FMT_24 1
#define FMT_16 2

#define SHUFFLE_READ  1
#define SHUFFLE_WRITE 2
#define SHUFFLE_READWRITE 3

#ifndef ZTST_GEQUAL
#define ZTST_GEQUAL 2
#define ZTST_GREATER 3
#endif

#ifndef AFAIL_KEEP
#define AFAIL_KEEP 0
#define AFAIL_FB_ONLY 1
#define AFAIL_ZB_ONLY 2
#define AFAIL_RGB_ONLY 3
#define AFAIL_RGB_ONLY_DSB 4
#define AFAIL_RGB_ONLY_SW_Z 5
#endif

#ifndef PS_ATST_NONE
#define PS_ATST_NONE 0
#define PS_ATST_LEQUAL 1
#define PS_ATST_GEQUAL 2
#define PS_ATST_EQUAL 3
#define PS_ATST_NOTEQUAL 4
#endif

#ifndef PS_AA1_NONE
#define PS_AA1_NONE 0
#define PS_AA1_LINE 1
#define PS_AA1_TRIANGLE 2
#define PS_AA1_TRIANGLE_SW_Z 3
#endif

#ifndef PS_FST
#define PS_FST 0
#define PS_WMS 0
#define PS_WMT 0
#define PS_ADJS 0
#define PS_ADJT 0
#define PS_AEM 0
#define PS_TFX 0
#define PS_TCC 1
#define PS_ATST 1
#define PS_AFAIL 0
#define PS_FOG 0
#define PS_BLEND_HW 0
#define PS_A_MASKED 0
#define PS_FBA 0
#define PS_FBMASK 0
#define PS_LTF 1
#define PS_TCOFFSETHACK 0
#define PS_SHUFFLE 0
#define PS_SHUFFLE_SAME 0
#define PS_PROCESS_BA 0
#define PS_PROCESS_RG 0
#define PS_SHUFFLE_ACROSS 0
#define PS_WRITE_RG 0
#define PS_READ16_SRC 0
#define PS_DST_FMT 0
#define PS_DEPTH_FMT 0
#define PS_PAL_FMT 0
#define PS_CHANNEL_FETCH 0
#define PS_TALES_OF_ABYSS_HLE 0
#define PS_URBAN_CHAOS_HLE 0
#define PS_COLCLIP_HW 0
#define PS_COLCLIP 0
#define PS_BLEND_A 0
#define PS_BLEND_B 0
#define PS_BLEND_C 0
#define PS_BLEND_D 0
#define PS_BLEND_MIX 0
#define PS_ROUND_INV 0
#define PS_FIXED_ONE_A 0
#define PS_PABE 0
#define PS_DITHER 0
#define PS_DITHER_ADJUST 0
#define PS_ZCLAMP 0
#define PS_ZFLOOR 0
#define PS_ZTST 0
#define PS_SCANMSK 0
#define PS_AUTOMATIC_LOD 0
#define PS_MANUAL_LOD 0
#define PS_ANISOTROPIC_FILTERING 0
#define PS_TEX_IS_FB 0
#define PS_NO_COLOR 0
#define PS_NO_COLOR1 0
#define PS_DATE 0
#define PS_IIP 0
#define PS_AA1 0
#define PS_ABE 0
#define PS_REGION_RECT 0
#define PS_RTA_CORRECTION 0
#define PS_RTA_SRC_CORRECTION 0
#endif

#define SW_BLEND (PS_BLEND_A || PS_BLEND_B || PS_BLEND_D)
#define SW_BLEND_NEEDS_RT (SW_BLEND && (PS_BLEND_A == 1 || PS_BLEND_B == 1 || PS_BLEND_C == 1 || PS_BLEND_D == 1))
#define SW_AD_TO_HW (PS_BLEND_C == 1 && PS_A_MASKED)
#define AFAIL_NEEDS_RT (PS_AFAIL == AFAIL_ZB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
#define AFAIL_NEEDS_DEPTH (PS_AFAIL == AFAIL_FB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
#define ZTST_NEEDS_DEPTH (PS_ZTST == ZTST_GEQUAL || PS_ZTST == ZTST_GREATER)
#define AA1_NEEDS_DEPTH (PS_AA1 == PS_AA1_TRIANGLE_SW_Z)

#define PS_FEEDBACK_LOOP_IS_NEEDED_RT (PS_TEX_IS_FB == 1 || AFAIL_NEEDS_RT || PS_FBMASK || SW_BLEND_NEEDS_RT || SW_AD_TO_HW || (PS_DATE >= 5))
#define PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH (AFAIL_NEEDS_DEPTH || ZTST_NEEDS_DEPTH || AA1_NEEDS_DEPTH)
#define ZWRITE (PS_ZCLAMP || PS_ZFLOOR || PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH)

#define PS_RETURN_COLOR (!PS_NO_COLOR)
#define PS_RETURN_COLOR1 (PS_RETURN_COLOR && !PS_NO_COLOR1 && HAS_DUAL_SOURCE_BLEND)
#define PS_RETURN_DEPTH (ZWRITE)
#define PS_HAS_OUTPUT (PS_RETURN_COLOR || PS_RETURN_DEPTH)
#define PS_USE_PRIMITIVE_ID ((PS_DATE == 1 || PS_DATE == 2 || PS_DATE == 3) && HAS_PRIMITIVE_INDEX)

#define NEEDS_TEX (PS_TFX != 4)

#if PS_RETURN_COLOR1
enable dual_source_blending;
#endif
#if PS_USE_PRIMITIVE_ID
enable primitive_index;
#endif

#endif // FRAGMENT_SHADER

//////////////////////////////////////////////////////////////////////
// Shared declarations
//////////////////////////////////////////////////////////////////////

struct VSConstants
{
	VertexScale: vec2f,
	VertexOffset: vec2f,
	TextureScale: vec2f,
	TextureOffset: vec2f,
	PointSize: vec2f,
	MaxDepth: u32,
	LineAA1Width: f32,
};

struct PSConstants
{
	FogColor_AREF: vec4f,
	WH: vec4f,
	TA_MaxDepth_Af: vec4f,
	FbMask: vec4u,
	HalfTexel: vec4f,
	MinMax: vec4f,
	LODParams: vec4f,
	STRange: vec4f,
	ChannelShuffle: vec4i,
	ChannelShuffleOffset: vec2f,
	TC_OffsetHack: vec2f,
	STScale: vec2f,
	DitherMatrix: mat4x4f,
	ScaleFactor: vec4f,
	LineCovScale: f32,
	_pad0: f32,
	_pad1: f32,
	_pad2: f32,
};

struct VSOutput
{
	@builtin(position) p: vec4f,
	@location(0) t: vec4f,
	@location(1) ti: vec4f,
#if (defined(VERTEX_SHADER) && VS_IIP != 0) || (defined(FRAGMENT_SHADER) && PS_IIP != 0)
	@location(2) c: vec4f,
#else
	@location(2) @interpolate(flat) c: vec4f,
#endif
	@location(3) inv_cov: f32,
	@location(4) @interpolate(flat) interior: u32,
};

//////////////////////////////////////////////////////////////////////
// Vertex Shader
//////////////////////////////////////////////////////////////////////

#ifdef VERTEX_SHADER

@group(0) @binding(0) var<uniform> cb0: VSConstants;

#if VS_EXPAND == VS_EXPAND_NONE

struct VSInput
{
	@location(0) st: vec2f,
	@location(1) c: vec4u,
	@location(2) q: f32,
	@location(3) p: vec2u,
	@location(4) z: u32,
	@location(5) uv: vec2u,
	@location(6) f: vec4f,
};

@vertex fn vs_main(input: VSInput) -> VSOutput
{
	var output: VSOutput;

	let z = min(input.z, cb0.MaxDepth);

	var p = vec4f(vec2f(input.p), f32(z), 1.0) - vec4f(0.05, 0.05, 0.0, 0.0);
	p = vec4f(p.xy * vec2f(cb0.VertexScale.x, -cb0.VertexScale.y) - vec2f(cb0.VertexOffset.x, -cb0.VertexOffset.y), p.z * exp2(-32.0), p.w);
	output.p = p;

#if VS_TME
	let uv = vec2f(input.uv) - cb0.TextureOffset;
	let st = input.st - cb0.TextureOffset;

	let ti_xy = uv * cb0.TextureScale;
#if VS_FST
	let ti_zw = uv;
#else
	let ti_zw = st / cb0.TextureScale;
#endif
	output.ti = vec4f(ti_xy, ti_zw);
	output.t = vec4f(st, input.f.r, input.q);
#else
	output.t = vec4f(0.0, 0.0, input.f.r, 1.0);
	output.ti = vec4f(0.0);
#endif

	output.c = vec4f(input.c);
	output.inv_cov = 0.0;
	output.interior = 0u;
	return output;
}

#else // VS_EXPAND

struct RawVertex
{
	ST: vec2f,
	RGBA: u32,
	Q: f32,
	XY: u32,
	Z: u32,
	UV: u32,
	FOG: u32,
};

struct VSPushConstants
{
	BaseVertex: u32,
	BaseIndex: u32,
	pad_cb2_0: u32,
	pad_cb2_1: u32,
};

@group(0) @binding(2) var<storage, read> vertex_buffer: array<RawVertex>;
@group(0) @binding(3) var<storage, read> index_buffer: array<u32>;
@group(0) @binding(4) var<uniform> cb2: VSPushConstants;

struct ProcessedVertex
{
	p: vec4f,
	t: vec4f,
	ti: vec4f,
	c: vec4f,
};

fn load_index(_i: u32) -> u32
{
	let i = _i + cb2.BaseIndex;
	let shift = (i & 1u) << 4u;
	return (index_buffer[i >> 1u] >> shift) & 0xFFFFu;
}

fn load_vertex(index: u32) -> ProcessedVertex
{
	let rvtx = vertex_buffer[cb2.BaseVertex + index];

	let a_st = rvtx.ST;
	let a_c = vec4u(extractBits(rvtx.RGBA, 0u, 8u), extractBits(rvtx.RGBA, 8u, 8u),
	                extractBits(rvtx.RGBA, 16u, 8u), extractBits(rvtx.RGBA, 24u, 8u));
	let a_q = rvtx.Q;
	let a_p = vec2u(extractBits(rvtx.XY, 0u, 16u), extractBits(rvtx.XY, 16u, 16u));
	let a_z = rvtx.Z;
	let a_uv = vec2u(extractBits(rvtx.UV, 0u, 16u), extractBits(rvtx.UV, 16u, 16u));
	let a_f = unpack4x8unorm(rvtx.FOG);

	var vtx: ProcessedVertex;

	let z = min(a_z, cb0.MaxDepth);
	var p = vec4f(vec2f(a_p), f32(z), 1.0) - vec4f(0.05, 0.05, 0.0, 0.0);
	p = vec4f(p.xy * vec2f(cb0.VertexScale.x, -cb0.VertexScale.y) - vec2f(cb0.VertexOffset.x, -cb0.VertexOffset.y), p.z * exp2(-32.0), p.w);
	vtx.p = p;

#if VS_TME
	let uv = vec2f(a_uv) - cb0.TextureOffset;
	let st = a_st - cb0.TextureOffset;
	let ti_xy = uv * cb0.TextureScale;
#if VS_FST
	let ti_zw = uv;
#else
	let ti_zw = st / cb0.TextureScale;
#endif
	vtx.ti = vec4f(ti_xy, ti_zw);
	vtx.t = vec4f(st, a_f.r, a_q);
#else
	vtx.t = vec4f(0.0, 0.0, a_f.r, 1.0);
	vtx.ti = vec4f(0.0);
#endif

	vtx.c = vec4f(a_c);
	return vtx;
}

fn get_xy_unscaled(xy: vec2f) -> vec2f
{
	return round(xy / cb0.VertexScale) / 16.0;
}

fn get_xy_deltas_unscaled(v0: ProcessedVertex, v1: ProcessedVertex, v2: ProcessedVertex) -> mat2x2f
{
	let xy0 = get_xy_unscaled(v0.p.xy);
	let xy1 = get_xy_unscaled(v1.p.xy);
	let xy2 = get_xy_unscaled(v2.p.xy);
	return mat2x2f(xy1 - xy0, xy2 - xy0);
}

fn get_aa1_triangle_expand_dir(v0: ProcessedVertex, v1: ProcessedVertex, v2: ProcessedVertex) -> vec2f
{
	let xy_deltas = get_xy_deltas_unscaled(v0, v1, v2);
	let line_delta = xy_deltas[0];
	let line_opposite = xy_deltas[1];

	let line_normal = vec2f(line_delta.y, -line_delta.x);
	var line_expand = select(vec2f(1.0, 0.0), vec2f(0.0, 1.0), abs(line_delta.x) >= abs(line_delta.y));

	if ((dot(line_expand, line_normal) >= 0.0) == (dot(line_opposite, line_normal) >= 0.0))
	{
		line_expand = -line_expand;
	}

	return line_expand;
}

fn get_inverse(m: mat2x2f, det: f32) -> mat2x2f
{
	return mat2x2f(vec2f(m[1][1], -m[0][1]), vec2f(-m[1][0], m[0][0])) * (1.0 / det);
}

fn extrapolate_aa1_triangle_edge(v0: ptr<function, ProcessedVertex>, v1: ProcessedVertex, v2: ProcessedVertex, dp_mat: mat2x2f, dp: vec2f)
{
#if VS_TME
#if VS_FST
	let dt = mat2x2f(v1.ti.zw - (*v0).ti.zw, v2.ti.zw - (*v0).ti.zw);
#else
	let dt = mat2x2f(v1.t.xy - (*v0).t.xy, v2.t.xy - (*v0).t.xy);
#endif
#endif

#if VS_IIP
	let dc = mat2x4f(v1.c - (*v0).c, v2.c - (*v0).c);
#endif

	let dz = vec2f(v1.p.z - (*v0).p.z, v2.p.z - (*v0).p.z);
	let df = vec2f(v1.t.z - (*v0).t.z, v2.t.z - (*v0).t.z);
	let dq = vec2f(v1.t.w - (*v0).t.w, v2.t.w - (*v0).t.w);

	let dp_det = determinant(dp_mat);
	let len0 = length(dp_mat[0]);
	let len1 = length(dp_mat[1]);
	let len2 = length(dp_mat[1] - dp_mat[0]);
	let min_perp_length = abs(dp_det) / max(max(len0, len1), len2);

	let inv_dp_mat = get_inverse(dp_mat, dp_det);

	let weights = select(inv_dp_mat * dp, vec2f(0.0), min_perp_length < 2.0);

	(*v0).p = vec4f((*v0).p.xy + dp * cb0.PointSize, (*v0).p.zw);

#if VS_TME
#if VS_FST
	let new_ti_zw = (*v0).ti.zw + dt * weights;
	(*v0).ti = vec4f(new_ti_zw * cb0.TextureScale, new_ti_zw);
#else
	let new_t_xy = (*v0).t.xy + dt * weights;
	(*v0).t = vec4f(new_t_xy, (*v0).t.z, (*v0).t.w + dot(dq, weights));
	(*v0).ti = vec4f((*v0).ti.xy, new_t_xy / cb0.TextureScale);
#endif
#endif

#if VS_IIP
	(*v0).c = clamp((*v0).c + dc * weights, vec4f(0.0), vec4f(255.0));
#endif

	(*v0).p.z += dot(dz, weights);
	(*v0).t.z += dot(df, weights);
}

@vertex fn vs_main(@builtin(vertex_index) vid: u32) -> VSOutput
{
	var vtx: ProcessedVertex;
	var output: VSOutput;
	output.inv_cov = 0.0;
	output.interior = 0u;

#if VS_EXPAND == VS_EXPAND_POINT

	vtx = load_vertex(vid >> 2u);

	vtx.p.x += select(0.0, cb0.PointSize.x, (vid & 1u) != 0u);
	vtx.p.y += select(0.0, cb0.PointSize.y, (vid & 2u) != 0u);

#elif (VS_EXPAND == VS_EXPAND_LINE) || (VS_EXPAND == VS_EXPAND_LINE_AA1)

	let vid_base = vid >> 2u;

	let is_bottom = (vid & 2u) != 0u;
	let is_right = (vid & 1u) != 0u;
	let vid_other = select(vid_base + 1u, vid_base - 1u, is_bottom);

	vtx = load_vertex(vid_base);
	let other = load_vertex(vid_other);

	let line_delta = select(other.p.xy - vtx.p.xy, vtx.p.xy - other.p.xy, is_bottom);
	let line_vector = normalize(line_delta / cb0.VertexScale);
	var line_expand = vec2f(line_vector.y, -line_vector.x);
#if VS_EXPAND == VS_EXPAND_LINE_AA1
	line_expand *= 2.0 * cb0.LineAA1Width;
#endif
	let line_width = (line_expand * cb0.PointSize) / 2.0;
	let offset = select(-line_width, line_width, is_right);
	vtx.p = vec4f(vtx.p.xy + offset, vtx.p.zw);

#if VS_EXPAND == VS_EXPAND_LINE_AA1
	output.inv_cov = select(-1.0, 1.0, is_right);
#endif

#elif VS_EXPAND == VS_EXPAND_SPRITE

	let vid_base = vid >> 1u;
	let vid_lt = vid_base & ~1u;
	let vid_rb = vid_base | 1u;

	let lt = load_vertex(vid_lt);
	let rb = load_vertex(vid_rb);
	vtx = rb;

	let is_right = ((vid & 1u) != 0u);
	vtx.p.x = select(vtx.p.x, lt.p.x, is_right);
	vtx.t.x = select(vtx.t.x, lt.t.x, is_right);
	vtx.ti.x = select(vtx.ti.x, lt.ti.x, is_right);
	vtx.ti.z = select(vtx.ti.z, lt.ti.z, is_right);

	let is_bottom = ((vid & 2u) != 0u);
	vtx.p.y = select(vtx.p.y, lt.p.y, is_bottom);
	vtx.t.y = select(vtx.t.y, lt.t.y, is_bottom);
	vtx.ti.y = select(vtx.ti.y, lt.ti.y, is_bottom);
	vtx.ti.w = select(vtx.ti.w, lt.ti.w, is_bottom);

#elif VS_EXPAND == VS_EXPAND_TRIANGLE_AA1

	let prim_id = vid / 39u;
	let prim_offset = vid - 39u * prim_id;
	let interior = prim_offset < 3u;
	let edge = 3u <= prim_offset && prim_offset < 21u;

	if (interior)
	{
		vtx = load_vertex(load_index(3u * prim_id + prim_offset));
		output.inv_cov = 0.0;
		output.interior = 1u;
	}
	else if (edge)
	{
		let prim_offset_edges = prim_offset - 3u;
		let i0 = prim_offset_edges / 6u;
		let i1 = select(i0 + 1u, i0 - 2u, i0 >= 2u);
		let i2 = select(i0 + 2u, i0 - 1u, i0 >= 1u);
		let edge_offset = prim_offset_edges - 6u * i0;

		let is_bottom = (2u <= edge_offset) && (edge_offset <= 4u);
		let is_outside = (edge_offset & 1u) != 0u;

		vtx = load_vertex(load_index(3u * prim_id + select(i0, i1, is_bottom)));
		let other = load_vertex(load_index(3u * prim_id + select(i1, i0, is_bottom)));
		let opposite = load_vertex(load_index(3u * prim_id + i2));

		let pos_deltas = get_xy_deltas_unscaled(vtx, other, opposite);

		let expand_dir = select(vec2f(0.0), get_aa1_triangle_expand_dir(vtx, other, opposite), is_outside);

		extrapolate_aa1_triangle_edge(&vtx, other, opposite, pos_deltas, expand_dir);

		output.inv_cov = select(0.0, 1.0, is_outside);
		output.interior = 0u;
	}
	else
	{
		let prim_offset_cap = prim_offset - 21u;
		let i0 = prim_offset_cap / 6u;
		let i1 = select(i0 + 1u, i0 - 2u, i0 >= 2u);
		let i2 = select(i0 + 2u, i0 - 1u, i0 >= 1u);
		let cap_offset = prim_offset_cap - 6u * i0;

		let is_near_corner = cap_offset == 0u || cap_offset == 3u;
		let is_far_corner = cap_offset == 2u || cap_offset == 5u;
		let is_first_tri = cap_offset < 3u;

		vtx = load_vertex(load_index(3u * prim_id + i0));
		let other = load_vertex(load_index(3u * prim_id + select(i2, i1, is_first_tri)));
		let opposite = load_vertex(load_index(3u * prim_id + select(i1, i2, is_first_tri)));

		let pos_deltas = get_xy_deltas_unscaled(vtx, other, opposite);

		let edge_expand_dir_0 = get_aa1_triangle_expand_dir(vtx, other, opposite);
		let edge_expand_dir_1 = get_aa1_triangle_expand_dir(vtx, opposite, other);

		let corner_filled = all(edge_expand_dir_0 == edge_expand_dir_1);

		let far_corner_dir = select(-normalize((pos_deltas[0] + pos_deltas[1]) / 2.0), vec2f(0.0), corner_filled);

		var expand_dir = edge_expand_dir_0;
		if (is_near_corner)
		{
			expand_dir = vec2f(0.0);
		}
		else if (is_far_corner)
		{
			expand_dir = far_corner_dir;
		}

		extrapolate_aa1_triangle_edge(&vtx, other, opposite, pos_deltas, expand_dir);

		output.inv_cov = select(1.0, 0.0, is_near_corner);
		output.interior = 0u;
	}

#endif

	output.p = vtx.p;
	output.t = vtx.t;
	output.ti = vtx.ti;
	output.c = vtx.c;
	return output;
}

#endif // VS_EXPAND

#endif // VERTEX_SHADER

//////////////////////////////////////////////////////////////////////
// Fragment Shader
//////////////////////////////////////////////////////////////////////

#ifdef FRAGMENT_SHADER

@group(0) @binding(1) var<uniform> cb1: PSConstants;

#if NEEDS_TEX
@group(1) @binding(0) var Texture: texture_2d<f32>;
@group(1) @binding(1) var TextureSampler: sampler;
@group(1) @binding(2) var Palette: texture_2d<f32>;
#endif

#if PS_FEEDBACK_LOOP_IS_NEEDED_RT
@group(1) @binding(3) var RtSampler: texture_2d<f32>;
#endif

#if PS_DATE > 0
@group(1) @binding(4) var PrimMinTexture: texture_2d<f32>;
#endif

#if PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH
@group(1) @binding(5) var DepthSampler: texture_2d<f32>;
#endif

struct PSInput
{
	@builtin(position) p: vec4f,
	@location(0) t: vec4f,
	@location(1) ti: vec4f,
#if PS_IIP != 0
	@location(2) c: vec4f,
#else
	@location(2) @interpolate(flat) c: vec4f,
#endif
	@location(3) inv_cov: f32,
	@location(4) @interpolate(flat) interior: u32,
#if PS_USE_PRIMITIVE_ID
	@builtin(primitive_index) primitive_id: u32,
#endif
};

#if PS_HAS_OUTPUT
struct PSOutput
{
#if PS_RETURN_COLOR1
	@location(0) @blend_src(0) c0: vec4f,
	@location(0) @blend_src(1) c1: vec4f,
#elif PS_RETURN_COLOR
	@location(0) c0: vec4f,
#endif
#if PS_RETURN_DEPTH
	@builtin(frag_depth) depth: f32,
#endif
};
#endif

#if PS_FEEDBACK_LOOP_IS_NEEDED_RT
fn sample_from_rt(pos: vec2f) -> vec4f
{
	return textureLoad(RtSampler, vec2i(pos), 0);
}
#endif

#if PS_FEEDBACK_LOOP_IS_NEEDED_DEPTH
fn sample_from_depth(pos: vec2f) -> f32
{
	return textureLoad(DepthSampler, vec2i(pos), 0).r;
}
#endif

fn is_nan_or_inf2(v: vec2f) -> bool
{
	let bits = bitcast<vec2u>(v) & vec2u(0x7FFFFFFFu);
	return any(bits >= vec2u(0x7F800000u));
}

#if NEEDS_TEX

#if (PS_AUTOMATIC_LOD != 1) && (PS_MANUAL_LOD == 1)
fn manual_lod(uv_w: f32) -> f32
{
	let K = cb1.LODParams.x;
	let L = cb1.LODParams.y;
	let bias = cb1.LODParams.z;
	let max_lod = cb1.LODParams.w;

	let gs_lod = K - log2(abs(uv_w)) * L;
	return min(gs_lod, max_lod) - bias;
}
#endif

#if PS_ANISOTROPIC_FILTERING > 1
fn sample_c_af(uv_in: vec2f, uv_w: f32) -> vec4f
{
	var uv = select(uv_in, vec2f(0.0, 0.0), is_nan_or_inf2(uv_in));
	uv = clamp(uv, vec2f(-8388608.0), vec2f(8388608.0));

	let sz = vec2f(textureDimensions(Texture, 0));
	var dX = dpdx(uv) * sz;
	var dY = dpdy(uv) * sz;

	var length_x = length(dX);
	var length_y = length(dY);

	let d_zero = length_x < 0.001 || length_y < 0.001;
	let f = (dX.x * dY.y - dX.y * dY.x);
	let d_par = f < 0.001;
	let d_per = dot(dX, dY) < 0.001;
	var d_inf_nan = is_nan_or_inf2(dX) || is_nan_or_inf2(dY);

	if (!(d_zero || d_par || d_per || d_inf_nan))
	{
		let A = dX.y * dX.y + dY.y * dY.y;
		let B = -2.0 * (dX.x * dX.y + dY.x * dY.y);
		let C = dX.x * dX.x + dY.x * dY.x;
		let f2 = (dX.x * dY.y - dY.x * dX.y);
		let F = f2 * f2;

		let p = A - C;
		let q = A + C;
		let t = sqrt(p * p + B * B);

		let signB = sign(B);
		let denom_plus = t * (q + t);
		let denom_minus = t * (q - t);

		let sqrtA = sqrt(F * (t + p));
		let sqrtB = sqrt(F * (t - p));

		let inv_sqrt_denom_plus = inverseSqrt(denom_plus);
		let inv_sqrt_denom_minus = inverseSqrt(denom_minus);

		let new_dX = vec2f(sqrtA * inv_sqrt_denom_plus, sqrtB * inv_sqrt_denom_plus * signB);
		let new_dY = vec2f(sqrtB * inv_sqrt_denom_minus * -signB, sqrtA * inv_sqrt_denom_minus);

		d_inf_nan = is_nan_or_inf2(new_dX) || is_nan_or_inf2(new_dY);
		if (!d_inf_nan)
		{
			dX = new_dX;
			dY = new_dY;
			length_x = length(dX);
			length_y = length(dY);
		}
	}

	let is_major_x = length_x > length_y;
	let length_major = select(length_y, length_x, is_major_x);
	let length_minor = select(length_x, length_y, is_major_x);

	var aniso_ratio: f32;
	var length_lod: f32;
	var aniso_line: vec2f;
	if (length_major <= 1.0)
	{
		aniso_ratio = 1.0;
		length_lod = length_major;
		aniso_line = vec2f(0.0, 0.0);
	}
	else
	{
		let aniso_line_dir = select(dY, dX, is_major_x);

		aniso_ratio = min(length_major / length_minor, f32(PS_ANISOTROPIC_FILTERING));
		length_lod = length_major / aniso_ratio;

		if (length_lod < 1.0)
		{
			aniso_ratio = max(1.0, aniso_ratio * length_lod);
		}

		aniso_ratio = round(aniso_ratio);

		aniso_line = aniso_line_dir * 0.5 * (1.0 / sz);
	}

#if PS_AUTOMATIC_LOD == 1
	let lod = log2(length_lod);
#elif PS_MANUAL_LOD == 1
	let lod = manual_lod(uv_w);
#else
	let lod = 0.0;
#endif

	var colour: vec4f;
	if (aniso_ratio == 1.0)
	{
		colour = textureSampleLevel(Texture, TextureSampler, uv, lod);
	}
	else
	{
		var num = vec4f(0.0, 0.0, 0.0, 0.0);
		let segment = (2.0 * aniso_line) / aniso_ratio;
		for (var i = 0.0; i < aniso_ratio; i += 1.0)
		{
			let d = -aniso_line + (0.5 + i) * segment;
			let uv_sample = uv + d;
			num += textureSampleLevel(Texture, TextureSampler, uv_sample, lod);
		}

		colour = num / aniso_ratio;
	}
	return colour;
}
#endif

fn sample_c(uv_in: vec2f, t: vec4f, pos: vec4f) -> vec4f
{
#if PS_TEX_IS_FB
	return sample_from_rt(pos.xy);
#elif PS_REGION_RECT
	return textureLoad(Texture, vec2i(uv_in), 0);
#else
	var uv = uv_in;

#if !PS_ADJS && !PS_ADJT
	uv *= cb1.STScale;
#else
#if PS_ADJS
	uv.x = (uv.x - cb1.STRange.x) * cb1.STRange.z;
#else
	uv.x = uv.x * cb1.STScale.x;
#endif
#if PS_ADJT
	uv.y = (uv.y - cb1.STRange.y) * cb1.STRange.w;
#else
	uv.y = uv.y * cb1.STScale.y;
#endif
#endif

#if PS_ANISOTROPIC_FILTERING > 1
	return sample_c_af(uv, t.w);
#elif PS_AUTOMATIC_LOD == 1
	return textureSample(Texture, TextureSampler, uv);
#elif PS_MANUAL_LOD == 1
	return textureSampleLevel(Texture, TextureSampler, uv, manual_lod(t.w));
#else
	return textureSampleLevel(Texture, TextureSampler, uv, 0.0);
#endif
#endif
}

fn sample_p(idx: u32) -> vec4f
{
	return textureLoad(Palette, vec2i(i32(idx), 0), 0);
}

fn sample_p_norm(u: f32) -> vec4f
{
	return sample_p(u32(u * 255.5));
}

fn clamp_wrap_uv(uv_in: vec4f) -> vec4f
{
	var uv = uv_in;
	let tex_size = cb1.WH.xyxy;

#if PS_WMS == PS_WMT
#if PS_REGION_RECT == 1 && PS_WMS == 0
	uv = fract(uv);
#elif PS_REGION_RECT == 1 && PS_WMS == 1
	uv = clamp(uv, vec4f(0.0), vec4f(1.0));
#elif PS_WMS == 2
	uv = clamp(uv, cb1.MinMax.xyxy, cb1.MinMax.zwzw);
#elif PS_WMS == 3
#if PS_FST == 0
	uv = fract(uv);
#endif
	uv = vec4f((vec4u(uv * tex_size) & bitcast<vec4u>(cb1.MinMax.xyxy)) | bitcast<vec4u>(cb1.MinMax.zwzw)) / tex_size;
#endif
#else
#if PS_REGION_RECT == 1 && PS_WMS == 0
	uv = vec4f(fract(uv.x), uv.y, fract(uv.z), uv.w);
#elif PS_REGION_RECT == 1 && PS_WMS == 1
	uv = vec4f(clamp(uv.x, 0.0, 1.0), uv.y, clamp(uv.z, 0.0, 1.0), uv.w);
#elif PS_WMS == 2
	uv = vec4f(clamp(uv.x, cb1.MinMax.x, cb1.MinMax.z), uv.y, clamp(uv.z, cb1.MinMax.x, cb1.MinMax.z), uv.w);
#elif PS_WMS == 3
#if PS_FST == 0
	uv = vec4f(fract(uv.x), uv.y, fract(uv.z), uv.w);
#endif
	{
		let xz = vec2f((vec2u(uv.xz * tex_size.xx) & bitcast<vec2u>(cb1.MinMax.xx)) | bitcast<vec2u>(cb1.MinMax.zz)) / tex_size.xx;
		uv = vec4f(xz.x, uv.y, xz.y, uv.w);
	}
#endif
#if PS_REGION_RECT == 1 && PS_WMT == 0
	uv = vec4f(uv.x, fract(uv.y), uv.z, fract(uv.w));
#elif PS_REGION_RECT == 1 && PS_WMT == 1
	uv = vec4f(uv.x, clamp(uv.y, 0.0, 1.0), uv.z, clamp(uv.w, 0.0, 1.0));
#elif PS_WMT == 2
	uv = vec4f(uv.x, clamp(uv.y, cb1.MinMax.y, cb1.MinMax.w), uv.z, clamp(uv.w, cb1.MinMax.y, cb1.MinMax.w));
#elif PS_WMT == 3
#if PS_FST == 0
	uv = vec4f(uv.x, fract(uv.y), uv.z, fract(uv.w));
#endif
	{
		let yw = vec2f((vec2u(uv.yw * tex_size.yy) & bitcast<vec2u>(cb1.MinMax.yy)) | bitcast<vec2u>(cb1.MinMax.ww)) / tex_size.yy;
		uv = vec4f(uv.x, yw.x, uv.z, yw.y);
	}
#endif
#endif

#if PS_REGION_RECT == 1
	uv = clamp(uv * cb1.WH.zwzw + cb1.STRange.xyxy, cb1.STRange.xyxy, cb1.STRange.zwzw);
#endif

	return uv;
}

fn sample_4c(uv: vec4f, t: vec4f, pos: vec4f) -> array<vec4f, 4>
{
	var c: array<vec4f, 4>;
	c[0] = sample_c(uv.xy, t, pos);
	c[1] = sample_c(uv.zy, t, pos);
	c[2] = sample_c(uv.xw, t, pos);
	c[3] = sample_c(uv.zw, t, pos);
	return c;
}

fn sample_4_index(uv: vec4f, t: vec4f, pos: vec4f) -> vec4u
{
	var c: vec4f;

#if PS_PAL_FMT == 3
	c.x = sample_c(uv.xy, t, pos).r;
	c.y = sample_c(uv.zy, t, pos).r;
	c.z = sample_c(uv.xw, t, pos).r;
	c.w = sample_c(uv.zw, t, pos).r;
#else
	c.x = sample_c(uv.xy, t, pos).a;
	c.y = sample_c(uv.zy, t, pos).a;
	c.z = sample_c(uv.xw, t, pos).a;
	c.w = sample_c(uv.zw, t, pos).a;
#endif

#if PS_RTA_SRC_CORRECTION
	let i = vec4u(round(c * 128.25));
#else
	let i = vec4u(c * 255.5);
#endif

#if PS_PAL_FMT == 1
	return i & vec4u(0xFu);
#elif PS_PAL_FMT == 2
	return i >> vec4u(4u);
#else
	return i;
#endif
}

fn sample_4p(u: vec4u) -> array<vec4f, 4>
{
	var c: array<vec4f, 4>;
	c[0] = sample_p(u.x);
	c[1] = sample_p(u.y);
	c[2] = sample_p(u.z);
	c[3] = sample_p(u.w);
	return c;
}

fn fetch_raw_depth(xy: vec2i, pos: vec4f) -> u32
{
#if PS_TEX_IS_FB
	let col = sample_from_rt(pos.xy);
#else
	let col = textureLoad(Texture, xy, 0);
#endif
	return u32(col.r * exp2(32.0));
}

fn fetch_raw_color(xy: vec2i, pos: vec4f) -> vec4f
{
#if PS_TEX_IS_FB
	return sample_from_rt(pos.xy);
#else
	return textureLoad(Texture, xy, 0);
#endif
}

fn fetch_c(uv: vec2i, pos: vec4f) -> vec4f
{
#if PS_TEX_IS_FB
	return sample_from_rt(pos.xy);
#else
	return textureLoad(Texture, uv, 0);
#endif
}

//////////////////////////////////////////////////////////////////////
// Depth sampling
//////////////////////////////////////////////////////////////////////

fn clamp_wrap_uv_depth(uv_in: vec2i) -> vec2i
{
	var uv = uv_in;
	let mask = bitcast<vec4i>(cb1.MinMax) << vec4u(4u);
#if (PS_WMS == PS_WMT)
#if (PS_WMS == 2)
	uv = clamp(uv, mask.xy, mask.zw);
#elif (PS_WMS == 3)
	uv = (uv & mask.xy) | mask.zw;
#endif
#else
#if (PS_WMS == 2)
	uv.x = clamp(uv.x, mask.x, mask.z);
#elif (PS_WMS == 3)
	uv.x = (uv.x & mask.x) | mask.z;
#endif
#if (PS_WMT == 2)
	uv.y = clamp(uv.y, mask.y, mask.w);
#elif (PS_WMT == 3)
	uv.y = (uv.y & mask.y) | mask.w;
#endif
#endif
	return uv;
}

fn sample_depth(st: vec2f, pos: vec4f) -> vec4f
{
	var uv_f = vec2f(clamp_wrap_uv_depth(vec2i(st))) * vec2f(cb1.ScaleFactor.x);

#if PS_REGION_RECT == 1
	uv_f = clamp(uv_f + cb1.STRange.xy, cb1.STRange.xy, cb1.STRange.zw);
#endif

	let uv = vec2i(uv_f);
	var t = vec4f(0.0);

#if (PS_TALES_OF_ABYSS_HLE == 1)
	{
		let depth = fetch_raw_depth(vec2i(pos.xy), pos);
		t = textureLoad(Palette, vec2i(i32((depth >> 8u) & 0xFFu), 0), 0) * 255.0;
	}
#elif (PS_URBAN_CHAOS_HLE == 1)
	{
		let depth = fetch_raw_depth(vec2i(pos.xy), pos);
		t = textureLoad(Palette, vec2i(i32(depth & 0xFFu), 0), 0) * 255.0;
		var green = f32((depth >> 8u) & 0xFFu) * 36.0;
		green = min(green, 255.0);
		t.g += green;
	}
#elif (PS_DEPTH_FMT == 1)
	{
		let d = u32(fetch_c(uv, pos).r * exp2(32.0));
		t = vec4f(vec4u((d & 0xFFu), ((d >> 8u) & 0xFFu), ((d >> 16u) & 0xFFu), (d >> 24u)));
	}
#elif (PS_DEPTH_FMT == 2)
	{
		let d = u32(fetch_c(uv, pos).r * exp2(32.0));
		t = vec4f(vec4u((d & 0x1Fu), ((d >> 5u) & 0x1Fu), ((d >> 10u) & 0x1Fu), (d >> 15u) & 0x01u)) * vec4f(8.0, 8.0, 8.0, 128.0);
	}
#elif (PS_DEPTH_FMT == 3)
	{
		t = fetch_c(uv, pos) * 255.0;
	}
#endif

#if (PS_AEM_FMT == FMT_24)
	t.a = select(0.0, 255.0 * cb1.TA_MaxDepth_Af.x, (PS_AEM == 0) || any(t.rgb != vec3f(0.0)));
#elif (PS_AEM_FMT == FMT_16)
	t.a = select(select(0.0, 255.0 * cb1.TA_MaxDepth_Af.x, (PS_AEM == 0) || any(t.rgb != vec3f(0.0))), 255.0 * cb1.TA_MaxDepth_Af.y, t.a >= 128.0);
#elif PS_PAL_FMT != 0 && !PS_TALES_OF_ABYSS_HLE && !PS_URBAN_CHAOS_HLE
	t = trunc(sample_4p(vec4u(t.aaaa))[0] * 255.0 + 0.05);
#endif

	return t;
}

//////////////////////////////////////////////////////////////////////
// Fetch a Single Channel
//////////////////////////////////////////////////////////////////////

fn fetch_red(xy: vec2i, pos: vec4f) -> vec4f
{
#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
	let depth = (fetch_raw_depth(xy, pos)) & 0xFFu;
	let rt = vec4f(f32(depth) / 255.0);
#else
	let rt = fetch_raw_color(xy, pos);
#endif
	return sample_p_norm(rt.r) * 255.0;
}

fn fetch_green(xy: vec2i, pos: vec4f) -> vec4f
{
#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
	let depth = (fetch_raw_depth(xy, pos) >> 8u) & 0xFFu;
	let rt = vec4f(f32(depth) / 255.0);
#else
	let rt = fetch_raw_color(xy, pos);
#endif
	return sample_p_norm(rt.g) * 255.0;
}

fn fetch_blue(xy: vec2i, pos: vec4f) -> vec4f
{
#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
	let depth = (fetch_raw_depth(xy, pos) >> 16u) & 0xFFu;
	let rt = vec4f(f32(depth) / 255.0);
#else
	let rt = fetch_raw_color(xy, pos);
#endif
	return sample_p_norm(rt.b) * 255.0;
}

fn fetch_alpha(xy: vec2i, pos: vec4f) -> vec4f
{
	let rt = fetch_raw_color(xy, pos);
	return sample_p_norm(rt.a) * 255.0;
}

fn fetch_rgb(xy: vec2i, pos: vec4f) -> vec4f
{
	let rt = fetch_raw_color(xy, pos);
	let c = vec4f(sample_p_norm(rt.r).r, sample_p_norm(rt.g).g, sample_p_norm(rt.b).b, 1.0);
	return c * 255.0;
}

fn fetch_gXbY(xy: vec2i, pos: vec4f) -> vec4f
{
#if (PS_DEPTH_FMT == 1) || (PS_DEPTH_FMT == 2)
	let depth = fetch_raw_depth(xy, pos);
	let bg = (depth >> (8u + u32(cb1.ChannelShuffle.w))) & 0xFFu;
	return vec4f(f32(bg));
#else
	let rt = vec4i(fetch_raw_color(xy, pos) * 255.0);
	let green = (rt.g >> u32(cb1.ChannelShuffle.w)) & cb1.ChannelShuffle.z;
	let blue = (rt.b << u32(cb1.ChannelShuffle.y)) & cb1.ChannelShuffle.x;
	return vec4f(f32(green | blue));
#endif
}

fn sample_color(st_in: vec2f, t_in: vec4f, pos: vec4f) -> vec4f
{
	var st = st_in;
#if PS_TCOFFSETHACK
	st += cb1.TC_OffsetHack.xy;
#endif

	var t: vec4f;
	var c: array<vec4f, 4>;
	var dd = vec2f(0.0);

#if PS_LTF == 0 && PS_AEM_FMT == FMT_32 && PS_PAL_FMT == 0 && PS_REGION_RECT == 0 && PS_WMS < 2 && PS_WMT < 2
	c[0] = sample_c(st, t_in, pos);
#else
	var uv: vec4f;

#if PS_LTF
	uv = st.xyxy + cb1.HalfTexel;
	dd = fract(uv.xy * cb1.WH.zw);
#if PS_FST == 0
	dd = clamp(dd, vec2f(0.0), vec2f(0.9999999));
#endif
#else
	uv = st.xyxy;
#endif

	uv = clamp_wrap_uv(uv);

#if PS_PAL_FMT != 0
	c = sample_4p(sample_4_index(uv, t_in, pos));
#else
	c = sample_4c(uv, t_in, pos);
#endif
#endif

	for (var i = 0u; i < 4u; i++)
	{
#if (PS_AEM_FMT == FMT_24)
		c[i].a = select(0.0, cb1.TA_MaxDepth_Af.x, PS_AEM == 0 || any(c[i].rgb != vec3f(0.0)));
#elif (PS_AEM_FMT == FMT_16)
		c[i].a = select(select(0.0, cb1.TA_MaxDepth_Af.x, PS_AEM == 0 || any((vec3i(c[i].rgb * 255.0) & vec3i(0xF8)) != vec3i(0))), cb1.TA_MaxDepth_Af.y, c[i].a >= 0.5);
#endif
	}

#if PS_LTF
	t = mix(mix(c[0], c[1], dd.x), mix(c[2], c[3], dd.x), dd.y);
#else
	t = c[0];
#endif
#if PS_AEM_FMT == FMT_32 && PS_PAL_FMT == 0 && PS_RTA_SRC_CORRECTION
	t.a = t.a * (128.5 / 255.0);
#endif
	return trunc(t * 255.0 + 0.05);
}

#endif // NEEDS_TEX

fn tfx(T: vec4f, C: vec4f) -> vec4f
{
	var C_out: vec4f;
	let FxT = trunc((C * T) / 128.0);

#if (PS_TFX == 0)
	C_out = FxT;
#elif (PS_TFX == 1)
	C_out = T;
#elif (PS_TFX == 2)
	C_out = vec4f(FxT.rgb + C.a, T.a + C.a);
#elif (PS_TFX == 3)
	C_out = vec4f(FxT.rgb + C.a, T.a);
#else
	C_out = C;
#endif

#if (PS_TCC == 0)
	C_out.a = C.a;
#endif

#if (PS_TFX == 0) || (PS_TFX == 2) || (PS_TFX == 3)
	C_out = min(C_out, vec4f(255.0));
#endif

	return C_out;
}

fn atst(C: vec4f) -> bool
{
	let a = C.a;

#if PS_ATST == PS_ATST_LEQUAL
	return (a <= cb1.FogColor_AREF.a);
#elif PS_ATST == PS_ATST_GEQUAL
	return (a >= cb1.FogColor_AREF.a);
#elif PS_ATST == PS_ATST_EQUAL
	return (abs(a - cb1.FogColor_AREF.a) <= 0.5);
#elif PS_ATST == PS_ATST_NOTEQUAL
	return (abs(a - cb1.FogColor_AREF.a) >= 0.5);
#else
	return true;
#endif
}

fn fog(c: vec4f, f: f32) -> vec4f
{
#if PS_FOG
	return vec4f(trunc(mix(cb1.FogColor_AREF.rgb, c.rgb, (f * 255.0) / 256.0)), c.a);
#else
	return c;
#endif
}

fn ps_color(input: PSInput) -> vec4f
{
#if PS_FST == 0
	let st = input.t.xy / input.t.w;
	let st_int = input.ti.zw / input.t.w;
#else
	let st = input.ti.xy;
	let st_int = input.ti.zw;
#endif

#if !NEEDS_TEX
	var T = vec4f(0.0);
#elif PS_CHANNEL_FETCH == 1
	var T = fetch_red(vec2i(input.p.xy + cb1.ChannelShuffleOffset), input.p);
#elif PS_CHANNEL_FETCH == 2
	var T = fetch_green(vec2i(input.p.xy + cb1.ChannelShuffleOffset), input.p);
#elif PS_CHANNEL_FETCH == 3
	var T = fetch_blue(vec2i(input.p.xy + cb1.ChannelShuffleOffset), input.p);
#elif PS_CHANNEL_FETCH == 4
	var T = fetch_alpha(vec2i(input.p.xy + cb1.ChannelShuffleOffset), input.p);
#elif PS_CHANNEL_FETCH == 5
	var T = fetch_rgb(vec2i(input.p.xy + cb1.ChannelShuffleOffset), input.p);
#elif PS_CHANNEL_FETCH == 6
	var T = fetch_gXbY(vec2i(input.p.xy + cb1.ChannelShuffleOffset), input.p);
#elif PS_DEPTH_FMT > 0
	var T = sample_depth(st_int, input.p);
#else
	var T = sample_color(st, input.t, input.p);
#endif

#if PS_SHUFFLE && !PS_READ16_SRC && !PS_SHUFFLE_SAME && !(PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE)
	let denorm_c_before = vec4u(T);
#if (PS_PROCESS_BA & SHUFFLE_READ)
	T.r = f32((denorm_c_before.b << 3u) & 0xF8u);
	T.g = f32(((denorm_c_before.b >> 2u) & 0x38u) | ((denorm_c_before.a << 6u) & 0xC0u));
	T.b = f32((denorm_c_before.a << 1u) & 0xF8u);
	T.a = f32(denorm_c_before.a & 0x80u);
#else
	T.r = f32((denorm_c_before.r << 3u) & 0xF8u);
	T.g = f32(((denorm_c_before.r >> 2u) & 0x38u) | ((denorm_c_before.g << 6u) & 0xC0u));
	T.b = f32((denorm_c_before.g << 1u) & 0xF8u);
	T.a = f32(denorm_c_before.g & 0x80u);
#endif

	T.a = select(select(0.0, cb1.TA_MaxDepth_Af.x, PS_AEM == 0 || any((vec3i(T.rgb) & vec3i(0xF8)) != vec3i(0))), cb1.TA_MaxDepth_Af.y, T.a >= 127.5) * 255.0;
#endif

	var C = tfx(T, input.c);

	C = fog(C, input.t.z);

	return C;
}

fn ps_fbmask(C: ptr<function, vec4f>, pos: vec4f)
{
#if PS_FBMASK
#if PS_COLCLIP_HW == 1
	let RT = trunc(sample_from_rt(pos.xy) * 65535.0);
#else
	let RT = trunc(sample_from_rt(pos.xy) * 255.0 + 0.1);
#endif
	*C = vec4f((vec4u(*C) & ~cb1.FbMask) | (vec4u(RT) & cb1.FbMask));
#endif
}

fn ps_dither(C: ptr<function, vec3f>, As: f32, pos: vec4f)
{
#if PS_DITHER > 0 && PS_DITHER < 3
#if PS_DITHER == 2
	let fpos = vec2i(pos.xy);
#else
	let fpos = vec2i(pos.xy * cb1.ScaleFactor.y);
#endif

	var value = cb1.DitherMatrix[fpos.y & 3][fpos.x & 3];

#if PS_DITHER_ADJUST
#if PS_BLEND_C == 2
	let Alpha = cb1.TA_MaxDepth_Af.w;
#else
	let Alpha = As;
#endif

	value *= select(1.0, min(1.0 / Alpha, 1.0), Alpha > 0.0);
#endif

#if PS_ROUND_INV
	*C -= value;
#else
	*C += value;
#endif
#endif
}

fn ps_color_clamp_wrap(C: ptr<function, vec3f>)
{
#if SW_BLEND || (PS_DITHER > 0 && PS_DITHER < 3) || PS_FBMASK

#if PS_DST_FMT == FMT_16 && PS_BLEND_MIX == 0 && PS_ROUND_INV
	*C += 7.0;
#endif

#if PS_COLCLIP == 0 && PS_COLCLIP_HW == 0
	*C = clamp(*C, vec3f(0.0), vec3f(255.0));
#endif

#if PS_DST_FMT == FMT_16 && PS_DITHER != 3 && (PS_BLEND_MIX == 0 || PS_DITHER > 0)
	*C = vec3f(vec3i(*C) & vec3i(0xF8));
#elif PS_COLCLIP == 1 || PS_COLCLIP_HW == 1
	*C = vec3f(vec3i(*C) & vec3i(0xFF));
#endif

#elif PS_DST_FMT == FMT_16 && PS_DITHER != 3 && PS_BLEND_MIX == 0 && PS_BLEND_HW == 0
	*C = vec3f(vec3i(*C) & vec3i(0xF8));
#endif
}

fn ps_blend(Color: ptr<function, vec4f>, As_rgba: ptr<function, vec4f>, pos: vec4f)
{
	let As = (*As_rgba).a;

#if SW_BLEND

#if PS_PABE
	if (As < 1.0)
	{
		*As_rgba = vec4f(0.0, 0.0, 0.0, (*As_rgba).a);
		return;
	}

	*As_rgba = vec4f(1.0, 1.0, 1.0, (*As_rgba).a);
#endif

#if PS_FEEDBACK_LOOP_IS_NEEDED_RT
	var RT = sample_from_rt(pos.xy);
#else
	var RT = vec4f(0.0);
#endif

#if PS_RTA_CORRECTION
	let Ad = trunc(RT.a * 128.0 + 0.1) / 128.0;
#else
	let Ad = trunc(RT.a * 255.0 + 0.1) / 128.0;
#endif

#if PS_SHUFFLE && PS_FEEDBACK_LOOP_IS_NEEDED_RT
	let denorm_rt = vec4u(RT);
#if (PS_PROCESS_BA & SHUFFLE_WRITE)
	RT.r = f32((denorm_rt.b << 3u) & 0xF8u);
	RT.g = f32(((denorm_rt.b >> 2u) & 0x38u) | ((denorm_rt.a << 6u) & 0xC0u));
	RT.b = f32((denorm_rt.a << 1u) & 0xF8u);
	RT.a = f32(denorm_rt.a & 0x80u);
#else
	RT.r = f32((denorm_rt.r << 3u) & 0xF8u);
	RT.g = f32(((denorm_rt.r >> 2u) & 0x38u) | ((denorm_rt.g << 6u) & 0xC0u));
	RT.b = f32((denorm_rt.g << 1u) & 0xF8u);
	RT.a = f32(denorm_rt.g & 0x80u);
#endif
#endif

#if PS_COLCLIP_HW == 1
	let Cd = trunc(RT.rgb * 65535.0);
#else
	let Cd = trunc(RT.rgb * 255.0 + 0.1);
#endif
	let Cs = (*Color).rgb;

#if PS_BLEND_A == 0
	let A = Cs;
#elif PS_BLEND_A == 1
	let A = Cd;
#else
	let A = vec3f(0.0);
#endif

#if PS_BLEND_B == 0
	let B = Cs;
#elif PS_BLEND_B == 1
	let B = Cd;
#else
	let B = vec3f(0.0);
#endif

#if PS_BLEND_C == 0
	let C = As;
#elif PS_BLEND_C == 1
	let C = Ad;
#else
	let C = cb1.TA_MaxDepth_Af.w;
#endif

#if PS_BLEND_D == 0
	let D = Cs;
#elif PS_BLEND_D == 1
	let D = Cd;
#else
	let D = vec3f(0.0);
#endif

	var C_clamped = C;
#if PS_BLEND_MIX > 0 && PS_BLEND_HW != 1 && PS_BLEND_HW != 2
	C_clamped = min(C_clamped, 1.0);
#endif

#if PS_BLEND_A == PS_BLEND_B
	var rgb = D;
#elif PS_BLEND_MIX == 2
	var rgb = ((A - B) * C_clamped + D) + (124.0 / 256.0);
#elif PS_BLEND_MIX == 1
	var rgb = ((A - B) * C_clamped + D) - (124.0 / 256.0);
#else
	var rgb = trunc((A - B) * C + D);
#endif

#if PS_BLEND_HW == 1
	{
		let alpha_compensate = max(vec3f(1.0), rgb / vec3f(255.0));
		*As_rgba = vec4f(vec3f(C) - alpha_compensate, (*As_rgba).a);
	}
#elif PS_BLEND_HW == 2
	{
		let division_alpha = 1.0 + C;
		rgb /= vec3f(division_alpha);
	}
#elif PS_BLEND_HW == 3
	{
		let overflow_check = (rgb - vec3f(255.0)) / 255.0;
		let alpha_compensate = max(vec3f(0.0), overflow_check);
		*As_rgba = vec4f(vec3f(C_clamped) - alpha_compensate, (*As_rgba).a);
	}
#endif

	*Color = vec4f(rgb, (*Color).a);

#else

#if PS_BLEND_C == 2
	var Alpha = vec3f(cb1.TA_MaxDepth_Af.w);
#else
	var Alpha = vec3f(As);
#endif

#if PS_BLEND_HW == 1
	*Color = vec4f(255.0, 255.0, 255.0, (*Color).a);
#elif PS_BLEND_HW == 2
	*Color = vec4f(max(vec3f(0.0), (Alpha - vec3f(1.0))) * vec3f(255.0), (*Color).a);
#elif PS_BLEND_HW == 3 && PS_RTA_CORRECTION == 0
	{
		let max_color = max(max((*Color).r, (*Color).g), (*Color).b);
		let color_compensate = 255.0 / max(128.0, max_color);
		*Color = vec4f((*Color).rgb * vec3f(color_compensate), (*Color).a);
	}
#elif PS_BLEND_HW == 4
	*As_rgba = vec4f(Alpha * vec3f(128.0 / 255.0), (*As_rgba).a);
	*Color = vec4f(127.5, 127.5, 127.5, (*Color).a);
#elif PS_BLEND_HW == 5
	Alpha *= vec3f(128.0 / 255.0);
	*As_rgba = vec4f(Alpha - vec3f(0.5), (*As_rgba).a);
	*Color = vec4f((*Color).rgb * Alpha, (*Color).a);
#elif PS_BLEND_HW == 6
	Alpha *= vec3f(128.0 / 255.0);
	*As_rgba = vec4f(Alpha, (*As_rgba).a);
	*Color = vec4f((*Color).rgb * (Alpha - vec3f(0.5)), (*Color).a);
#endif
#endif
}

#if PS_HAS_OUTPUT
@fragment fn ps_main(input: PSInput) -> PSOutput
{
	var output: PSOutput;
#else
@fragment fn ps_main(input: PSInput)
{
#endif
	var input_z = input.p.z;

#if PS_ZFLOOR
	input_z = floor(input_z * exp2(32.0)) * exp2(-32.0);
#endif

#if PS_ZTST == ZTST_GEQUAL
	if (input_z < sample_from_depth(input.p.xy))
	{
		discard;
	}
#elif PS_ZTST == ZTST_GREATER
	if (input_z <= sample_from_depth(input.p.xy))
	{
		discard;
	}
#endif

#if PS_SCANMSK & 2
	if ((i32(input.p.y) & 1) == (PS_SCANMSK & 1))
	{
		discard;
	}
#endif

#if PS_DATE >= 5

#if PS_WRITE_RG == 1
	let rt_a = sample_from_rt(input.p.xy).g;
#else
	let rt_a = sample_from_rt(input.p.xy).a;
#endif

#if (PS_DATE & 3) == 1
#if PS_RTA_CORRECTION
	let bad = (254.5 / 255.0) < rt_a;
#else
	let bad = (127.5 / 255.0) < rt_a;
#endif
#elif (PS_DATE & 3) == 2
#if PS_RTA_CORRECTION
	let bad = rt_a < (254.5 / 255.0);
#else
	let bad = rt_a < (127.5 / 255.0);
#endif
#else
	let bad = false;
#endif

	if (bad)
	{
		discard;
	}

#endif

#if PS_DATE == 3 && PS_USE_PRIMITIVE_ID
	{
		let stencil_ceil = i32(textureLoad(PrimMinTexture, vec2i(input.p.xy), 0).r);
		if (i32(input.primitive_id) > stencil_ceil)
		{
			discard;
		}
	}
#endif

	var C = ps_color(input);

#if PS_AA1
#if PS_AA1 == PS_AA1_LINE
	let cov = clamp(cb1.LineCovScale * (1.0 - abs(input.inv_cov)), 0.0, 1.0);
#else
	let cov = clamp(1.0 - abs(input.inv_cov), 0.0, 1.0);
#endif
#if PS_ABE
	if (floor(C.a) == 128.0)
	{
		C.a = 128.0 * cov;
	}
#else
	C.a = 128.0 * cov;
#endif
#elif PS_FIXED_ONE_A
	C.a = 128.0;
#endif

	let atst_pass = atst(C);

#if PS_ATST != PS_ATST_NONE && PS_AFAIL == AFAIL_KEEP
	if (!atst_pass)
	{
		discard;
	}
#endif

#if SW_AD_TO_HW
#if PS_RTA_CORRECTION
	let RT = trunc(sample_from_rt(input.p.xy) * 128.0 + 0.1);
#else
	let RT = trunc(sample_from_rt(input.p.xy) * 255.0 + 0.1);
#endif
	var alpha_blend = vec4f(RT.a / 128.0);
#else
	var alpha_blend = vec4f(C.a / 128.0);
#endif

#if (PS_DST_FMT == FMT_16)
	let A_one = 128.0;
	C.a = select(step(128.0, C.a) * A_one, A_one, PS_FBA != 0);
#elif (PS_DST_FMT == FMT_32) && (PS_FBA != 0)
	if (C.a < 128.0)
	{
		C.a += 128.0;
	}
#endif

#if PS_DATE == 1 || PS_DATE == 2
#if PS_RETURN_COLOR
#if PS_DATE == 1 && PS_USE_PRIMITIVE_ID
	output.c0 = select(vec4f(2147483648.0), vec4f(f32(input.primitive_id)), C.a > 127.5);
#elif PS_DATE == 2 && PS_USE_PRIMITIVE_ID
	output.c0 = select(vec4f(2147483648.0), vec4f(f32(input.primitive_id)), C.a < 127.5);
#else
	output.c0 = vec4f(2147483648.0);
#endif
#endif
#else
	ps_blend(&C, &alpha_blend, input.p);

#if PS_SHUFFLE
#if !PS_READ16_SRC && !PS_SHUFFLE_SAME && !(PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE)
	{
		let denorm_c_after = vec4u(C);
#if (PS_PROCESS_BA & SHUFFLE_READ)
		C.b = f32(((denorm_c_after.r >> 3u) & 0x1Fu) | ((denorm_c_after.g << 2u) & 0xE0u));
		C.a = f32(((denorm_c_after.g >> 6u) & 0x3u) | ((denorm_c_after.b >> 1u) & 0x7Cu) | (denorm_c_after.a & 0x80u));
#else
		C.r = f32(((denorm_c_after.r >> 3u) & 0x1Fu) | ((denorm_c_after.g << 2u) & 0xE0u));
		C.g = f32(((denorm_c_after.g >> 6u) & 0x3u) | ((denorm_c_after.b >> 1u) & 0x7Cu) | (denorm_c_after.a & 0x80u));
#endif
	}
#endif

#if PS_SHUFFLE_SAME
#if (PS_PROCESS_BA & SHUFFLE_READ)
	{
		let denorm_c = vec4u(C);
		C = vec4f(f32((denorm_c.b & 0x7Fu) | (denorm_c.a & 0x80u)));
	}
#else
	C = vec4f(C.r, C.g, C.r, C.g);
#endif
#elif PS_READ16_SRC
	{
		let denorm_c = vec4u(C);
		let denorm_TA = vec2u(vec2f(cb1.TA_MaxDepth_Af.xy) * 255.0 + 0.5);
		let rb = f32((denorm_c.r >> 3u) | (((denorm_c.g >> 3u) & 0x7u) << 5u));
		let ga = f32((denorm_c.g >> 6u) | ((denorm_c.b >> 3u) << 2u) | (denorm_TA.x & 0x80u));
		C = vec4f(rb, ga, rb, ga);
	}
#elif PS_SHUFFLE_ACROSS
#if (PS_PROCESS_BA == SHUFFLE_READWRITE && PS_PROCESS_RG == SHUFFLE_READWRITE)
	C = vec4f(C.b, C.a, C.r, C.g);
#elif (PS_PROCESS_BA & SHUFFLE_READ)
	C = vec4f(C.b, C.a, C.b, C.a);
#else
	C = vec4f(C.r, C.g, C.r, C.g);
#endif
#endif
#endif

	{
		var Crgb = C.rgb;
		ps_dither(&Crgb, alpha_blend.a, input.p);
		ps_color_clamp_wrap(&Crgb);
		C = vec4f(Crgb, C.a);
	}

	ps_fbmask(&C, input.p);

#if (PS_AFAIL == AFAIL_RGB_ONLY_DSB) && !PS_NO_COLOR1
	alpha_blend.a = f32(atst_pass);
#endif

#if PS_RETURN_COLOR
#if PS_RTA_CORRECTION
	let out_a = C.a / 128.0;
#else
	let out_a = C.a / 255.0;
#endif
#if PS_COLCLIP_HW == 1
	var out_c = vec4f(C.rgb / 65535.0, out_a);
#else
	var out_c = vec4f(C.rgb / 255.0, out_a);
#endif

#if PS_AFAIL == AFAIL_FB_ONLY
	if (!atst_pass)
	{
		input_z = sample_from_depth(input.p.xy);
	}
#elif PS_AFAIL == AFAIL_ZB_ONLY
	if (!atst_pass)
	{
		out_c = sample_from_rt(input.p.xy);
	}
#elif (PS_AFAIL == AFAIL_RGB_ONLY || PS_AFAIL == AFAIL_RGB_ONLY_SW_Z)
	if (!atst_pass)
	{
		out_c.a = sample_from_rt(input.p.xy).a;
#if PS_AFAIL == AFAIL_RGB_ONLY_SW_Z
		input_z = sample_from_depth(input.p.xy);
#endif
	}
#endif

	output.c0 = out_c;
#if PS_RETURN_COLOR1
	output.c1 = alpha_blend;
#endif
#endif

#if PS_ZCLAMP
	input_z = min(input_z, cb1.TA_MaxDepth_Af.z);
#endif

#if PS_AA1 == PS_AA1_TRIANGLE_SW_Z
	if (input.interior == 0u)
	{
		input_z = sample_from_depth(input.p.xy);
	}
#endif

#if PS_RETURN_DEPTH
	output.depth = input_z;
#endif
#endif // PS_DATE

#if PS_HAS_OUTPUT
	return output;
#endif
}

#endif // FRAGMENT_SHADER
