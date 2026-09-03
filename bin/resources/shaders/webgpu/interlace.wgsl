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

struct InterlaceUniforms
{
	ZrH: vec4f,
};

@group(0) @binding(0) var samp0: texture_2d<f32>;
@group(0) @binding(1) var samp0_s: sampler;
@group(0) @binding(2) var<uniform> cb: InterlaceUniforms;

fn sample_c(uv: vec2f) -> vec4f
{
	return textureSampleLevel(samp0, samp0_s, uv, 0.0);
}

#ifdef ps_main0
@fragment fn ps_main0(input: PSInput) -> @location(0) vec4f
{
	let idx = i32(cb.ZrH.x);
	let field = idx & 1;
	let vpos = i32(input.p.y);

	if ((vpos & 1) != field)
	{
		discard;
	}
	return sample_c(input.tex);
}
#endif

#ifdef ps_main1
@fragment fn ps_main1(input: PSInput) -> @location(0) vec4f
{
	return sample_c(input.tex);
}
#endif

#ifdef ps_main2
@fragment fn ps_main2(input: PSInput) -> @location(0) vec4f
{
	let vstep = vec2f(0.0, cb.ZrH.y);
	let c0 = sample_c(input.tex - vstep);
	let c1 = sample_c(input.tex);
	let c2 = sample_c(input.tex + vstep);

	return (c0 + c1 * 2.0 + c2) / 4.0;
}
#endif

#ifdef ps_main3
@fragment fn ps_main3(input: PSInput) -> @location(0) vec4f
{
	let idx = i32(cb.ZrH.x);
	let bank = idx >> 1u;
	let field = idx & 1;
	let vres = i32(cb.ZrH.z) >> 1u;
	let lofs = ((((vres + 1) >> 1u) << 1u) - vres) & bank;
	let vpos = i32(input.p.y) + lofs;

	if ((vpos & 1) != field)
	{
		discard;
	}
	return sample_c(input.tex);
}
#endif

#ifdef ps_main4
@fragment fn ps_main4(input: PSInput) -> @location(0) vec4f
{
	let idx = i32(cb.ZrH.x);
	let bank = idx >> 1u;
	let field = idx & 1;
	let vpos = i32(input.p.y);
	let sensitivity = cb.ZrH.w;
	let motion_thr = vec3f(1.0, 1.0, 1.0) * sensitivity;
	let bofs = vec2f(0.0, 0.5);
	let vscale = vec2f(1.0, 0.5);
	let lofs = vec2f(0.0, cb.ZrH.y) * vscale;
	let iptr = input.tex * vscale;

	var p_t0: vec2f;
	var p_t1: vec2f;
	var p_t2: vec2f;
	var p_t3: vec2f;

	switch (idx)
	{
		case 1:
		{
			p_t0 = iptr;
			p_t1 = iptr;
			p_t2 = iptr + bofs;
			p_t3 = iptr + bofs;
		}
		case 2:
		{
			p_t0 = iptr + bofs;
			p_t1 = iptr;
			p_t2 = iptr;
			p_t3 = iptr + bofs;
		}
		case 3:
		{
			p_t0 = iptr + bofs;
			p_t1 = iptr + bofs;
			p_t2 = iptr;
			p_t3 = iptr;
		}
		default:
		{
			p_t0 = iptr;
			p_t1 = iptr + bofs;
			p_t2 = iptr + bofs;
			p_t3 = iptr;
		}
	}

	let hn = sample_c(p_t0 - lofs);
	let cn = sample_c(p_t1);
	let ln = sample_c(p_t0 + lofs);

	let ho = sample_c(p_t2 - lofs);
	let co = sample_c(p_t3);
	let lo = sample_c(p_t2 + lofs);

	var mh = hn.rgb - ho.rgb;
	var mc = cn.rgb - co.rgb;
	var ml = ln.rgb - lo.rgb;

	mh = max(mh, -mh) - motion_thr;
	mc = max(mc, -mc) - motion_thr;
	ml = max(ml, -ml) - motion_thr;

	let mh_max = max(max(mh.x, mh.y), mh.z);
	let mc_max = max(max(mc.x, mc.y), mc.z);
	let ml_max = max(max(ml.x, ml.y), ml.z);

	var o_col0: vec4f;
	if ((vpos & 1) == field)
	{
		o_col0 = sample_c(p_t0);
	}
	else if ((iptr.y > 0.5 - lofs.y) || (iptr.y < 0.0 + lofs.y))
	{
		o_col0 = cn;
	}
	else
	{
		if (((mh_max > 0.0) || (ml_max > 0.0)) || (mc_max > 0.0))
		{
			o_col0 = (hn + ln) / 2.0;
		}
		else
		{
			if ((mh_max != -motion_thr.x) || (ml_max != -motion_thr.x) || (mc_max != -motion_thr.x))
			{
				var mhln = hn.rgb - ln.rgb;
				var mchn = hn.rgb - cn.rgb;

				mhln = max(mhln, -mhln) - motion_thr;
				mchn = max(mchn, -mchn) - motion_thr;

				let mhln_max = max(max(mhln.x, mhln.y), mhln.z);
				let mchn_max = max(max(mchn.x, mchn.y), mchn.z);

				if (mhln_max < 0.0 && mchn_max >= (mhln_max * 0.90))
				{
					o_col0 = (hn + ln) / 2.0;
				}
				else
				{
					o_col0 = cn;
				}
			}
			else
			{
				o_col0 = cn;
			}
		}
	}
	return o_col0;
}
#endif

#endif
