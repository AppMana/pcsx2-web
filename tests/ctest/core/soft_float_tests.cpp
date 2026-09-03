// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Wasm/FloatMode.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <random>

namespace
{
	FPControlRegister MakeFPCR(FPRoundMode mode, bool ftz, bool daz)
	{
		FPControlRegister fpcr = FPControlRegister::GetDefault();
		fpcr.SetRoundMode(mode);
		fpcr.SetFlushToZero(ftz);
		fpcr.SetDenormalsAreZero(daz);
		return fpcr;
	}

	// The EE default: chop to zero with denormals flushed on input and output.
	const FPControlRegister CHOP = MakeFPCR(FPRoundMode::ChopZero, true, true);

	u32 Bits(float v)
	{
		return std::bit_cast<u32>(v);
	}

	float F(u32 bits)
	{
		return std::bit_cast<float>(bits);
	}

	// The hello_tty fixture operands (web/tests/fixtures/hello_tty/main.c).
	constexpr u32 FLOAT_ONE = 0x3f800000u;
	constexpr u32 FLOAT_HALF = 0x3f000000u;
	constexpr u32 FLOAT_TWO = 0x40000000u;
	constexpr u32 FLOAT_THREE = 0x40400000u;
	constexpr u32 FLOAT_FOUR = 0x40800000u;
	constexpr u32 FLOAT_SEVEN = 0x40e00000u;
	constexpr u32 FLOAT_TEN = 0x41200000u;
	constexpr u32 FLOAT_1P23 = 0x3f9d70a4u;
	constexpr u32 FLOAT_1P4 = 0x3fb33333u;
	constexpr u32 FLOAT_1P5_ULP = 0x3fc00001u;
	constexpr u32 FLOAT_123456P789 = 0x47f12065u;
	constexpr u32 FLOAT_MAX = 0x7f7fffffu;
	constexpr u32 FLOAT_MMAX = 0xff7fffffu;
	constexpr u32 FLOAT_ADD_TINY = 0x33c00000u;
	constexpr u32 FLOAT_ADD_TIE = 0x33800000u;
	constexpr u32 FLOAT_SUB_TINY = 0x33a00000u;
	constexpr u32 FLOAT_MIN_NORM = 0x00800000u;
	constexpr u32 FLOAT_MIN_NORM1 = 0x00800001u;
	constexpr u32 FLOAT_DENORM_H = 0x00400000u;
	constexpr u32 FLOAT_ZERO = 0x00000000u;
} // namespace

