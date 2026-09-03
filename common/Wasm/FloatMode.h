// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Software emulation of the MXCSR/FPCR rounding and denormal controls for hosts that only offer
// round-to-nearest-even with denormals (wasm32), and for the PCSX2_SOFT_FLOAT_MODE native build
// that verifies the emulation against the hardware interpreter.
//
// Every helper takes the exact real result of a single-precision operation, computed in double
// precision plus (where a double cannot hold it) an exact residual, and rounds it to single
// precision in the requested direction. The host itself must be in round-to-nearest-even with
// denormals enabled, which is what both hosts guarantee.

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/FPControl.h"

#include <bit>
#include <cmath>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace SoftFloat
{
	static constexpr u32 FLOAT_MAX_BITS = 0x7f7fffffu;
	static constexpr u32 FLOAT_INF_BITS = 0x7f800000u;

	// Largest single value: 2^128 - 2^104. The round-to-nearest overflow threshold, halfway to
	// 2^128, is 2^128 - 2^103.
	static constexpr double FLOAT_MAX_VALUE = 340282346638528859811704183484516925440.0;
	static constexpr double FLOAT_OVERFLOW_THRESHOLD = 340282356779733661637539395458142568448.0;

	__forceinline_odr float FromBits(u32 bits)
	{
		return std::bit_cast<float>(bits);
	}

	__forceinline_odr u32 ToBits(float v)
	{
		return std::bit_cast<u32>(v);
	}

	__forceinline_odr bool IsDenormal(float v)
	{
		const u32 bits = ToBits(v);
		return ((bits & 0x7f800000u) == 0) && ((bits & 0x007fffffu) != 0);
	}

	__forceinline_odr float FlushDenormal(float v)
	{
		const u32 bits = ToBits(v);
		return ((bits & 0x7f800000u) == 0) ? FromBits(bits & 0x80000000u) : v;
	}

	__forceinline_odr float ApplyDAZ(float v, FPControlRegister fpcr)
	{
		return fpcr.GetDenormalsAreZero() ? FlushDenormal(v) : v;
	}

	__forceinline_odr float ApplyFTZ(float v, FPControlRegister fpcr)
	{
		return fpcr.GetFlushToZero() ? FlushDenormal(v) : v;
	}

	// The compare instructions also treat denormal inputs as zero under DAZ.
	__forceinline_odr bool IsZero(float v, FPControlRegister fpcr)
	{
		const u32 bits = ToBits(v);
		return ((bits & 0x7fffffffu) == 0) || (fpcr.GetDenormalsAreZero() && (bits & 0x7f800000u) == 0);
	}

	__forceinline_odr float CopySignMax(double sign_of)
	{
		return FromBits(FLOAT_MAX_BITS | (std::signbit(sign_of) ? 0x80000000u : 0u));
	}

	__forceinline_odr float CopySignInf(double sign_of)
	{
		return FromBits(FLOAT_INF_BITS | (std::signbit(sign_of) ? 0x80000000u : 0u));
	}

	// Adjacent singles by bit stepping, so that denormals and the overflow to infinity behave the
	// same on every host.
	__forceinline_odr float StepTowardZero(float v)
	{
		return FromBits(ToBits(v) - 1u);
	}

	__forceinline_odr float StepAwayFromZero(float v)
	{
		return FromBits(ToBits(v) + 1u);
	}

	__forceinline_odr float NextDown(float v)
	{
		if (v == 0.0f)
			return FromBits(0x80000001u);
		return (v > 0.0f) ? StepTowardZero(v) : StepAwayFromZero(v);
	}

	__forceinline_odr float NextUp(float v)
	{
		if (v == 0.0f)
			return FromBits(0x00000001u);
		return (v > 0.0f) ? StepAwayFromZero(v) : StepTowardZero(v);
	}

	// Result of an operation whose exact value exceeds the single range in magnitude.
	__forceinline_odr float Overflow(double sign_of, FPRoundMode mode)
	{
		switch (mode)
		{
			case FPRoundMode::ChopZero:
				return CopySignMax(sign_of);
			case FPRoundMode::NegativeInfinity:
				return std::signbit(sign_of) ? CopySignInf(sign_of) : CopySignMax(sign_of);
			case FPRoundMode::PositiveInfinity:
				return std::signbit(sign_of) ? CopySignMax(sign_of) : CopySignInf(sign_of);
			case FPRoundMode::Nearest:
			default:
				return CopySignInf(sign_of);
		}
	}

	// Rounds the real number s + e to single precision, where s is a finite double and e is the
	// exact residual of the double operation that produced s: |e| <= ulp(s) / 2, and e == 0 when s
	// itself is exact. Because every single and every midpoint between singles is a double, s + e
	// lies on the same side of every rounding boundary as s unless s is the boundary, and then the
	// sign of e decides.
	inline float RoundResidual(double s, double e, FPRoundMode mode)
	{
		float r = static_cast<float>(s);
		if (!std::isfinite(s))
			return r;

		const double rd = static_cast<double>(r);
		if (std::isinf(r))
		{
			// |s| >= 2^128 - 2^103, so s + e exceeds the single range unless s sits exactly on the
			// threshold and e points back toward zero, which only round-to-nearest cares about.
			if (mode == FPRoundMode::Nearest && std::fabs(s) == FLOAT_OVERFLOW_THRESHOLD && e != 0.0 &&
				std::signbit(e) != std::signbit(s))
			{
				return CopySignMax(s);
			}
			return Overflow(s, mode);
		}

		switch (mode)
		{
			case FPRoundMode::Nearest:
			{
				if (rd == s)
					return r;

				// s lies strictly between r and its neighbour on the side of s. r is wrong only when
				// s is exactly their midpoint and the residual breaks the tie away from r.
				const float n = (s > rd) ? NextUp(r) : NextDown(r);
				if (std::isinf(n))
					return r;
				const double nd = static_cast<double>(n);
				const double mid = (rd + nd) * 0.5;
				if (s != mid || e == 0.0)
					return r;
				return (std::signbit(e) == std::signbit(nd - rd)) ? n : r;
			}

			case FPRoundMode::ChopZero:
			{
				if (rd != s)
					return (std::fabs(rd) > std::fabs(s)) ? StepTowardZero(r) : r;
				if (e != 0.0 && std::signbit(e) != std::signbit(s) && r != 0.0f)
					return StepTowardZero(r);
				return r;
			}

			case FPRoundMode::NegativeInfinity:
			{
				if (rd != s)
					return (rd > s) ? NextDown(r) : r;
				if (e < 0.0)
					return NextDown(r);
				return r;
			}

			case FPRoundMode::PositiveInfinity:
			default:
			{
				if (rd != s)
					return (rd < s) ? NextUp(r) : r;
				if (e > 0.0)
					return NextUp(r);
				return r;
			}
		}
	}

	// Exact product of two doubles as an unevaluated sum (Dekker), valid when no overflow or
	// underflow of the partial products occurs.
	__forceinline_odr void TwoProduct(double a, double b, double& hi, double& lo)
	{
		constexpr double SPLITTER = 134217729.0; // 2^27 + 1
		hi = a * b;
		const double ca = SPLITTER * a;
		const double ah = ca - (ca - a);
		const double al = a - ah;
		const double cb = SPLITTER * b;
		const double bh = cb - (cb - b);
		const double bl = b - bh;
		lo = ((ah * bh - hi) + ah * bl + al * bh) + al * bl;
	}

	// Sign of the exact value num - c * den, where c * den is within a factor of two of num so
	// that num - hi is exact (Sterbenz) and one rounded addition cannot lose the sign.
	__forceinline_odr int ResidualSign(double num, double c, double den)
	{
		double hi, lo;
		TwoProduct(c, den, hi, lo);
		const double rem = (num - hi) - lo;
		return (rem > 0.0) ? 1 : ((rem < 0.0) ? -1 : 0);
	}

	// Directed rounding of the exact quotient num / den (both finite, den != 0) to single
	// precision. The double quotient rounded to single is within one ulp of the exact value; the
	// residual decides whether it overshot.
	inline float RoundQuotient(double num, double den, FPRoundMode mode)
	{
		const double q = num / den;
		if (mode == FPRoundMode::Nearest)
			return static_cast<float>(q);

		const float r = static_cast<float>(q);
		if (!std::isfinite(r))
		{
			if (std::isnan(r))
				return r;
			return Overflow(q, mode);
		}

		// exact > r when num - r * den has the sign of den.
		const int rem = ResidualSign(num, static_cast<double>(r), den);
		const bool above = (rem != 0) && ((rem > 0) == !std::signbit(den));
		const bool below = (rem != 0) && !above;
		switch (mode)
		{
			case FPRoundMode::ChopZero:
				if ((r > 0.0f && below) || (r < 0.0f && above))
					return StepTowardZero(r);
				return r;
			case FPRoundMode::NegativeInfinity:
				return below ? NextDown(r) : r;
			case FPRoundMode::PositiveInfinity:
			default:
				return above ? NextUp(r) : r;
		}
	}

	// Square root of a finite non-negative double, rounded in the requested direction.
	inline double SqrtDouble(double a, FPRoundMode mode)
	{
		double r = std::sqrt(a);
		if (mode == FPRoundMode::Nearest || r == 0.0 || !std::isfinite(r))
			return r;

		// r * r versus a, exactly.
		double hi, lo;
		TwoProduct(r, r, hi, lo);
		const double rem = (hi - a) + lo;
		switch (mode)
		{
			case FPRoundMode::ChopZero:
			case FPRoundMode::NegativeInfinity:
				if (rem > 0.0)
					r = std::nextafter(r, 0.0);
				break;
			case FPRoundMode::PositiveInfinity:
			default:
				if (rem < 0.0)
					r = std::nextafter(r, INFINITY);
				break;
		}
		return r;
	}

	// Single rounding of the exact sum x + y of two finite doubles that cannot overflow: the
	// rounded sum plus its TwoSum residual is the exact value.
	__forceinline_odr float SumRound(double x, double y, FPRoundMode mode)
	{
		const double s = x + y;
		if (s == 0.0)
		{
			// An exact zero sum is -0 only when both inputs are -0, except when rounding toward
			// negative infinity, where x + (-x) is -0 as well.
			if (mode == FPRoundMode::NegativeInfinity && (std::signbit(x) || std::signbit(y)))
				return -0.0f;
			return static_cast<float>(s);
		}

		const double bb = s - x;
		const double e = (x - (s - bb)) + (y - bb);
		return RoundResidual(s, e, mode);
	}

	__forceinline_odr float Add(float a, float b, FPControlRegister fpcr)
	{
		a = ApplyDAZ(a, fpcr);
		b = ApplyDAZ(b, fpcr);
		if (!std::isfinite(a) || !std::isfinite(b))
			return a + b;

		return ApplyFTZ(SumRound(static_cast<double>(a), static_cast<double>(b), fpcr.GetRoundMode()), fpcr);
	}

	// a * b + c with one rounding, as the host FMA instruction computes it. The product of two
	// singles is exact in a double, so the fused result is the rounded sum of that product and c.
	__forceinline_odr float Fma(float a, float b, float c, FPControlRegister fpcr)
	{
		a = ApplyDAZ(a, fpcr);
		b = ApplyDAZ(b, fpcr);
		c = ApplyDAZ(c, fpcr);
		if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c))
			return a * b + c;

		return ApplyFTZ(SumRound(static_cast<double>(a) * static_cast<double>(b), static_cast<double>(c), fpcr.GetRoundMode()), fpcr);
	}

	__forceinline_odr float Sub(float a, float b, FPControlRegister fpcr)
	{
		return Add(a, -b, fpcr);
	}

	__forceinline_odr float Mul(float a, float b, FPControlRegister fpcr)
	{
		a = ApplyDAZ(a, fpcr);
		b = ApplyDAZ(b, fpcr);
		if (!std::isfinite(a) || !std::isfinite(b))
			return a * b;

		// The product of two singles is exact in a double.
		return ApplyFTZ(RoundResidual(static_cast<double>(a) * static_cast<double>(b), 0.0, fpcr.GetRoundMode()), fpcr);
	}

	__forceinline_odr float Div(float a, float b, FPControlRegister fpcr)
	{
		a = ApplyDAZ(a, fpcr);
		b = ApplyDAZ(b, fpcr);
		if (!std::isfinite(a) || !std::isfinite(b) || a == 0.0f || b == 0.0f)
			return a / b;

		return ApplyFTZ(RoundQuotient(static_cast<double>(a), static_cast<double>(b), fpcr.GetRoundMode()), fpcr);
	}

	__forceinline_odr float Sqrt(float a, FPControlRegister fpcr)
	{
		a = ApplyDAZ(a, fpcr);
		if (!(a > 0.0f) || !std::isfinite(a))
			return std::sqrt(a);

		const FPRoundMode mode = fpcr.GetRoundMode();
		float r = std::sqrt(a);
		if (mode != FPRoundMode::Nearest)
		{
			// r * r is exact in a double and tells which side of the root r landed on.
			const double sq = static_cast<double>(r) * static_cast<double>(r);
			const double da = static_cast<double>(a);
			if ((mode == FPRoundMode::ChopZero || mode == FPRoundMode::NegativeInfinity) && sq > da)
				r = StepTowardZero(r);
			else if (mode == FPRoundMode::PositiveInfinity && sq < da)
				r = StepAwayFromZero(r);
		}
		return ApplyFTZ(r, fpcr);
	}

	// (float)((double)a / sqrt((double)b)): the EE FPU interpreter's RSQRT evaluates the root and
	// the quotient in double precision under the host rounding mode before narrowing. Directed
	// rounding composes through the narrowing, so the result is the direct rounding of the exact
	// a / sqrt_mode(b); round-to-nearest is the same expression the hardware evaluates.
	__forceinline_odr float DivBySqrtDouble(float a, float b, FPControlRegister fpcr)
	{
		a = ApplyDAZ(a, fpcr);
		b = ApplyDAZ(b, fpcr);
		const double da = static_cast<double>(a);
		const double db = static_cast<double>(b);
		const FPRoundMode mode = fpcr.GetRoundMode();
		if (mode == FPRoundMode::Nearest || !std::isfinite(a) || !std::isfinite(b) || !(b > 0.0f) || a == 0.0f)
			return ApplyFTZ(static_cast<float>(da / std::sqrt(db)), fpcr);

		return ApplyFTZ(RoundQuotient(da, SqrtDouble(db, mode), mode), fpcr);
	}

	__forceinline_odr float FromInt(s32 v, FPControlRegister fpcr)
	{
		return RoundResidual(static_cast<double>(v), 0.0, fpcr.GetRoundMode());
	}
} // namespace SoftFloat
