// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/GS/Renderers/WebGPU/GSDeviceWebGPU.h"
#include "pcsx2/GS/Renderers/WebGPU/WGSLPreprocessor.h"

#include <gtest/gtest.h>

namespace
{
	std::string Expand(const WGSLPreprocessor& pp, const char* source)
	{
		std::string out;
		std::string error;
		EXPECT_TRUE(pp.Process(source, &out, &error)) << error;
		return out;
	}

	std::string ExpectFail(const WGSLPreprocessor& pp, const char* source)
	{
		std::string out;
		std::string error;
		EXPECT_FALSE(pp.Process(source, &out, &error));
		return error;
	}
} // namespace

TEST(WGSLPreprocessor, PassesThroughPlainText)
{
	WGSLPreprocessor pp;
	EXPECT_EQ(Expand(pp, "fn main() {}\nlet x = 1;"), "fn main() {}\nlet x = 1;");
	EXPECT_EQ(Expand(pp, "a\n"), "a\n");
	EXPECT_EQ(Expand(pp, ""), "");
}

TEST(WGSLPreprocessor, IfdefElseEndifPreservesLineNumbers)
{
	WGSLPreprocessor pp;
	pp.Define("FOO", 1);
	EXPECT_EQ(Expand(pp, "#ifdef FOO\na\n#else\nb\n#endif\nc"), "\na\n\n\n\nc");
	EXPECT_EQ(Expand(pp, "#ifdef BAR\na\n#else\nb\n#endif\nc"), "\n\n\nb\n\nc");
	EXPECT_EQ(Expand(pp, "#ifndef BAR\na\n#endif"), "\na\n");
}

TEST(WGSLPreprocessor, NestedConditionals)
{
	WGSLPreprocessor pp;
	pp.Define("A", 1);
	pp.Define("B", 0);
	const char* src =
		"#if A\n"
		"#if B\n"
		"ab\n"
		"#else\n"
		"a!b\n"
		"#if defined(C)\n"
		"never\n"
		"#endif\n"
		"#endif\n"
		"#else\n"
		"#if B\n"
		"!ab\n"
		"#endif\n"
		"#endif\n";
	EXPECT_EQ(Expand(pp, src), "\n\n\n\na!b\n\n\n\n\n\n\n\n\n\n");
}

TEST(WGSLPreprocessor, ElifChains)
{
	WGSLPreprocessor pp;
	pp.Define("V", 2);
	const char* src = "#if V == 1\none\n#elif V == 2\ntwo\n#elif V == 2\ntwo again\n#else\nother\n#endif";
	EXPECT_EQ(Expand(pp, src), "\n\n\ntwo\n\n\n\n\n");
	pp.Define("V", 7);
	EXPECT_EQ(Expand(pp, src), "\n\n\n\n\n\n\nother\n");
}