// Expected values are the EE console lines recorded from the native interpreter
// (web/tests/fixtures/hello_tty/expected/tty.txt).
TEST(SoftFloat, HelloTtyFPU)
{
	EXPECT_EQ(Bits(SoftFloat::Add(F(FLOAT_ONE), F(FLOAT_ADD_TINY), CHOP)), 0x3f800000u); // FPU_ADD_1P5ULP
	EXPECT_EQ(Bits(SoftFloat::Add(F(FLOAT_ONE), F(FLOAT_ADD_TIE), CHOP)), 0x3f800000u); // FPU_ADD_TIE
	EXPECT_EQ(Bits(SoftFloat::Sub(F(FLOAT_ONE), F(FLOAT_SUB_TINY), CHOP)), 0x3f7ffffeu); // FPU_SUB_1P25ULP
	EXPECT_EQ(Bits(SoftFloat::Mul(F(FLOAT_1P5_ULP), F(FLOAT_1P5_ULP), CHOP)), 0x40100001u); // FPU_MUL_1P5ULP_SQ
	EXPECT_EQ(Bits(SoftFloat::Mul(F(FLOAT_1P23), F(FLOAT_1P4), CHOP)), 0x3fdc6a7eu); // FPU_MUL_1P23_1P4
	EXPECT_EQ(Bits(SoftFloat::Div(F(FLOAT_ONE), F(FLOAT_THREE), CHOP)), 0x3eaaaaaau); // FPU_DIV_1_3
	EXPECT_EQ(Bits(SoftFloat::Div(F(FLOAT_TWO), F(FLOAT_THREE), CHOP)), 0x3f2aaaaau); // FPU_DIV_2_3
	EXPECT_EQ(Bits(SoftFloat::Div(F(FLOAT_TEN), F(FLOAT_SEVEN), CHOP)), 0x3fb6db6du); // FPU_DIV_10_7
	EXPECT_EQ(Bits(SoftFloat::Sqrt(F(FLOAT_TWO), CHOP)), 0x3fb504f3u); // FPU_SQRT_2
	EXPECT_EQ(Bits(SoftFloat::Sqrt(F(0x40a00000u), CHOP)), 0x400f1bbcu); // FPU_SQRT_5
	EXPECT_EQ(Bits(SoftFloat::Sqrt(F(FLOAT_123456P789), CHOP)), 0x43afae9du); // FPU_SQRT_123456P789
	EXPECT_EQ(Bits(SoftFloat::DivBySqrtDouble(F(FLOAT_ONE), F(FLOAT_TWO), CHOP)), 0x3f3504f3u); // FPU_RSQRT_1_2

	// madd.s/msub.s: the product is rounded, then the sum.
	const float product = SoftFloat::Mul(F(FLOAT_1P23), F(FLOAT_1P4), CHOP);
	EXPECT_EQ(Bits(SoftFloat::Add(F(FLOAT_ONE), product, CHOP)), 0x402e353fu); // FPU_MADD_1_1P23_1P4
	EXPECT_EQ(Bits(SoftFloat::Sub(F(FLOAT_ONE), product, CHOP)), 0xbf38d4fcu); // FPU_MSUB_1_1P23_1P4

	EXPECT_EQ(Bits(SoftFloat::Add(F(FLOAT_DENORM_H), F(FLOAT_DENORM_H), CHOP)), 0x00000000u); // FPU_DAZ_ADD
	EXPECT_EQ(Bits(SoftFloat::Mul(F(FLOAT_DENORM_H), F(FLOAT_TWO), CHOP)), 0x00000000u); // FPU_DAZ_MUL
	EXPECT_EQ(Bits(SoftFloat::Mul(F(FLOAT_MIN_NORM), F(FLOAT_HALF), CHOP)), 0x00000000u); // FPU_FTZ_MUL
	EXPECT_EQ(Bits(SoftFloat::Div(F(FLOAT_MIN_NORM), F(FLOAT_FOUR), CHOP)), 0x00000000u); // FPU_FTZ_DIV
	EXPECT_EQ(Bits(SoftFloat::Sub(F(FLOAT_MIN_NORM1), F(FLOAT_MIN_NORM), CHOP)), 0x00000000u); // FPU_FTZ_SUB

	// Overflow under chop lands on the largest finite value, which the FPU then reports as FMAX.
	EXPECT_EQ(Bits(SoftFloat::Mul(F(FLOAT_MAX), F(FLOAT_TWO), CHOP)), 0x7f7fffffu); // FPU_OVF_MUL
	EXPECT_EQ(Bits(SoftFloat::Add(F(FLOAT_MAX), F(FLOAT_MAX), CHOP)), 0x7f7fffffu); // FPU_OVF_ADD
	EXPECT_EQ(Bits(SoftFloat::Mul(F(FLOAT_MMAX), F(FLOAT_TWO), CHOP)), 0xff7fffffu); // FPU_OVF_NEG_MUL

	EXPECT_EQ(Bits(SoftFloat::FromInt(0x7fffffff, CHOP)), 0x4effffffu); // FPU_CVTS_INT_MAX
	EXPECT_EQ(Bits(SoftFloat::FromInt(0x01000003, CHOP)), 0x4b800001u); // FPU_CVTS_2P24_3
	EXPECT_EQ(Bits(SoftFloat::FromInt(-1, CHOP)), 0xbf800000u); // FPU_CVTS_M1
}

