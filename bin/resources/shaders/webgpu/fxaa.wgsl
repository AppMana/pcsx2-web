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

#define FxaaSubpixMax 0.0
#define FxaaEdgeThreshold 0.063
#define FxaaEdgeThresholdMin 0.00
#define FXAA_QUALITY_P0 1.0
#define FXAA_QUALITY_P1 1.5
#define FXAA_QUALITY_P2 2.0
#define FXAA_QUALITY_P3 2.0
#define FXAA_QUALITY_P4 2.0
#define FXAA_QUALITY_P5 2.0
#define FXAA_QUALITY_P6 2.0
#define FXAA_QUALITY_P7 2.0
#define FXAA_QUALITY_P8 2.0
#define FXAA_QUALITY_P9 2.0
#define FXAA_QUALITY_P10 4.0
#define FXAA_QUALITY_P11 8.0
#define FXAA_QUALITY_P12 8.0

struct PSInput
{
	@builtin(position) p: vec4f,
	@location(0) tex: vec2f,
};

@group(0) @binding(0) var TextureSampler: texture_2d<f32>;
@group(0) @binding(1) var TextureSampler_s: sampler;

fn FxaaSat(x: vec3f) -> vec3f
{
	return clamp(x, vec3f(0.0), vec3f(1.0));
}

fn RGBLuminance(color: vec3f) -> f32
{
	let lumCoeff = vec3f(0.2126729, 0.7151522, 0.0721750);
	return dot(color.rgb, lumCoeff);
}

fn RGBGammaToLinear(color_in: vec3f, gamma: f32) -> vec3f
{
	let color = FxaaSat(color_in);
	return vec3f(
		select(pow((color.r + 0.055) / 1.055, gamma), color.r / 12.92, color.r <= 0.0404482362771082),
		select(pow((color.g + 0.055) / 1.055, gamma), color.g / 12.92, color.g <= 0.0404482362771082),
		select(pow((color.b + 0.055) / 1.055, gamma), color.b / 12.92, color.b <= 0.0404482362771082));
}

fn LinearToRGBGamma(color_in: vec3f, gamma: f32) -> vec3f
{
	let color = FxaaSat(color_in);
	return vec3f(
		select(1.055 * pow(color.r, 1.0 / gamma) - 0.055, color.r * 12.92, color.r <= 0.00313066844250063),
		select(1.055 * pow(color.g, 1.0 / gamma) - 0.055, color.g * 12.92, color.g <= 0.00313066844250063),
		select(1.055 * pow(color.b, 1.0 / gamma) - 0.055, color.b * 12.92, color.b <= 0.00313066844250063));
}

fn PreGammaPass(color_in: vec4f) -> vec4f
{
	let GammaConst = 2.233;
	var rgb = RGBGammaToLinear(color_in.rgb, GammaConst);
	rgb = LinearToRGBGamma(rgb, GammaConst);
	return vec4f(rgb, RGBLuminance(rgb));
}

fn FxaaLuma(rgba: vec4f) -> f32
{
	return RGBLuminance(rgba.xyz);
}

