// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

/*
 * ix86 core v0.9.1
 *
 * Original Authors (v0.6.2 and prior):
 *		linuzappz <linuzappz@pcsx.net>
 *		alexey silinov
 *		goldfinger
 *		zerofrog(@gmail.com)
 *
 * Authors of v0.9.1:
 *		Jake.Stine(@gmail.com)
 *		cottonvibes(@gmail.com)
 *		sudonim(1@gmail.com)
 */

#include "common/emitter/internal.h"

namespace x86Emitter
{

	void xImpl_JmpCall::operator()(const xAddressReg& absreg) const
	{
		XTRACE(isJmp ? XOp::JMP : XOp::CALL, 0, absreg);
		// Jumps are always wide and don't need the rex.W
		xOpWrite(0, 0xff, isJmp ? 4 : 2, absreg.GetNonWide());
	}
	void xImpl_JmpCall::operator()(const xIndirectNative& src) const
	{
		XTRACE(isJmp ? XOp::JMP : XOp::CALL, 0, src);
		// Jumps are always wide and don't need the rex.W
		EmitRex(0, xIndirect32(src.Base, src.Index, 1, 0));
		xWrite8(0xff);
		EmitSibMagic(isJmp ? 4 : 2, src);
	}


	// Special form for calling functions.  This form automatically resolves the
	// correct displacement based on the size of the instruction being generated.
	void xImpl_JmpCall::operator()(const void* func) const
	{
		if (isJmp)
		{
			XTRACE(XOp::JMP, 0, xtrace::CodePtr{func});
			xJccKnownTarget(Jcc_Unconditional, (const void*)(uptr)func, false); // double cast to/from (uptr) needed to appease GCC
		}
		else
		{
			XTRACE(XOp::CALL, 0, xtrace::FuncPtr{func});

			// calls are relative to the instruction after this one, and length is
			// always 5 bytes (16 bit calls are bad mojo, so no bother to do special logic).

			sptr dest = (sptr)func - ((sptr)xGetPtr() + 5);
			pxAssertMsg(dest == (s32)dest, "Indirect jump is too far, must use a register!");
			xWrite8(0xe8);
			xWrite32(dest);
		}
	}

	template <typename Reg1, typename Reg2>
	void prepareRegsForFastcall(const Reg1& a1, const Reg2& a2)
	{
		if (a1.IsEmpty())
			return;

		// Make sure we don't mess up if someone tries to fastcall with a1 in arg2reg and a2 in arg1reg
		if (a2.Id != arg1reg.Id)
		{
			xMOV(Reg1(arg1reg), a1);
			if (!a2.IsEmpty())
			{
				xMOV(Reg2(arg2reg), a2);
			}
		}
		else if (a1.Id != arg2reg.Id)
		{
			xMOV(Reg2(arg2reg), a2);
			xMOV(Reg1(arg1reg), a1);
		}
		else
		{
			xPUSH(a1);
			xMOV(Reg2(arg2reg), a2);
			xPOP(Reg1(arg1reg));
		}
	}

	void xImpl_FastCall::operator()(const void* f, const xRegister32& a1, const xRegister32& a2) const
	{
		XTRACE(XOp::FASTCALL, 0, xtrace::FuncPtr{f}, a1, a2);
		prepareRegsForFastcall(a1, a2);
		uptr disp = ((uptr)xGetPtr() + 5) - (uptr)f;
		if ((sptr)disp == (s32)disp)
		{
			xCALL(f);
		}
		else
		{
			xLEA(rax, ptr64[f]);
			xCALL(rax);
		}
	}

	void xImpl_FastCall::operator()(const void* f, const xRegisterLong& a1, const xRegisterLong& a2) const
	{
		XTRACE(XOp::FASTCALL, 0, xtrace::FuncPtr{f}, a1, a2);
		prepareRegsForFastcall(a1, a2);
		uptr disp = ((uptr)xGetPtr() + 5) - (uptr)f;
		if ((sptr)disp == (s32)disp)
		{
			xCALL(f);
		}
		else
		{
			xLEA(rax, ptr64[f]);
			xCALL(rax);
		}
	}