TEST(SoftFloat, HelloTtyVU0)
{
	// vadd/vsub/vmul lanes: a = {1, 1.5ulp, 1.23, denorm}, b = {tiny, 1.5ulp, 1.4, denorm}.
	EXPECT_EQ(Bits(SoftFloat::Add(F(FLOAT_1P5_ULP), F(FLOAT_1P5_ULP), CHOP)), 0x40400001u); // VU0_ADD_Y
	EXPECT_EQ(Bits(SoftFloat::Add(F(FLOAT_1P23), F(FLOAT_1P4), CHOP)), 0x402851ebu); // VU0_ADD_Z
	EXPECT_EQ(Bits(SoftFloat::Sub(F(FLOAT_1P5_ULP), F(FLOAT_1P5_ULP), CHOP)), 0x00000000u); // VU0_SUB_Y
	EXPECT_EQ(Bits(SoftFloat::Sub(F(FLOAT_1P23), F(FLOAT_1P4), CHOP)), 0xbe2e1478u); // VU0_SUB_Z
	EXPECT_EQ(Bits(SoftFloat::Mul(F(FLOAT_ONE), F(FLOAT_ADD_TINY), CHOP)), 0x33c00000u); // VU0_MUL_X

	// vmula c*d then vmadd a*b: {1*3 + 1*tiny, 2*3 + 1.5ulp^2, 10*7 + 1.23*1.4, 1*0 + 0}.
	const float acc_z = SoftFloat::Mul(F(FLOAT_TEN), F(FLOAT_SEVEN), CHOP);
	EXPECT_EQ(Bits(SoftFloat::Add(acc_z, SoftFloat::Mul(F(FLOAT_1P23), F(FLOAT_1P4), CHOP), CHOP)), 0x428f71a9u); // VU0_MADD_Z
	const float acc_y = SoftFloat::Mul(F(FLOAT_TWO), F(FLOAT_THREE), CHOP);
	EXPECT_EQ(Bits(SoftFloat::Add(acc_y, SoftFloat::Mul(F(FLOAT_1P5_ULP), F(FLOAT_1P5_ULP), CHOP), CHOP)), 0x41040000u); // VU0_MADD_Y

	// vitof0 of {INT_MAX, 2^24 + 3, -1, 3}.
	EXPECT_EQ(Bits(SoftFloat::FromInt(0x7fffffff, CHOP)), 0x4effffffu); // VU0_ITOF0_X
	EXPECT_EQ(Bits(SoftFloat::FromInt(0x01000003, CHOP)), 0x4b800001u); // VU0_ITOF0_Y
	EXPECT_EQ(Bits(SoftFloat::FromInt(3, CHOP)), 0x40400000u); // VU0_ITOF0_W

	// vopmula/vopmsub on {itof(ints).xyz, c.xyz}: ACC = fs.yzx * ft.zxy, then ACC - fs.yzx * ft.zxy.
	const float ix = F(0x4effffffu), iy = F(0x4b800001u), iz = F(0xbf800000u);
	const float cx = F(FLOAT_ONE), cy = F(FLOAT_TWO), cz = F(FLOAT_TEN);
	const float accx = SoftFloat::Mul(iy, cz, CHOP);
	const float accy = SoftFloat::Mul(iz, cx, CHOP);
	const float accz = SoftFloat::Mul(ix, cy, CHOP);
	EXPECT_EQ(Bits(SoftFloat::Sub(accx, SoftFloat::Mul(cy, iz, CHOP), CHOP)), 0x4d200001u); // VU0_OPMSUB_X
	EXPECT_EQ(Bits(SoftFloat::Sub(accy, SoftFloat::Mul(cz, ix, CHOP), CHOP)), 0xd09fffffu); // VU0_OPMSUB_Y
	EXPECT_EQ(Bits(SoftFloat::Sub(accz, SoftFloat::Mul(cx, iy, CHOP), CHOP)), 0x4f7efffeu); // VU0_OPMSUB_Z

	// Overflow operands: ACC = {MAX*2, MAX*MAX, -MAX*2, minnorm*0.5} then + a*b.
	EXPECT_EQ(Bits(SoftFloat::Mul(F(FLOAT_MAX), F(FLOAT_MAX), CHOP)), 0x7f7fffffu); // VU0_OVF_MUL_Y
	EXPECT_EQ(Bits(SoftFloat::Mul(F(FLOAT_MIN_NORM), F(FLOAT_HALF), CHOP)), 0x00000000u); // VU0_OVF_MUL_W
	const float ovf_z = SoftFloat::Mul(F(FLOAT_MMAX), F(FLOAT_TWO), CHOP);
	EXPECT_EQ(Bits(SoftFloat::Add(ovf_z, SoftFloat::Mul(F(FLOAT_1P23), F(FLOAT_1P4), CHOP), CHOP)), 0xff7ffffeu); // VU0_OVF_MADD_Z
	EXPECT_EQ(Bits(SoftFloat::Div(F(FLOAT_MAX), F(FLOAT_TWO), CHOP)), 0x7effffffu); // VU0_DIV_MAX_2
	EXPECT_EQ(Bits(SoftFloat::Div(F(FLOAT_MAX), F(FLOAT_MAX), CHOP)), 0x3f800000u); // VU0_DIV_MAX_MAX
}