TEST(WGSLPreprocessor, ExpressionEvaluation)
{
	WGSLPreprocessor::DefineMap defines;
	defines["A"] = "3";
	defines["B"] = "0x10";
	defines["EMPTY"] = "";
	defines["ALIAS"] = "A";
	defines["EXPR"] = "A + 1";

	const auto eval = [&](const char* expr) {
		s64 result = -12345;
		std::string error;
		EXPECT_TRUE(WGSLPreprocessor::EvaluateExpression(expr, defines, &result, &error)) << expr << ": " << error;
		return result;
	};

	EXPECT_EQ(eval("1 + 2 * 3"), 7);
	EXPECT_EQ(eval("(1 + 2) * 3"), 9);
	EXPECT_EQ(eval("A"), 3);
	EXPECT_EQ(eval("B"), 16);
	EXPECT_EQ(eval("B >> 2"), 4);
	EXPECT_EQ(eval("1 << 4"), 16);
	EXPECT_EQ(eval("A == 3 && B != 3"), 1);
	EXPECT_EQ(eval("!A"), 0);
	EXPECT_EQ(eval("!!A"), 1);
	EXPECT_EQ(eval("-A"), -3);
	EXPECT_EQ(eval("~0"), -1);
	EXPECT_EQ(eval("A & 1"), 1);
	EXPECT_EQ(eval("A | 4"), 7);
	EXPECT_EQ(eval("A ^ 1"), 2);
	EXPECT_EQ(eval("A % 2"), 1);
	EXPECT_EQ(eval("7 / 2"), 3);
	EXPECT_EQ(eval("A < 4"), 1);
	EXPECT_EQ(eval("A <= 3"), 1);
	EXPECT_EQ(eval("A > 3"), 0);
	EXPECT_EQ(eval("A >= 3"), 1);
	EXPECT_EQ(eval("A ? 10 : 20"), 10);
	EXPECT_EQ(eval("0 ? 10 : 20"), 20);
	EXPECT_EQ(eval("defined(A)"), 1);
	EXPECT_EQ(eval("defined A"), 1);
	EXPECT_EQ(eval("defined(UNKNOWN)"), 0);
	EXPECT_EQ(eval("UNKNOWN"), 0);
	EXPECT_EQ(eval("UNKNOWN == 0"), 1);
	EXPECT_EQ(eval("EMPTY"), 1);
	EXPECT_EQ(eval("ALIAS"), 3);
	EXPECT_EQ(eval("EXPR"), 4);
	EXPECT_EQ(eval("EXPR * 2"), 8);
	EXPECT_EQ(eval("1u"), 1);
	EXPECT_EQ(eval("A // comment"), 3);
	EXPECT_EQ(eval("true && !false"), 1);
}

TEST(WGSLPreprocessor, ExpressionErrors)
{
	WGSLPreprocessor::DefineMap defines;
	defines["SELF"] = "SELF + 1";
	s64 result;
	std::string error;
	EXPECT_FALSE(WGSLPreprocessor::EvaluateExpression("1 +", defines, &result, &error));
	EXPECT_FALSE(WGSLPreprocessor::EvaluateExpression("(1", defines, &result, &error));
	EXPECT_FALSE(WGSLPreprocessor::EvaluateExpression("1 / 0", defines, &result, &error));
	EXPECT_FALSE(WGSLPreprocessor::EvaluateExpression("SELF", defines, &result, &error));
	EXPECT_FALSE(WGSLPreprocessor::EvaluateExpression("1 2", defines, &result, &error));
	EXPECT_FALSE(WGSLPreprocessor::EvaluateExpression("$", defines, &result, &error));
}

TEST(WGSLPreprocessor, MacroSubstitutionInBody)
{
	WGSLPreprocessor pp;
	pp.Define("PS_FST", 1);
	pp.Define("NAME", "alias");
	pp.Define("ALIAS_OF_ALIAS", "NAME");
	pp.Define("EMPTY", "");
	EXPECT_EQ(Expand(pp, "let a = PS_FST;"), "let a = 1;");
	EXPECT_EQ(Expand(pp, "let PS_FST_2 = PS_FSTX + XPS_FST;"), "let PS_FST_2 = PS_FSTX + XPS_FST;");
	EXPECT_EQ(Expand(pp, "NAME ALIAS_OF_ALIAS"), "alias alias");
	EXPECT_EQ(Expand(pp, "aEMPTYb EMPTY c"), "aEMPTYb EMPTY c");
	EXPECT_EQ(Expand(pp, "1.5e3 0x1F 2PS_FST"), "1.5e3 0x1F 2PS_FST");
}

TEST(WGSLPreprocessor, DefineAndUndefInSource)
{
	WGSLPreprocessor pp;
	const char* src =
		"#define X 5\n"
		"X\n"
		"#undef X\n"
		"X\n"
		"#define Y\n"
		"#ifdef Y\n"
		"y\n"
		"#endif\n"
		"#if Y\n"
		"y2\n"
		"#endif\n";
	EXPECT_EQ(Expand(pp, src), "\n5\n\nX\n\n\ny\n\n\ny2\n\n");
}

TEST(WGSLPreprocessor, DefinesInInactiveBlocksAreIgnored)
{
	WGSLPreprocessor pp;
	EXPECT_EQ(Expand(pp, "#if 0\n#define X 1\n#error nope\n#endif\nX"), "\n\n\n\nX");
}

