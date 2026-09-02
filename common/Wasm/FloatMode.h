// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Software emulation of the MXCSR/FPCR rounding and denormal controls for wasm32, which only
// offers round-to-nearest-even and has no flush-to-zero or denormals-are-zero mode.

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/FPControl.h"

#include <bit>
#include <cmath>

namespace WasmFloat
{
	__fi float FlushDenormal(float v)
	{
		const u32 bits = std::bit_cast<u32>(v);
		return ((bits & 0x7f800000u) == 0) ? std::bit_cast<float>(bits & 0x80000000u) : v;
	}

	__fi float ApplyDAZ(float v, FPControlRegister fpcr)
	{
		return fpcr.GetDenormalsAreZero() ? FlushDenormal(v) : v;
	}

	__fi float ApplyFTZ(float v, FPControlRegister fpcr)
	{
		return fpcr.GetFlushToZero() ? FlushDenormal(v) : v;
	}

	// exact holds the mathematically exact result of a single-precision operation (f32 + - * fit in
	// f64 without rounding).  Demote to nearest, then step toward zero when the magnitude grew.
	__fi float ChopFromExact(double exact)
	{
		const float rounded = static_cast<float>(exact);
		if (std::isfinite(rounded) && std::fabs(static_cast<double>(rounded)) > std::fabs(exact))
			return std::nextafter(rounded, 0.0f);
		return rounded;
	}

	__fi float RoundFromExact(double exact, FPControlRegister fpcr)
	{
		switch (fpcr.GetRoundMode())
		{
			case FPRoundMode::ChopZero:
				return ChopFromExact(exact);
			case FPRoundMode::NegativeInfinity:
			{
				const float rounded = static_cast<float>(exact);
				return (std::isfinite(rounded) && static_cast<double>(rounded) > exact) ? std::nextafter(rounded, -INFINITY) : rounded;
			}
			case FPRoundMode::PositiveInfinity:
			{
				const float rounded = static_cast<float>(exact);
				return (std::isfinite(rounded) && static_cast<double>(rounded) < exact) ? std::nextafter(rounded, INFINITY) : rounded;
			}
			case FPRoundMode::Nearest:
			default:
				return static_cast<float>(exact);
		}
	}

	__fi float Add(float a, float b, FPControlRegister fpcr)
	{
		a = ApplyDAZ(a, fpcr);
		b = ApplyDAZ(b, fpcr);
		return ApplyFTZ(RoundFromExact(static_cast<double>(a) + static_cast<double>(b), fpcr), fpcr);
	}

	__fi float Sub(float a, float b, FPControlRegister fpcr)
	{
		a = ApplyDAZ(a, fpcr);
		b = ApplyDAZ(b, fpcr);
		return ApplyFTZ(RoundFromExact(static_cast<double>(a) - static_cast<double>(b), fpcr), fpcr);
	}

	__fi float Mul(float a, float b, FPControlRegister fpcr)
	{
		a = ApplyDAZ(a, fpcr);
		b = ApplyDAZ(b, fpcr);
		return ApplyFTZ(RoundFromExact(static_cast<double>(a) * static_cast<double>(b), fpcr), fpcr);
	}

	// The f64 quotient is itself rounded, so the direction is decided from the exact remainder
	// a - r * b, which f64 represents exactly for the nearest candidates (Sterbenz).
	__fi float Div(float a, float b, FPControlRegister fpcr)
	{
		a = ApplyDAZ(a, fpcr);
		b = ApplyDAZ(b, fpcr);
		float r = static_cast<float>(static_cast<double>(a) / static_cast<double>(b));
		if (std::isfinite(r) && r != 0.0f && std::isfinite(a) && std::isfinite(b) && b != 0.0f)
		{
			const double rem = static_cast<double>(a) - static_cast<double>(r) * static_cast<double>(b);
			const bool overshoot = (rem != 0.0) && (std::signbit(rem) != std::signbit(a));
			const bool undershoot = (rem != 0.0) && (std::signbit(rem) == std::signbit(a));
			switch (fpcr.GetRoundMode())
			{
				case FPRoundMode::ChopZero:
					if (overshoot)
						r = std::nextafter(r, 0.0f);
					break;
				case FPRoundMode::NegativeInfinity:
					if ((r > 0.0f && overshoot) || (r < 0.0f && undershoot))
						r = std::nextafter(r, -INFINITY);
					break;
				case FPRoundMode::PositiveInfinity:
					if ((r > 0.0f && undershoot) || (r < 0.0f && overshoot))
						r = std::nextafter(r, INFINITY);
					break;
				case FPRoundMode::Nearest:
				default:
					break;
			}
		}
		return ApplyFTZ(r, fpcr);
	}

	// r * r is exact in f64, which gives the direction of the correctly rounded f32 sqrt.
	__fi float Sqrt(float a, FPControlRegister fpcr)
	{
		a = ApplyDAZ(a, fpcr);
		float r = std::sqrt(a);
		if (std::isfinite(r) && r > 0.0f)
		{
			const double sq = static_cast<double>(r) * static_cast<double>(r);
			switch (fpcr.GetRoundMode())
			{
				case FPRoundMode::ChopZero:
				case FPRoundMode::NegativeInfinity:
					if (sq > static_cast<double>(a))
						r = std::nextafter(r, 0.0f);
					break;
				case FPRoundMode::PositiveInfinity:
					if (sq < static_cast<double>(a))
						r = std::nextafter(r, INFINITY);
					break;
				case FPRoundMode::Nearest:
				default:
					break;
			}
		}
		return ApplyFTZ(r, fpcr);
	}
} // namespace WasmFloat
