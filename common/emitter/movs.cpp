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
#include "common/emitter/implement/helpers.h"

namespace x86Emitter
{

	void xImpl_Mov::operator()(const xRegisterInt& to, const xRegisterInt& from) const
	{
		XTRACE(XOp::MOV, 0, to, from);
		pxAssert(to.GetOperandSize() == from.GetOperandSize());

		if (to == from)
			return; // ignore redundant MOVs.

		xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0x88 : 0x89, from, to);
	}

	void xImpl_Mov::operator()(const xIndirectVoid& dest, const xRegisterInt& from) const
	{
		XTRACE(XOp::MOV, 0, dest, from);
		// mov eax has a special from when writing directly to a DISP32 address
		// (sans any register index/base registers).

		xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0x88 : 0x89, from, dest);
	}

	void xImpl_Mov::operator()(const xRegisterInt& to, const xIndirectVoid& src) const
	{
		XTRACE(XOp::MOV, 0, to, src);
		// mov eax has a special from when reading directly from a DISP32 address
		// (sans any register index/base registers).

		xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0x8a : 0x8b, to, src);
	}

	void xImpl_Mov::operator()(const xIndirect64orLess& dest, sptr imm) const
	{
		XTRACE(XOp::MOV, 0, dest, imm);
		switch (dest.GetOperandSize())
		{
			case 1:
				pxAssertMsg(imm == (s8)imm || imm == (u8)imm, "Immediate won't fit!");
				break;
			case 2:
				pxAssertMsg(imm == (s16)imm || imm == (u16)imm, "Immediate won't fit!");
				break;
			case 4:
				pxAssertMsg(imm == (s32)imm || imm == (u32)imm, "Immediate won't fit!");
				break;
			case 8:
				pxAssertMsg(imm == (s32)imm, "Immediate won't fit in immediate slot, go through a register!");
				break;
			default:
				pxAssertMsg(0, "Bad indirect size!");
		}
		xOpWrite(dest.GetPrefix16(), dest.Is8BitOp() ? 0xc6 : 0xc7, 0, dest, dest.GetImmSize());
		dest.xWriteImm(imm);
	}

	// preserve_flags  - set to true to disable optimizations which could alter the state of
	//   the flags (namely replacing mov reg,0 with xor).
	void xImpl_Mov::operator()(const xRegisterInt& to, sptr imm, bool preserve_flags) const
	{
		XTRACE(XOp::MOV, preserve_flags ? XRecord::PreserveFlags : 0, to, imm);
		switch (to.GetOperandSize())
		{
			case 1:
				pxAssertMsg(imm == (s8)imm || imm == (u8)imm, "Immediate won't fit!");
				break;
			case 2:
				pxAssertMsg(imm == (s16)imm || imm == (u16)imm, "Immediate won't fit!");
				break;
			case 4:
				pxAssertMsg(imm == (s32)imm || imm == (u32)imm, "Immediate won't fit!");
				break;
			case 8:
				pxAssertMsg(imm == (s32)imm || imm == (u32)imm, "Immediate won't fit in immediate slot, use mov64 or lea!");
				break;
			default:
				pxAssertMsg(0, "Bad indirect size!");
		}
		const xRegisterInt& to_ = to.GetNonWide();
		if (!preserve_flags && (imm == 0))
		{
			xXOR(to_, to_);
		}
		else if (imm == (sptr)(u32)imm || !to.IsWide())
		{
			// Note: MOV does not have (reg16/32,imm8) forms.
			u8 opcode = (to_.Is8BitOp() ? 0xb0 : 0xb8) | to_.Id;
			xOpAccWrite(to_.GetPrefix16(), opcode, 0, to_);
			to_.xWriteImm(imm);
		}
		else
		{
			xOpWrite(to.GetPrefix16(), 0xc7, 0, to);
			to.xWriteImm(imm);
		}
	}


	void xImpl_MovImm64::operator()(const xRegister64& to, s64 imm, bool preserve_flags) const
	{
		XTRACE(XOp::MOV64, preserve_flags ? XRecord::PreserveFlags : 0, to, imm);
		if (imm == (u32)imm || imm == (s32)imm)
		{
			xMOV(to, imm, preserve_flags);
		}
		else
		{
			u8 opcode = 0xb8 | to.Id;
			xOpAccWrite(to.GetPrefix16(), opcode, 0, to);
			xWrite64(imm);
		}
	}


	// --------------------------------------------------------------------------------------
	//  CMOVcc
	// --------------------------------------------------------------------------------------