	void xImpl_FastCall::operator()(const void* f, u32 a1, const xRegisterLong& a2) const
	{
		XTRACE(XOp::FASTCALL, 0, xtrace::FuncPtr{f}, a1, a2);
		if (!a2.IsEmpty())
		{
			xMOV(arg2reg, a2);
		}
		xMOV(arg1reg, a1);
		(*this)(f, arg1reg, arg2reg);
	}

	void xImpl_FastCall::operator()(const void* f, void* a1) const
	{
		XTRACE(XOp::FASTCALL, 0, xtrace::FuncPtr{f}, xtrace::Addr{a1});
		xLEA(arg1reg, ptr[a1]);
		(*this)(f, arg1reg, arg2reg);
	}

	void xImpl_FastCall::operator()(const void* f, u32 a1, const xRegister32& a2) const
	{
		XTRACE(XOp::FASTCALL, 0, xtrace::FuncPtr{f}, a1, a2);
		if (!a2.IsEmpty())
		{
			xMOV(arg2regd, a2);
		}
		xMOV(arg1regd, a1);
		(*this)(f, arg1regd, arg2regd);
	}

	void xImpl_FastCall::operator()(const void* f, const xIndirect32& a1) const
	{
		XTRACE(XOp::FASTCALL, 0, xtrace::FuncPtr{f}, a1);
		xMOV(arg1regd, a1);
		(*this)(f, arg1regd);
	}

	void xImpl_FastCall::operator()(const void* f, u32 a1, u32 a2) const
	{
		XTRACE(XOp::FASTCALL, 0, xtrace::FuncPtr{f}, a1, a2);
		xMOV(arg1regd, a1);
		xMOV(arg2regd, a2);
		(*this)(f, arg1regd, arg2regd);
	}

	void xImpl_FastCall::operator()(const xIndirectNative& f, const xRegisterLong& a1, const xRegisterLong& a2) const
	{
		XTRACE(XOp::FASTCALL, 0, f, a1, a2);
		prepareRegsForFastcall(a1, a2);
		xCALL(f);
	}


	// ------------------------------------------------------------------------
	// Emits a 32 bit jump, and returns a pointer to the 32 bit displacement.
	// (displacements should be assigned relative to the end of the jump instruction,
	// or in other words *(retval+1) )
	__emitinline s32* xJcc32(JccComparisonType comparison, s32 displacement)
	{
#ifdef X86EMITTER_XTRACE
		if (xtrace::s_enabled && xtrace::s_depth == 0)
		{
			const s32* slot = reinterpret_cast<const s32*>(xGetPtr() + ((comparison == Jcc_Unconditional) ? 1 : 2));
			const XOperand ops[] = {xtrace::Conv(comparison), [slot]() {
				XOperand op;
				op.kind = static_cast<u8>(XOperandKind::Slot);
				op.index = xtrace::NewSlot(slot);
				return op;
			}()};
			xtrace::Record(XOp::JCC_LINK, 0, ops, 2);
		}
#endif
		if (comparison == Jcc_Unconditional)
			xWrite8(0xe9);
		else
		{
			xWrite8(0x0f);
			xWrite8(0x80 | comparison);
		}
		xWrite<s32>(displacement);

		return ((s32*)xGetPtr()) - 1;
	}

	// ------------------------------------------------------------------------
	// Emits a 32 bit jump, and returns a pointer to the 8 bit displacement.
	// (displacements should be assigned relative to the end of the jump instruction,
	// or in other words *(retval+1) )
	__emitinline s8* xJcc8(JccComparisonType comparison, s8 displacement)
	{
		xWrite8((comparison == Jcc_Unconditional) ? 0xeb : (0x70 | comparison));
		xWrite<s8>(displacement);
		return (s8*)xGetPtr() - 1;
	}