TEST(WGSLPreprocessor, DirectivesTolerateWhitespaceAndComments)
{
	WGSLPreprocessor pp;
	pp.Define("A", 1);
	EXPECT_EQ(Expand(pp, "  #  if A // comment\nx\n\t#endif // done"), "\nx\n");
}

TEST(WGSLPreprocessor, Errors)
{
	WGSLPreprocessor pp;
	EXPECT_NE(ExpectFail(pp, "#if 1\nx"), "");
	EXPECT_NE(ExpectFail(pp, "#endif"), "");
	EXPECT_NE(ExpectFail(pp, "#else"), "");
	EXPECT_NE(ExpectFail(pp, "#if 1\n#else\n#else\n#endif"), "");
	EXPECT_NE(ExpectFail(pp, "#if 1\n#else\n#elif 1\n#endif"), "");
	EXPECT_NE(ExpectFail(pp, "#define F(x) x"), "");
	EXPECT_NE(ExpectFail(pp, "#include \"foo\""), "");
	EXPECT_NE(ExpectFail(pp, "#error boom"), "");
	EXPECT_NE(ExpectFail(pp, "#if 1 +\n#endif"), "");
	EXPECT_NE(ExpectFail(pp, "#ifdef\n#endif"), "");
	EXPECT_EQ(ExpectFail(pp, "a\n#endif").substr(0, 7), "line 2:");
}

namespace
{
	constexpr const char* PS_MACRO_TEMPLATE =
		"fst PS_FST wms PS_WMS wmt PS_WMT adjs PS_ADJS adjt PS_ADJT\n"
		"aem_fmt PS_AEM_FMT pal_fmt PS_PAL_FMT dst_fmt PS_DST_FMT depth_fmt PS_DEPTH_FMT\n"
		"channel PS_CHANNEL_FETCH urban PS_URBAN_CHAOS_HLE tales PS_TALES_OF_ABYSS_HLE aem PS_AEM\n"
		"tfx PS_TFX tcc PS_TCC atst PS_ATST afail PS_AFAIL fog PS_FOG blend_hw PS_BLEND_HW a_masked PS_A_MASKED fba PS_FBA\n"
		"ltf PS_LTF auto_lod PS_AUTOMATIC_LOD manual_lod PS_MANUAL_LOD colclip PS_COLCLIP date PS_DATE tcoffsethack PS_TCOFFSETHACK region_rect PS_REGION_RECT\n"
		"blend PS_BLEND_A PS_BLEND_B PS_BLEND_C PS_BLEND_D mix PS_BLEND_MIX round_inv PS_ROUND_INV fixed_one_a PS_FIXED_ONE_A iip PS_IIP\n"
		"shuffle PS_SHUFFLE PS_SHUFFLE_SAME PS_PROCESS_BA PS_PROCESS_RG PS_SHUFFLE_ACROSS PS_READ16_SRC PS_WRITE_RG fbmask PS_FBMASK\n"
		"colclip_hw PS_COLCLIP_HW rta PS_RTA_CORRECTION PS_RTA_SRC_CORRECTION dither PS_DITHER PS_DITHER_ADJUST zclamp PS_ZCLAMP zfloor PS_ZFLOOR pabe PS_PABE\n"
		"scanmsk PS_SCANMSK tex_is_fb PS_TEX_IS_FB no_color PS_NO_COLOR PS_NO_COLOR1 ztst PS_ZTST aa1 PS_AA1 abe PS_ABE aniso PS_ANISOTROPIC_FILTERING rov PS_ROV_COLOR PS_ROV_DEPTH\n"
		"#if FRAGMENT_SHADER\nfragment\n#endif\n";

	constexpr const char* VS_MACRO_TEMPLATE = "tme VS_TME fst VS_FST iip VS_IIP point_size VS_POINT_SIZE expand VS_EXPAND pvl VS_PROVOKING_VERTEX_LAST\n#ifdef VERTEX_SHADER\nvertex\n#endif\n";

	std::string ExpandPS(const GSHWDrawConfig::PSSelector& sel)
	{
		WGSLPreprocessor pp;
		GSDeviceWebGPU::AddTFXFragmentShaderMacros(pp, sel);
		return Expand(pp, PS_MACRO_TEMPLATE);
	}