#define ccSane() pxAssertMsg(ccType >= 0 && ccType <= 0x0f, "Invalid comparison type specifier.")

// Macro useful for trapping unwanted use of EBP.
//#define EbpAssert() pxAssert( to != ebp )
#define EbpAssert()



	void xImpl_CMov::operator()(const xRegister16or32or64& to, const xRegister16or32or64& from) const
	{
		XTRACE(XOp::CMOV, 0, ccType, to, from);
		pxAssert(to->GetOperandSize() == from->GetOperandSize());
		ccSane();
		xOpWrite0F(to->GetPrefix16(), 0x40 | ccType, to, from);
	}

	void xImpl_CMov::operator()(const xRegister16or32or64& to, const xIndirectVoid& sibsrc) const
	{
		XTRACE(XOp::CMOV, 0, ccType, to, sibsrc);
		ccSane();
		xOpWrite0F(to->GetPrefix16(), 0x40 | ccType, to, sibsrc);
	}

	//void xImpl_CMov::operator()( const xDirectOrIndirect32& to, const xDirectOrIndirect32& from ) const { ccSane(); _DoI_helpermess( *this, to, from ); }
	//void xImpl_CMov::operator()( const xDirectOrIndirect16& to, const xDirectOrIndirect16& from ) const { ccSane(); _DoI_helpermess( *this, to, from ); }

	void xImpl_Set::operator()(const xRegister8& to) const
	{
		XTRACE(XOp::SETCC, 0, ccType, to);
		ccSane();
		xOpWrite0F(0x90 | ccType, 0, to);
	}
	void xImpl_Set::operator()(const xIndirect8& dest) const
	{
		XTRACE(XOp::SETCC, 0, ccType, dest);
		ccSane();
		xOpWrite0F(0x90 | ccType, 0, dest);
	}
	//void xImpl_Set::operator()( const xDirectOrIndirect8& dest ) const		{ ccSane(); _DoI_helpermess( *this, dest ); }

	void xImpl_MovExtend::operator()(const xRegister16or32or64& to, const xRegister8& from) const
	{
		XTRACE(SignExtend ? XOp::MOVSX : XOp::MOVZX, 0, to, from);
		EbpAssert();
		xOpWrite0F(
			(to->GetOperandSize() == 2) ? 0x66 : 0,
			SignExtend ? 0xbe : 0xb6,
			to, from);
	}

	void xImpl_MovExtend::operator()(const xRegister16or32or64& to, const xIndirect8& sibsrc) const
	{
		XTRACE(SignExtend ? XOp::MOVSX : XOp::MOVZX, 0, to, sibsrc);
		EbpAssert();
		xOpWrite0F(
			(to->GetOperandSize() == 2) ? 0x66 : 0,
			SignExtend ? 0xbe : 0xb6,
			to, sibsrc);
	}

	void xImpl_MovExtend::operator()(const xRegister32or64& to, const xRegister16& from) const
	{
		XTRACE(SignExtend ? XOp::MOVSX : XOp::MOVZX, 0, to, from);
		EbpAssert();
		xOpWrite0F(SignExtend ? 0xbf : 0xb7, to, from);
	}

	void xImpl_MovExtend::operator()(const xRegister32or64& to, const xIndirect16& sibsrc) const
	{
		XTRACE(SignExtend ? XOp::MOVSX : XOp::MOVZX, 0, to, sibsrc);
		EbpAssert();
		xOpWrite0F(SignExtend ? 0xbf : 0xb7, to, sibsrc);
	}

	void xImpl_MovExtend::operator()(const xRegister64& to, const xRegister32& from) const
	{
		XTRACE(SignExtend ? XOp::MOVSX : XOp::MOVZX, 0, to, from);
		EbpAssert();
		pxAssertMsg(SignExtend, "Use mov for 64-bit movzx");
		xOpWrite(0, 0x63, to, from);
	}

	void xImpl_MovExtend::operator()(const xRegister64& to, const xIndirect32& sibsrc) const
	{
		XTRACE(SignExtend ? XOp::MOVSX : XOp::MOVZX, 0, to, sibsrc);
		EbpAssert();
		pxAssertMsg(SignExtend, "Use mov for 64-bit movzx");
		xOpWrite(0, 0x63, to, sibsrc);
	}


} // end namespace x86Emitter