	// ------------------------------------------------------------------------
	// Writes a jump at the current x86Ptr, which targets a pre-established target address.
	// (usually a backwards jump)
	//
	// slideForward - used internally by xSmartJump to indicate that the jump target is going
	// to slide forward in the event of an 8 bit displacement.
	//
	__emitinline void xJccKnownTarget(JccComparisonType comparison, const void* target, bool slideForward)
	{
		// Calculate the potential j8 displacement first, assuming an instruction length of 2:
		sptr displacement8 = (sptr)target - (sptr)(xGetPtr() + 2);

		const int slideVal = slideForward ? ((comparison == Jcc_Unconditional) ? 3 : 4) : 0;
		displacement8 -= slideVal;

		if (slideForward)
		{
			pxAssertMsg(displacement8 >= 0, "Used slideForward on a backward jump; nothing to slide!");
		}

		if (is_s8(displacement8))
			xJcc8(comparison, displacement8);
		else
		{
			// Perform a 32 bit jump instead. :(
			s32* bah = xJcc32(comparison);
			sptr distance = (sptr)target - (sptr)xGetPtr();

			// This assert won't physically happen on x86 targets
			pxAssertMsg(distance >= -0x80000000LL && distance < 0x80000000LL, "Jump target is too far away, needs an indirect register");

			*bah = (s32)distance;
		}
	}

	// Low-level jump instruction!  Specify a comparison type and a target in void* form, and
	// a jump (either 8 or 32 bit) is generated.
	__emitinline void xJcc(JccComparisonType comparison, const void* target)
	{
		XTRACE(XOp::JCC, 0, comparison, xtrace::CodePtr{target});
		xJccKnownTarget(comparison, target, false);
	}

	xForwardJumpBase::xForwardJumpBase(uint opsize, JccComparisonType cctype)
	{
		pxAssert(opsize == 1 || opsize == 4);
		pxAssertMsg(cctype != Jcc_Unknown, "Invalid ForwardJump conditional type.");

		BasePtr = (s8*)xGetPtr() +
				  ((opsize == 1) ? 2 : // j8's are always 2 bytes.
                                   ((cctype == Jcc_Unconditional) ? 5 : 6)); // j32's are either 5 or 6 bytes

#ifdef X86EMITTER_XTRACE
		if (xtrace::s_enabled && xtrace::s_depth == 0)
		{
			const XOperand ops[] = {xtrace::Conv(cctype), xtrace::Conv(xtrace::Label{xtrace::NewLabel(BasePtr)})};
			xtrace::Record(XOp::JCC_FWD, 0, ops, 2);
		}
#endif

		if (opsize == 1)
			xWrite8((cctype == Jcc_Unconditional) ? 0xeb : (0x70 | cctype));
		else
		{
			if (cctype == Jcc_Unconditional)
				xWrite8(0xe9);
			else
			{
				xWrite8(0x0f);
				xWrite8(0x80 | cctype);
			}
		}

		xAdvancePtr(opsize);
	}

	void xForwardJumpBase::_setTarget(uint opsize) const
	{
		pxAssertMsg(BasePtr != NULL, "");

#ifdef X86EMITTER_XTRACE
		if (xtrace::s_enabled && xtrace::s_depth == 0)
		{
			const XOperand ops[] = {xtrace::Conv(xtrace::Label{xtrace::FindLabel(BasePtr)})};
			xtrace::Record(XOp::LABEL, 0, ops, 1);
		}
#endif

		sptr displacement = (sptr)xGetPtr() - (sptr)BasePtr;
		if (opsize == 1)
		{
			pxAssertMsg(is_s8(displacement), "Emitter Error: Invalid short jump displacement.");
			BasePtr[-1] = (s8)displacement;
		}
		else
		{
			// full displacement, no sanity checks needed :D
			((s32*)BasePtr)[-1] = displacement;
		}
	}

} // namespace x86Emitter