fn FxaaPixelShader(pos: vec2f, fxaaRcpFrame: vec2f, fxaaSubpix: f32, fxaaEdgeThreshold: f32, fxaaEdgeThresholdMin: f32) -> vec4f
{
	var posM = pos;
	var rgbyM = textureSampleLevel(TextureSampler, TextureSampler_s, posM, 0.0);
	rgbyM.w = RGBLuminance(rgbyM.xyz);
	
	var lumaS = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posM, 0.0, vec2i(0, 1)));
	var lumaE = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posM, 0.0, vec2i(1, 0)));
	var lumaN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posM, 0.0, vec2i(0, -1)));
	var lumaW = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posM, 0.0, vec2i(-1, 0)));

	var maxSM = max(lumaS, rgbyM.w);
	var minSM = min(lumaS, rgbyM.w);
	var maxESM = max(lumaE, maxSM);
	var minESM = min(lumaE, minSM);
	var maxWN = max(lumaN, lumaW);
	var minWN = min(lumaN, lumaW);

	var rangeMax = max(maxWN, maxESM);
	var rangeMin = min(minWN, minESM);
	var range = rangeMax - rangeMin;
	var rangeMaxScaled = rangeMax * fxaaEdgeThreshold;
	var rangeMaxClamped = max(fxaaEdgeThresholdMin, rangeMaxScaled);

	if (range < rangeMaxClamped)
	{
		return rgbyM;
	}

	var lumaNW = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posM, 0.0, vec2i(-1, -1)));
	var lumaSE = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posM, 0.0, vec2i(1, 1)));
	var lumaNE = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posM, 0.0, vec2i(1, -1)));
	var lumaSW = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posM, 0.0, vec2i(-1, 1)));

	var lumaNS = lumaN + lumaS;
	var lumaWE = lumaW + lumaE;
	var subpixRcpRange = 1.0 / range;
	var subpixNSWE = lumaNS + lumaWE;
	var edgeHorz1 = (-2.0 * rgbyM.w) + lumaNS;
	var edgeVert1 = (-2.0 * rgbyM.w) + lumaWE;
	var lumaNESE = lumaNE + lumaSE;
	var lumaNWNE = lumaNW + lumaNE;
	var edgeHorz2 = (-2.0 * lumaE) + lumaNESE;
	var edgeVert2 = (-2.0 * lumaN) + lumaNWNE;

	var lumaNWSW = lumaNW + lumaSW;
	var lumaSWSE = lumaSW + lumaSE;
	var edgeHorz4 = (abs(edgeHorz1) * 2.0) + abs(edgeHorz2);
	var edgeVert4 = (abs(edgeVert1) * 2.0) + abs(edgeVert2);
	var edgeHorz3 = (-2.0 * lumaW) + lumaNWSW;
	var edgeVert3 = (-2.0 * lumaS) + lumaSWSE;
	var edgeHorz = abs(edgeHorz3) + edgeHorz4;
	var edgeVert = abs(edgeVert3) + edgeVert4;

	var subpixNWSWNESE = lumaNWSW + lumaNESE;
	var lengthSign = fxaaRcpFrame.x;
	var horzSpan = edgeHorz >= edgeVert;
	var subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
	if (!horzSpan) { lumaN = lumaW; }
	if (!horzSpan) { lumaS = lumaE; }
	if (horzSpan) { lengthSign = fxaaRcpFrame.y; }
	var subpixB = (subpixA * (1.0 / 12.0)) - rgbyM.w;

	var gradientN = lumaN - rgbyM.w;
	var gradientS = lumaS - rgbyM.w;
	var lumaNN = lumaN + rgbyM.w;
	var lumaSS = lumaS + rgbyM.w;
	var pairN = abs(gradientN) >= abs(gradientS);
	var gradient = max(abs(gradientN), abs(gradientS));
	if (pairN) { lengthSign = -lengthSign; }
	var subpixC = clamp(abs(subpixB) * subpixRcpRange, 0.0, 1.0);

	var posB: vec2f;
	posB.x = posM.x;
	posB.y = posM.y;
	var offNP: vec2f;
	offNP.x = select(fxaaRcpFrame.x, 0.0, !horzSpan);
	offNP.y = select(fxaaRcpFrame.y, 0.0, horzSpan);
	if (!horzSpan) { posB.x += lengthSign * 0.5; }
	if ( horzSpan) { posB.y += lengthSign * 0.5; }

	var posN: vec2f;
	posN.x = posB.x - offNP.x * FXAA_QUALITY_P0;
	posN.y = posB.y - offNP.y * FXAA_QUALITY_P0;
	var posP: vec2f;
	posP.x = posB.x + offNP.x * FXAA_QUALITY_P0;
	posP.y = posB.y + offNP.y * FXAA_QUALITY_P0;
	var subpixD = ((-2.0) * subpixC) + 3.0;
	var lumaEndN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posN, 0.0));
	var subpixE = subpixC * subpixC;
	var lumaEndP = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posP, 0.0));

	if (!pairN) { lumaNN = lumaSS; }
	var gradientScaled = gradient * 1.0 / 4.0;
	var lumaMM = rgbyM.w - lumaNN * 0.5;
	var subpixF = subpixD * subpixE;
	var lumaMLTZero = lumaMM < 0.0;
	lumaEndN -= lumaNN * 0.5;
	lumaEndP -= lumaNN * 0.5;
	var doneN = abs(lumaEndN) >= gradientScaled;
	var doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN) { posN.x -= offNP.x * FXAA_QUALITY_P1; }
	if (!doneN) { posN.y -= offNP.y * FXAA_QUALITY_P1; }
	var doneNP = (!doneN) || (!doneP);
	if (!doneP) { posP.x += offNP.x * FXAA_QUALITY_P1; }
	if (!doneP) { posP.y += offNP.y * FXAA_QUALITY_P1; }

	if (doneNP) {
	if (!doneN) { lumaEndN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posN.xy, 0.0)); }
	if (!doneP) { lumaEndP = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posP.xy, 0.0)); }
	if (!doneN) { lumaEndN = lumaEndN - lumaNN * 0.5; }
	if (!doneP) { lumaEndP = lumaEndP - lumaNN * 0.5; }
	doneN = abs(lumaEndN) >= gradientScaled;
	doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN) { posN.x -= offNP.x * FXAA_QUALITY_P2; }
	if (!doneN) { posN.y -= offNP.y * FXAA_QUALITY_P2; }
	doneNP = (!doneN) || (!doneP);
	if (!doneP) { posP.x += offNP.x * FXAA_QUALITY_P2; }
	if (!doneP) { posP.y += offNP.y * FXAA_QUALITY_P2; }

	if (doneNP) {
	if (!doneN) { lumaEndN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posN.xy, 0.0)); }
	if (!doneP) { lumaEndP = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posP.xy, 0.0)); }
	if (!doneN) { lumaEndN = lumaEndN - lumaNN * 0.5; }
	if (!doneP) { lumaEndP = lumaEndP - lumaNN * 0.5; }
	doneN = abs(lumaEndN) >= gradientScaled;
	doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN) { posN.x -= offNP.x * FXAA_QUALITY_P3; }
	if (!doneN) { posN.y -= offNP.y * FXAA_QUALITY_P3; }
	doneNP = (!doneN) || (!doneP);
	if (!doneP) { posP.x += offNP.x * FXAA_QUALITY_P3; }
	if (!doneP) { posP.y += offNP.y * FXAA_QUALITY_P3; }

	if (doneNP) {
	if (!doneN) { lumaEndN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posN.xy, 0.0)); }
	if (!doneP) { lumaEndP = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posP.xy, 0.0)); }
	if (!doneN) { lumaEndN = lumaEndN - lumaNN * 0.5; }
	if (!doneP) { lumaEndP = lumaEndP - lumaNN * 0.5; }
	doneN = abs(lumaEndN) >= gradientScaled;
	doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN) { posN.x -= offNP.x * FXAA_QUALITY_P4; }
	if (!doneN) { posN.y -= offNP.y * FXAA_QUALITY_P4; }
	doneNP = (!doneN) || (!doneP);
	if (!doneP) { posP.x += offNP.x * FXAA_QUALITY_P4; }
	if (!doneP) { posP.y += offNP.y * FXAA_QUALITY_P4; }

	if (doneNP) {
	if (!doneN) { lumaEndN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posN.xy, 0.0)); }
	if (!doneP) { lumaEndP = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posP.xy, 0.0)); }
	if (!doneN) { lumaEndN = lumaEndN - lumaNN * 0.5; }
	if (!doneP) { lumaEndP = lumaEndP - lumaNN * 0.5; }
	doneN = abs(lumaEndN) >= gradientScaled;
	doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN) { posN.x -= offNP.x * FXAA_QUALITY_P5; }
	if (!doneN) { posN.y -= offNP.y * FXAA_QUALITY_P5; }
	doneNP = (!doneN) || (!doneP);
	if (!doneP) { posP.x += offNP.x * FXAA_QUALITY_P5; }
	if (!doneP) { posP.y += offNP.y * FXAA_QUALITY_P5; }

	if (doneNP) {
	if (!doneN) { lumaEndN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posN.xy, 0.0)); }
	if (!doneP) { lumaEndP = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posP.xy, 0.0)); }
	if (!doneN) { lumaEndN = lumaEndN - lumaNN * 0.5; }
	if (!doneP) { lumaEndP = lumaEndP - lumaNN * 0.5; }
	doneN = abs(lumaEndN) >= gradientScaled;
	doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN) { posN.x -= offNP.x * FXAA_QUALITY_P6; }
	if (!doneN) { posN.y -= offNP.y * FXAA_QUALITY_P6; }
	doneNP = (!doneN) || (!doneP);
	if (!doneP) { posP.x += offNP.x * FXAA_QUALITY_P6; }
	if (!doneP) { posP.y += offNP.y * FXAA_QUALITY_P6; }

	if (doneNP) {
	if (!doneN) { lumaEndN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posN.xy, 0.0)); }
	if (!doneP) { lumaEndP = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posP.xy, 0.0)); }
	if (!doneN) { lumaEndN = lumaEndN - lumaNN * 0.5; }
	if (!doneP) { lumaEndP = lumaEndP - lumaNN * 0.5; }
	doneN = abs(lumaEndN) >= gradientScaled;
	doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN) { posN.x -= offNP.x * FXAA_QUALITY_P7; }
	if (!doneN) { posN.y -= offNP.y * FXAA_QUALITY_P7; }
	doneNP = (!doneN) || (!doneP);
	if (!doneP) { posP.x += offNP.x * FXAA_QUALITY_P7; }
	if (!doneP) { posP.y += offNP.y * FXAA_QUALITY_P7; }

	if (doneNP) {
	if (!doneN) { lumaEndN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posN.xy, 0.0)); }
	if (!doneP) { lumaEndP = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posP.xy, 0.0)); }
	if (!doneN) { lumaEndN = lumaEndN - lumaNN * 0.5; }
	if (!doneP) { lumaEndP = lumaEndP - lumaNN * 0.5; }
	doneN = abs(lumaEndN) >= gradientScaled;
	doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN) { posN.x -= offNP.x * FXAA_QUALITY_P8; }
	if (!doneN) { posN.y -= offNP.y * FXAA_QUALITY_P8; }
	doneNP = (!doneN) || (!doneP);
	if (!doneP) { posP.x += offNP.x * FXAA_QUALITY_P8; }
	if (!doneP) { posP.y += offNP.y * FXAA_QUALITY_P8; }

	if (doneNP) {
	if (!doneN) { lumaEndN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posN.xy, 0.0)); }
	if (!doneP) { lumaEndP = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posP.xy, 0.0)); }
	if (!doneN) { lumaEndN = lumaEndN - lumaNN * 0.5; }
	if (!doneP) { lumaEndP = lumaEndP - lumaNN * 0.5; }
	doneN = abs(lumaEndN) >= gradientScaled;
	doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN) { posN.x -= offNP.x * FXAA_QUALITY_P9; }
	if (!doneN) { posN.y -= offNP.y * FXAA_QUALITY_P9; }
	doneNP = (!doneN) || (!doneP);
	if (!doneP) { posP.x += offNP.x * FXAA_QUALITY_P9; }
	if (!doneP) { posP.y += offNP.y * FXAA_QUALITY_P9; }

	if (doneNP) {
	if (!doneN) { lumaEndN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posN.xy, 0.0)); }
	if (!doneP) { lumaEndP = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posP.xy, 0.0)); }
	if (!doneN) { lumaEndN = lumaEndN - lumaNN * 0.5; }
	if (!doneP) { lumaEndP = lumaEndP - lumaNN * 0.5; }
	doneN = abs(lumaEndN) >= gradientScaled;
	doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN) { posN.x -= offNP.x * FXAA_QUALITY_P10; }
	if (!doneN) { posN.y -= offNP.y * FXAA_QUALITY_P10; }
	doneNP = (!doneN) || (!doneP);
	if (!doneP) { posP.x += offNP.x * FXAA_QUALITY_P10; }
	if (!doneP) { posP.y += offNP.y * FXAA_QUALITY_P10; }

	if (doneNP) {
	if (!doneN) { lumaEndN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posN.xy, 0.0)); }
	if (!doneP) { lumaEndP = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posP.xy, 0.0)); }
	if (!doneN) { lumaEndN = lumaEndN - lumaNN * 0.5; }
	if (!doneP) { lumaEndP = lumaEndP - lumaNN * 0.5; }
	doneN = abs(lumaEndN) >= gradientScaled;
	doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN) { posN.x -= offNP.x * FXAA_QUALITY_P11; }
	if (!doneN) { posN.y -= offNP.y * FXAA_QUALITY_P11; }
	doneNP = (!doneN) || (!doneP);
	if (!doneP) { posP.x += offNP.x * FXAA_QUALITY_P11; }
	if (!doneP) { posP.y += offNP.y * FXAA_QUALITY_P11; }

	if (doneNP) {
	if (!doneN) { lumaEndN = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posN.xy, 0.0)); }
	if (!doneP) { lumaEndP = FxaaLuma(textureSampleLevel(TextureSampler, TextureSampler_s, posP.xy, 0.0)); }
	if (!doneN) { lumaEndN = lumaEndN - lumaNN * 0.5; }
	if (!doneP) { lumaEndP = lumaEndP - lumaNN * 0.5; }
	doneN = abs(lumaEndN) >= gradientScaled;
	doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN) { posN.x -= offNP.x * FXAA_QUALITY_P12; }
	if (!doneN) { posN.y -= offNP.y * FXAA_QUALITY_P12; }
	doneNP = (!doneN) || (!doneP);
	if (!doneP) { posP.x += offNP.x * FXAA_QUALITY_P12; }
	if (!doneP) { posP.y += offNP.y * FXAA_QUALITY_P12; }
	}}}}}}}}}}}

	var dstN = posM.x - posN.x;
	var dstP = posP.x - posM.x;
	if (!horzSpan) { dstN = posM.y - posN.y; }
	if (!horzSpan) { dstP = posP.y - posM.y; }

	var goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
	var spanLength = (dstP + dstN);
	var goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
	var spanLengthRcp = 1.0 / spanLength;

	var directionN = dstN < dstP;
	var dst = min(dstN, dstP);
	var goodSpan = select(goodSpanP, goodSpanN, directionN);
	var subpixG = subpixF * subpixF;
	var pixelOffset = (dst * (-spanLengthRcp)) + 0.5;
	var subpixH = subpixG * fxaaSubpix;

	var pixelOffsetGood = select(0.0, pixelOffset, goodSpan);
	var pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
	if (!horzSpan) { posM.x += pixelOffsetSubpix * lengthSign; }
	if ( horzSpan) { posM.y += pixelOffsetSubpix * lengthSign; }

	return vec4f(textureSampleLevel(TextureSampler, TextureSampler_s, posM, 0.0).xyz, rgbyM.w);
}


fn FxaaPass(FxaaColor: vec4f, uv0: vec2f) -> vec4f
{
	let PixelSize = vec2f(textureDimensions(TextureSampler, 0));
	return FxaaPixelShader(uv0, 1.0 / PixelSize.xy, FxaaSubpixMax, FxaaEdgeThreshold, FxaaEdgeThresholdMin);
}

@fragment fn ps_main(input: PSInput) -> @location(0) vec4f
{
	var color = textureSampleLevel(TextureSampler, TextureSampler_s, input.tex, 0.0);
	color = PreGammaPass(color);
	color = FxaaPass(color, input.tex);

	return vec4f(color.rgb, 1.0);
}

#endif
