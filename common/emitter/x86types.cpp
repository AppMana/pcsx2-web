// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Register and operand types shared by every backend of the emitter API: the x86 encoder in
// x86emitter.cpp and the wasm lowering in wasm/. Nothing in here writes code.

#include "common/emitter/x86types.h"

namespace x86Emitter
{
	thread_local XMMSSEType g_xmmtypes[iREGCNT_XMM] = {XMMT_INT};

	// Empty initializers are due to frivolously pointless GCC errors (it demands the
	// objects be initialized even though they have no actual variable members).

	const xAddressIndexer<xIndirectVoid> ptr = {};
	const xAddressIndexer<xIndirectNative> ptrNative = {};
	const xAddressIndexer<xIndirect128> ptr128 = {};
	const xAddressIndexer<xIndirect64> ptr64 = {};
	const xAddressIndexer<xIndirect32> ptr32 = {};
	const xAddressIndexer<xIndirect16> ptr16 = {};
	const xAddressIndexer<xIndirect8> ptr8 = {};

	// ------------------------------------------------------------------------

	const xRegisterEmpty xEmptyReg = {};

	// clang-format off

const xRegisterSSE
    xmm0(0), xmm1(1),
    xmm2(2), xmm3(3),
    xmm4(4), xmm5(5),
    xmm6(6), xmm7(7),
    xmm8(8), xmm9(9),
    xmm10(10), xmm11(11),
    xmm12(12), xmm13(13),
    xmm14(14), xmm15(15);

const xRegisterSSE
    ymm0(0, xRegisterYMMTag()), ymm1(1, xRegisterYMMTag()),
    ymm2(2, xRegisterYMMTag()), ymm3(3, xRegisterYMMTag()),
    ymm4(4, xRegisterYMMTag()), ymm5(5, xRegisterYMMTag()),
    ymm6(6, xRegisterYMMTag()), ymm7(7, xRegisterYMMTag()),
    ymm8(8, xRegisterYMMTag()), ymm9(9, xRegisterYMMTag()),
    ymm10(10, xRegisterYMMTag()), ymm11(11, xRegisterYMMTag()),
    ymm12(12, xRegisterYMMTag()), ymm13(13, xRegisterYMMTag()),
    ymm14(14, xRegisterYMMTag()), ymm15(15, xRegisterYMMTag());

const xAddressReg
    rax(0), rbx(3),
    rcx(1), rdx(2),
    rsp(4), rbp(5),
    rsi(6), rdi(7),
    r8(8), r9(9),
    r10(10), r11(11),
    r12(12), r13(13),
    r14(14), r15(15);

const xRegister32
    eax(0), ebx(3),
    ecx(1), edx(2),
    esp(4), ebp(5),
    esi(6), edi(7),
    r8d(8), r9d(9),
    r10d(10), r11d(11),
    r12d(12), r13d(13),
    r14d(14), r15d(15);

const xRegister16
    ax(0), bx(3),
    cx(1), dx(2),
    sp(4), bp(5),
    si(6), di(7);

const xRegister8
    al(0),
    dl(2), bl(3),
    ah(4), ch(5),
    dh(6), bh(7),
    spl(4, true), bpl(5, true),
    sil(6, true), dil(7, true),
    r8b(8), r9b(9),
    r10b(10), r11b(11),
    r12b(12), r13b(13),
    r14b(14), r15b(15);

#if defined(_WIN32) && !defined(X86EMITTER_WASM_BACKEND)
const xAddressReg
    arg1reg = rcx,
    arg2reg = rdx,
    arg3reg = r8,
    arg4reg = r9,
    calleeSavedReg1 = rdi,
    calleeSavedReg2 = rsi;

const xRegister32
    arg1regd = ecx,
    arg2regd = edx,
    calleeSavedReg1d = edi,
    calleeSavedReg2d = esi;
#else
const xAddressReg
    arg1reg = rdi,
    arg2reg = rsi,
    arg3reg = rdx,
    arg4reg = rcx,
    calleeSavedReg1 = r12,
    calleeSavedReg2 = r13;

const xRegister32
    arg1regd = edi,
    arg2regd = esi,
    calleeSavedReg1d = r12d,
    calleeSavedReg2d = r13d;
#endif

	// clang-format on

	const xRegisterCL cl;

	const char* const x86_regnames_gpr8[] =
	{
		"al", "cl", "dl", "bl",
		"ah", "ch", "dh", "bh",
		"b8", "b9", "b10", "b11",
		"b12", "b13", "b14", "b15"
	};

	const char* const x86_regnames_gpr16[] =
	{
		"ax", "cx", "dx", "bx",
		"sp", "bp", "si", "di",
		"h8", "h9", "h10", "h11",
		"h12", "h13", "h14", "h15"
	};

	const char* const x86_regnames_gpr32[] =
	{
		"eax", "ecx", "edx", "ebx",
		"esp", "ebp", "esi", "edi",
		"e8", "e9", "e10", "e11",
		"e12", "e13", "e14", "e15"
	};