// A sum whose exact value needs more than 53 bits: 1 - 2^-60 truncates to the single below 1.
TEST(SoftFloat, ResidualBeyondDouble)
{
	const float one = 1.0f;
	const float tiny = std::ldexp(1.0f, -60);
	EXPECT_EQ(Bits(SoftFloat::Sub(one, tiny, MakeFPCR(FPRoundMode::ChopZero, false, false))), 0x3f7fffffu);
	EXPECT_EQ(Bits(SoftFloat::Sub(one, tiny, MakeFPCR(FPRoundMode::NegativeInfinity, false, false))), 0x3f7fffffu);
	EXPECT_EQ(Bits(SoftFloat::Add(one, tiny, MakeFPCR(FPRoundMode::PositiveInfinity, false, false))), 0x3f800001u);
	EXPECT_EQ(Bits(SoftFloat::Add(one, tiny, MakeFPCR(FPRoundMode::Nearest, false, false))), 0x3f800000u);

	// Exactly on a midpoint in double, with the residual deciding the tie.
	const float half_ulp = std::ldexp(1.0f, -24);
	EXPECT_EQ(Bits(SoftFloat::Add(SoftFloat::Add(one, half_ulp, MakeFPCR(FPRoundMode::Nearest, false, false)), tiny, MakeFPCR(FPRoundMode::Nearest, false, false))), 0x3f800000u);
	const float above_mid = std::ldexp(1.0f, -24);
	const float base = 1.0f;
	// (1 + 2^-24) + 2^-60 exceeds the midpoint: rounds up.
	const double exact = static_cast<double>(base) + static_cast<double>(above_mid);
	EXPECT_EQ(Bits(SoftFloat::RoundResidual(exact, static_cast<double>(tiny), FPRoundMode::Nearest)), 0x3f800001u);
	EXPECT_EQ(Bits(SoftFloat::RoundResidual(exact, -static_cast<double>(tiny), FPRoundMode::Nearest)), 0x3f800000u);
	EXPECT_EQ(Bits(SoftFloat::RoundResidual(exact, 0.0, FPRoundMode::Nearest)), 0x3f800000u);
}

TEST(SoftFloat, ZeroSigns)
{
	const FPControlRegister rd = MakeFPCR(FPRoundMode::NegativeInfinity, false, false);
	const FPControlRegister rn = MakeFPCR(FPRoundMode::Nearest, false, false);
	EXPECT_EQ(Bits(SoftFloat::Sub(1.0f, 1.0f, rn)), 0x00000000u);
	EXPECT_EQ(Bits(SoftFloat::Sub(1.0f, 1.0f, rd)), 0x80000000u);
	EXPECT_EQ(Bits(SoftFloat::Add(0.0f, 0.0f, rd)), 0x00000000u);
	EXPECT_EQ(Bits(SoftFloat::Add(-0.0f, -0.0f, rn)), 0x80000000u);
	EXPECT_EQ(Bits(SoftFloat::Add(-0.0f, 0.0f, rn)), 0x00000000u);
	EXPECT_EQ(Bits(SoftFloat::Add(-0.0f, 0.0f, rd)), 0x80000000u);
}

#if defined(ARCH_X86) && !defined(PCSX2_SOFT_FLOAT_MODE)

// Differential test against the hardware: the same operation under MXCSR in every mode, with
// the software result computed under the default control register.
namespace
{
	struct HardwareResults
	{
		float add, sub, mul, div, sqrt, rsqrt, fromint;
	};