	std::string ExpandVS(GSHWDrawConfig::VSSelector sel, bool pvl)
	{
		WGSLPreprocessor pp;
		GSDeviceWebGPU::AddTFXVertexShaderMacros(pp, sel, pvl);
		return Expand(pp, VS_MACRO_TEMPLATE);
	}
} // namespace

TEST(WGSLPreprocessor, GoldenTFXFragmentMacroTableZero)
{
	GSHWDrawConfig::PSSelector sel;
	EXPECT_EQ(ExpandPS(sel),
		"fst 0 wms 0 wmt 0 adjs 0 adjt 0\n"
		"aem_fmt 0 pal_fmt 0 dst_fmt 0 depth_fmt 0\n"
		"channel 0 urban 0 tales 0 aem 0\n"
		"tfx 0 tcc 0 atst 0 afail 0 fog 0 blend_hw 0 a_masked 0 fba 0\n"
		"ltf 0 auto_lod 0 manual_lod 0 colclip 0 date 0 tcoffsethack 0 region_rect 0\n"
		"blend 0 0 0 0 mix 0 round_inv 0 fixed_one_a 0 iip 0\n"
		"shuffle 0 0 0 0 0 0 0 fbmask 0\n"
		"colclip_hw 0 rta 0 0 dither 0 0 zclamp 0 zfloor 0 pabe 0\n"
		"scanmsk 0 tex_is_fb 0 no_color 0 0 ztst 0 aa1 0 abe 0 aniso 0 rov 0 0\n"
		"\nfragment\n\n");
}

TEST(WGSLPreprocessor, GoldenTFXFragmentMacroTableTextured)
{
	GSHWDrawConfig::PSSelector sel;
	sel.fst = 1;
	sel.wms = 3;
	sel.wmt = 2;
	sel.adjs = 1;
	sel.aem_fmt = 2;
	sel.pal_fmt = 3;
	sel.dst_fmt = 1;
	sel.depth_fmt = 3;
	sel.channel = 6;
	sel.tfx = 4;
	sel.tcc = 1;
	sel.atst = GSShader::PS_ATST::NOTEQUAL;
	sel.afail = GSShader::PS_AFAIL::RGB_ONLY_SW_Z;
	sel.fog = 1;
	sel.blend_hw = 6;
	sel.ltf = 1;
	sel.automatic_lod = 1;
	sel.date = 3;
	sel.region_rect = 1;
	sel.blend_a = 2;
	sel.blend_b = 1;
	sel.blend_c = 2;
	sel.blend_d = 1;
	sel.blend_mix = 2;
	sel.iip = 1;
	sel.shuffle = 1;
	sel.process_ba = 3;
	sel.process_rg = 1;
	sel.write_rg = 1;
	sel.fbmask = 1;
	sel.colclip_hw = 1;
	sel.rta_correction = 1;
	sel.dither = 3;
	sel.dither_adjust = 1;
	sel.zclamp = 1;
	sel.scanmsk = 2;
	sel.no_color1 = 1;
	sel.ztst = 3;
	sel.aa1 = GSShader::PS_AA1::TRIANGLE_SW_Z;
	sel.abe = 1;
	sel.sw_aniso = 16;
	EXPECT_EQ(ExpandPS(sel),
		"fst 1 wms 3 wmt 2 adjs 1 adjt 0\n"
		"aem_fmt 2 pal_fmt 3 dst_fmt 1 depth_fmt 3\n"
		"channel 6 urban 0 tales 0 aem 0\n"
		"tfx 4 tcc 1 atst 4 afail 5 fog 1 blend_hw 6 a_masked 0 fba 0\n"
		"ltf 1 auto_lod 1 manual_lod 0 colclip 0 date 3 tcoffsethack 0 region_rect 1\n"
		"blend 2 1 2 1 mix 2 round_inv 0 fixed_one_a 0 iip 1\n"
		"shuffle 1 0 3 1 0 0 1 fbmask 1\n"
		"colclip_hw 1 rta 1 0 dither 3 1 zclamp 1 zfloor 0 pabe 0\n"
		"scanmsk 2 tex_is_fb 0 no_color 0 1 ztst 3 aa1 3 abe 1 aniso 16 rov 0 0\n"
		"\nfragment\n\n");
}