	const char* const x86_regnames_gpr64[] =
	{
		"rax", "rcx", "rdx", "rbx",
		"rsp", "rbp", "rsi", "rdi",
		"r8", "r9", "r10", "r11",
		"r12", "r13", "r14", "r15"
	};

	const char* const x86_regnames_sse[] =
	{
		"xmm0", "xmm1", "xmm2", "xmm3",
		"xmm4", "xmm5", "xmm6", "xmm7",
		"xmm8", "xmm9", "xmm10", "xmm11",
		"xmm12", "xmm13", "xmm14", "xmm15"
	};

	const char* xRegisterBase::GetName()
	{
		if (Id == xRegId_Invalid)
			return "invalid";
		if (Id == xRegId_Empty)
			return "empty";

		// bad error?  Return a "big" error string.  Might break formatting of register tables
		// but that's the least of your worries if you see this baby.
		if (Id >= (int)iREGCNT_GPR || Id < 0)
			return "!Register index out of range!";

		switch (GetOperandSize())
		{
			case 1:
				return x86_regnames_gpr8[Id];
			case 2:
				return x86_regnames_gpr16[Id];
			case 4:
				return x86_regnames_gpr32[Id];
			case 8:
				return x86_regnames_gpr64[Id];
			case 16:
				return x86_regnames_sse[Id];
		}

		return "oops?";
	}

	// returns the inverted conditional type for this Jcc condition.  Ie, JNS will become JS.
	__fi JccComparisonType xInvertCond(JccComparisonType src)
	{
		pxAssert(src != Jcc_Unknown);
		if (Jcc_Unconditional == src)
			return Jcc_Unconditional;

		// x86 conditionals are clever!  To invert conditional types, just invert the lower bit:
		return (JccComparisonType)((int)src ^ 1);
	}

	// --------------------------------------------------------------------------------------
	//  xRegisterInt  (method implementations)
	// --------------------------------------------------------------------------------------
	xRegisterInt xRegisterInt::MatchSizeTo(xRegisterInt other) const
	{
		return other.GetOperandSize() == 1 ? xRegisterInt(xRegister8(*this)) : xRegisterInt(other.GetOperandSize(), Id);
	}

	// --------------------------------------------------------------------------------------
	//  xAddressReg  (operator overloads)
	// --------------------------------------------------------------------------------------
	xAddressVoid xAddressReg::operator+(const xAddressReg& right) const
	{
		pxAssertMsg(right.Id != -1 || Id != -1, "Uninitialized x86 register.");
		return xAddressVoid(*this, right);
	}

	xAddressVoid xAddressReg::operator+(sptr right) const
	{
		pxAssertMsg(Id != -1, "Uninitialized x86 register.");
		return xAddressVoid(*this, right);
	}

	xAddressVoid xAddressReg::operator+(const void* right) const
	{
		pxAssertMsg(Id != -1, "Uninitialized x86 register.");
		return xAddressVoid(*this, (sptr)right);
	}

	xAddressVoid xAddressReg::operator-(sptr right) const
	{
		pxAssertMsg(Id != -1, "Uninitialized x86 register.");
		return xAddressVoid(*this, -right);
	}

	xAddressVoid xAddressReg::operator-(const void* right) const
	{
		pxAssertMsg(Id != -1, "Uninitialized x86 register.");
		return xAddressVoid(*this, -(sptr)right);
	}

	xAddressVoid xAddressReg::operator*(int factor) const
	{
		pxAssertMsg(Id != -1, "Uninitialized x86 register.");
		return xAddressVoid(xEmptyReg, *this, factor);
	}

	xAddressVoid xAddressReg::operator<<(u32 shift) const
	{
		pxAssertMsg(Id != -1, "Uninitialized x86 register.");
		return xAddressVoid(xEmptyReg, *this, 1 << shift);
	}


	// --------------------------------------------------------------------------------------
	//  xAddressVoid  (method implementations)
	// --------------------------------------------------------------------------------------

	xAddressVoid::xAddressVoid(const xAddressReg& base, const xAddressReg& index, int factor, sptr displacement)
	{
		Base = base;
		Index = index;
		Factor = factor;
		Displacement = displacement;

		pxAssertMsg(base.Id != xRegId_Invalid, "Uninitialized x86 register.");
		pxAssertMsg(index.Id != xRegId_Invalid, "Uninitialized x86 register.");
	}

	xAddressVoid::xAddressVoid(const xAddressReg& index, sptr displacement)
	{
		Base = xEmptyReg;
		Index = index;
		Factor = 0;
		Displacement = displacement;

		pxAssertMsg(index.Id != xRegId_Invalid, "Uninitialized x86 register.");
	}

	xAddressVoid::xAddressVoid(sptr displacement)
	{
		Base = xEmptyReg;
		Index = xEmptyReg;
		Factor = 0;
		Displacement = displacement;
	}

	xAddressVoid::xAddressVoid(const void* displacement)
	{
		Base = xEmptyReg;
		Index = xEmptyReg;
		Factor = 0;
		Displacement = (sptr)displacement;
	}