	__attribute__((noinline)) HardwareResults RunHardware(float a, float b, s32 i, FPControlRegister fpcr)
	{
		volatile float va = a;
		volatile float vb = b;
		volatile s32 vi = i;
		volatile float out_add, out_sub, out_mul, out_div, out_sqrt, out_rsqrt, out_fromint;
		volatile double dout;

		const FPControlRegisterBackup backup(fpcr);
		asm volatile("" ::: "memory");
		out_add = va + vb;
		out_sub = va - vb;
		out_mul = va * vb;
		out_div = va / vb;
		out_sqrt = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(std::fabs(va))));
		dout = static_cast<double>(va) / _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(static_cast<double>(vb))));
		out_rsqrt = static_cast<float>(dout);
		out_fromint = static_cast<float>(vi);
		asm volatile("" ::: "memory");
		return HardwareResults{out_add, out_sub, out_mul, out_div, out_sqrt, out_rsqrt, out_fromint};
	}

	bool SameBits(float x, float y)
	{
		if (std::isnan(x) && std::isnan(y))
			return true;
		return Bits(x) == Bits(y);
	}

	void CheckAgainstHardware(float a, float b, s32 i, FPControlRegister fpcr, const char* what)
	{
		const HardwareResults hw = RunHardware(a, b, i, fpcr);
		const float soft_add = SoftFloat::Add(a, b, fpcr);
		const float soft_sub = SoftFloat::Sub(a, b, fpcr);
		const float soft_mul = SoftFloat::Mul(a, b, fpcr);
		const float soft_div = SoftFloat::Div(a, b, fpcr);
		const float soft_sqrt = SoftFloat::Sqrt(std::fabs(a), fpcr);
		const float soft_fromint = SoftFloat::FromInt(i, fpcr);
		EXPECT_TRUE(SameBits(hw.add, soft_add)) << what << " add " << std::hex << Bits(a) << " " << Bits(b) << " hw " << Bits(hw.add) << " soft " << Bits(soft_add);
		EXPECT_TRUE(SameBits(hw.sub, soft_sub)) << what << " sub " << std::hex << Bits(a) << " " << Bits(b) << " hw " << Bits(hw.sub) << " soft " << Bits(soft_sub);
		EXPECT_TRUE(SameBits(hw.mul, soft_mul)) << what << " mul " << std::hex << Bits(a) << " " << Bits(b) << " hw " << Bits(hw.mul) << " soft " << Bits(soft_mul);
		EXPECT_TRUE(SameBits(hw.div, soft_div)) << what << " div " << std::hex << Bits(a) << " " << Bits(b) << " hw " << Bits(hw.div) << " soft " << Bits(soft_div);
		EXPECT_TRUE(SameBits(hw.sqrt, soft_sqrt)) << what << " sqrt " << std::hex << Bits(a) << " hw " << Bits(hw.sqrt) << " soft " << Bits(soft_sqrt);
		EXPECT_TRUE(SameBits(hw.fromint, soft_fromint)) << what << " fromint " << std::hex << i << " hw " << Bits(hw.fromint) << " soft " << Bits(soft_fromint);
		if (b > 0.0f && std::isfinite(b))
		{
			const float soft_rsqrt = SoftFloat::DivBySqrtDouble(a, b, fpcr);
			EXPECT_TRUE(SameBits(hw.rsqrt, soft_rsqrt)) << what << " rsqrt " << std::hex << Bits(a) << " " << Bits(b) << " hw " << Bits(hw.rsqrt) << " soft " << Bits(soft_rsqrt);
		}
	}

	// Finite, non-NaN operands: the interpreters clamp infinities and NaN payloads are not
	// reproducible on wasm anyway.
	u32 FiniteBits(std::mt19937& rng)
	{
		for (;;)
		{
			const u32 bits = rng();
			if ((bits & 0x7f800000u) != 0x7f800000u)
				return bits;
		}
	}

	// Operand pairs within a few binades so that sums and differences round.
	u32 NearbyBits(std::mt19937& rng, u32 base)
	{
		const s32 exp = static_cast<s32>((base >> 23) & 0xff);
		const s32 delta = static_cast<s32>(rng() % 61) - 30;
		const s32 new_exp = std::clamp(exp + delta, 0, 254);
		return (rng() & 0x807fffffu) | (static_cast<u32>(new_exp) << 23);
	}
} // namespace

TEST(SoftFloat, MatchesHardwareAllModes)
{
	std::mt19937 rng(0x5eed1234u);
	static constexpr FPRoundMode modes[] = {FPRoundMode::Nearest, FPRoundMode::ChopZero, FPRoundMode::NegativeInfinity, FPRoundMode::PositiveInfinity};
	static constexpr const char* names[] = {"nearest", "chop", "down", "up"};

	for (u32 m = 0; m < 4; m++)
	{
		for (u32 flags = 0; flags < 4; flags++)
		{
			const FPControlRegister fpcr = MakeFPCR(modes[m], (flags & 1) != 0, (flags & 2) != 0);
			for (u32 n = 0; n < 40000; n++)
			{
				const u32 a = FiniteBits(rng);
				const u32 b = (n & 1) ? NearbyBits(rng, a) : FiniteBits(rng);
				CheckAgainstHardware(F(a), F(b), static_cast<s32>(rng()), fpcr, names[m]);
				if (HasFailure())
					return;
			}

			// Denormal-heavy operands and the overflow edge.
			for (u32 n = 0; n < 4000; n++)
			{
				const u32 a = rng() & 0x80ffffffu;
				const u32 b = (n & 1) ? (rng() & 0x80ffffffu) : FiniteBits(rng);
				CheckAgainstHardware(F(a), F(b), static_cast<s32>(rng()), fpcr, names[m]);
				const u32 big = 0x7f000000u | (rng() & 0x807fffffu);
				CheckAgainstHardware(F(big), F((n & 2) ? big : NearbyBits(rng, big)), static_cast<s32>(rng()), fpcr, names[m]);
				if (HasFailure())
					return;
			}
		}
	}
}

#endif