TEST(WGSLPreprocessor, GoldenTFXFragmentMacroTableFromKey)
{
	GSHWDrawConfig::PSSelector sel;
	sel.key_lo = 0x0000000000006401ull;
	sel.key_hi = 0x0000000000000000ull;
	EXPECT_EQ(sel.aem_fmt, 1u);
	EXPECT_EQ(sel.dst_fmt, 0u);
	EXPECT_EQ(sel.fog, 1u);
	EXPECT_EQ(sel.date, 6u);
	EXPECT_EQ(ExpandPS(sel),
		"fst 0 wms 0 wmt 0 adjs 0 adjt 0\n"
		"aem_fmt 1 pal_fmt 0 dst_fmt 0 depth_fmt 0\n"
		"channel 0 urban 0 tales 0 aem 0\n"
		"tfx 0 tcc 0 atst 0 afail 0 fog 1 blend_hw 0 a_masked 0 fba 0\n"
		"ltf 0 auto_lod 0 manual_lod 0 colclip 0 date 6 tcoffsethack 0 region_rect 0\n"
		"blend 0 0 0 0 mix 0 round_inv 0 fixed_one_a 0 iip 0\n"
		"shuffle 0 0 0 0 0 0 0 fbmask 0\n"
		"colclip_hw 0 rta 0 0 dither 0 0 zclamp 0 zfloor 0 pabe 0\n"
		"scanmsk 0 tex_is_fb 0 no_color 0 0 ztst 0 aa1 0 abe 0 aniso 0 rov 0 0\n"
		"\nfragment\n\n");
}

TEST(WGSLPreprocessor, GoldenTFXVertexMacroTable)
{
	GSHWDrawConfig::VSSelector sel;
	EXPECT_EQ(ExpandVS(sel, false), "tme 0 fst 0 iip 0 point_size 0 expand 0 pvl 0\n\nvertex\n\n");

	sel.key = 0;
	sel.tme = 1;
	sel.fst = 1;
	sel.expand = GSShader::VSExpand::TriangleAA1;
	EXPECT_EQ(ExpandVS(sel, true), "tme 1 fst 1 iip 0 point_size 0 expand 5 pvl 1\n\nvertex\n\n");

	GSHWDrawConfig::VSSelector from_key(0x2Cu);
	EXPECT_EQ(ExpandVS(from_key, false), "tme 0 fst 0 iip 1 point_size 1 expand 2 pvl 0\n\nvertex\n\n");
}

TEST(WGSLPreprocessor, GoldenConvertMacroTable)
{
	WGSLPreprocessor pp;
	GSDeviceWebGPU::AddConvertShaderMacros(pp, ShaderConvertSelector(ShaderConvert::RGBA8_TO_DEPTH16, 0xf, true, Filter::Biln));
	EXPECT_EQ(Expand(pp, "HAS_BILN HAS_STENCIL_OUTPUT HAS_INTEGER_OUTPUT HAS_DEPTH_OUTPUT HAS_FLOAT32_INPUT HAS_FLOAT32_OUTPUT"), "1 0 0 1 0 1");

	WGSLPreprocessor pp2;
	GSDeviceWebGPU::AddConvertShaderMacros(pp2, ShaderConvertSelector(ShaderConvert::DEPTH32_TO_16_BITS));
	EXPECT_EQ(Expand(pp2, "HAS_BILN HAS_STENCIL_OUTPUT HAS_INTEGER_OUTPUT HAS_DEPTH_OUTPUT HAS_FLOAT32_INPUT HAS_FLOAT32_OUTPUT"), "0 0 1 0 1 0");

	WGSLPreprocessor pp3;
	GSDeviceWebGPU::AddConvertShaderMacros(pp3, ShaderConvertSelector(ShaderConvert::DATM_1));
	EXPECT_EQ(Expand(pp3, "HAS_BILN HAS_STENCIL_OUTPUT HAS_INTEGER_OUTPUT HAS_DEPTH_OUTPUT HAS_FLOAT32_INPUT HAS_FLOAT32_OUTPUT"), "0 1 0 0 0 0");
}