	xAddressVoid& xAddressVoid::Add(const xAddressReg& src)
	{
		if (src == Index)
		{
			Factor++;
		}
		else if (src == Base)
		{
			// Compound the existing register reference into the Index/Scale pair.
			Base = xEmptyReg;

			if (src == Index)
				Factor++;
			else
			{
				pxAssertMsg(Index.IsEmpty(), "x86Emitter: Only one scaled index register is allowed in an address modifier.");
				Index = src;
				Factor = 2;
			}
		}
		else if (Base.IsEmpty())
			Base = src;
		else if (Index.IsEmpty())
			Index = src;
		else
			pxAssumeMsg(false, "x86Emitter: address modifiers cannot have more than two index registers."); // oops, only 2 regs allowed per ModRm!

		return *this;
	}

	xAddressVoid& xAddressVoid::Add(const xAddressVoid& src)
	{
		Add(src.Base);
		Add(src.Displacement);

		// If the factor is 1, we can just treat index like a base register also.
		if (src.Factor == 1)
		{
			Add(src.Index);
		}
		else if (Index.IsEmpty())
		{
			Index = src.Index;
			Factor = src.Factor;
		}
		else if (Index == src.Index)
		{
			Factor += src.Factor;
		}
		else
			pxAssumeMsg(false, "x86Emitter: address modifiers cannot have more than two index registers."); // oops, only 2 regs allowed per ModRm!

		return *this;
	}

	xIndirectVoid::xIndirectVoid(const xAddressVoid& src)
	{
		Base = src.Base;
		Index = src.Index;
		Scale = src.Factor;
		Displacement = src.Displacement;

		Reduce();
	}

	xIndirectVoid::xIndirectVoid(sptr disp)
	{
		Base = xEmptyReg;
		Index = xEmptyReg;
		Scale = 0;
		Displacement = disp;

		// no reduction necessary :D
	}

	xIndirectVoid::xIndirectVoid(xAddressReg base, xAddressReg index, int scale, sptr displacement)
	{
		Base = base;
		Index = index;
		Scale = scale;
		Displacement = displacement;

		Reduce();
	}

	// Generates a 'reduced' ModSib form, which has valid Base, Index, and Scale values.
	// Necessary because by default ModSib compounds registers into Index when possible.
	//
	// If the ModSib is in illegal form ([Base + Index*5] for example) then an assertion
	// followed by an InvalidParameter Exception will be tossed around in haphazard
	// fashion.
	//
	// Optimization Note: Currently VC does a piss poor job of inlining this, even though
	// constant propagation *should* resove it to little or no code (VC's constprop fails
	// on C++ class initializers).  There is a work around [using array initializers instead]
	// but it's too much trouble for code that isn't performance critical anyway.
	// And, with luck, maybe VC10 will optimize it better and make it a non-issue. :D
	//
	void xIndirectVoid::Reduce()
	{
		if (Index.IsStackPointer())
		{
			// esp cannot be encoded as the index, so move it to the Base, if possible.
			// note: intentionally leave index assigned to esp also (generates correct
			// encoding later, since ESP cannot be encoded 'alone')

			pxAssert(Scale == 0); // esp can't have an index modifier!
			pxAssert(Base.IsEmpty()); // base must be empty or else!

			Base = Index;
			return;
		}

		// If no index reg, then load the base register into the index slot.
		if (Index.IsEmpty())
		{
			Index = Base;
			Scale = 0;
			if (!Base.IsStackPointer()) // prevent ESP from being encoded 'alone'
				Base = xEmptyReg;
			return;
		}

		// The Scale has a series of valid forms, all shown here:

		switch (Scale)
		{
			case 0:
				break;
			case 1:
				Scale = 0;
				break;
			case 2:
				Scale = 1;
				break;

			case 3: // becomes [reg*2+reg]
				pxAssertMsg(Base.IsEmpty(), "Cannot scale an Index register by 3 when Base is not empty!");
				Base = Index;
				Scale = 1;
				break;

			case 4:
				Scale = 2;
				break;

			case 5: // becomes [reg*4+reg]
				pxAssertMsg(Base.IsEmpty(), "Cannot scale an Index register by 5 when Base is not empty!");
				Base = Index;
				Scale = 2;
				break;

			case 6: // invalid!
				pxAssumeMsg(false, "x86 asm cannot scale a register by 6.");
				break;

			case 7: // so invalid!
				pxAssumeMsg(false, "x86 asm cannot scale a register by 7.");
				break;

			case 8:
				Scale = 3;
				break;
			case 9: // becomes [reg*8+reg]
				pxAssertMsg(Base.IsEmpty(), "Cannot scale an Index register by 9 when Base is not empty!");
				Base = Index;
				Scale = 3;
				break;

				jNO_DEFAULT
		}
	}

	xIndirectVoid& xIndirectVoid::Add(sptr imm)
	{
		Displacement += imm;
		return *this;
	}
} // End namespace x86Emitter
