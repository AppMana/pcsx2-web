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

	// =====================================================================================================
	//  Group 1 Instructions - ADD, SUB, ADC, etc.
	// =====================================================================================================

	// Note on "[Indirect],Imm" forms : use int as the source operand since it's "reasonably inert" from a
	// compiler perspective.  (using uint tends to make the compiler try and fail to match signed immediates
	// with one of the other overloads).
	void xImpl_Group1::operator()(const xIndirect64orLess& sibdest, int imm) const
	{
		if (sibdest.Is8BitOp())
		{
			xOpWrite(sibdest.GetPrefix16(), 0x80, InstType, sibdest, 1);

			xWrite<s8>(imm);
		}
		else
		{
			u8 opcode = is_s8(imm) ? 0x83 : 0x81;
			xOpWrite(sibdest.GetPrefix16(), opcode, InstType, sibdest, is_s8(imm) ? 1 : sibdest.GetImmSize());

			if (is_s8(imm))
				xWrite<s8>(imm);
			else
				sibdest.xWriteImm(imm);
		}
	}

	void xImpl_Group1::operator()(const xRegisterInt& to, const xRegisterInt& from) const
	{
		pxAssert(to.GetOperandSize() == from.GetOperandSize());

		u8 opcode = (to.Is8BitOp() ? 0 : 1) | (InstType << 3);
		xOpWrite(to.GetPrefix16(), opcode, from, to);
	}

	void xImpl_Group1::operator()(const xIndirectVoid& sibdest, const xRegisterInt& from) const
	{
		u8 opcode = (from.Is8BitOp() ? 0 : 1) | (InstType << 3);
		xOpWrite(from.GetPrefix16(), opcode, from, sibdest);
	}

	void xImpl_Group1::operator()(const xRegisterInt& to, const xIndirectVoid& sibsrc) const
	{
		u8 opcode = (to.Is8BitOp() ? 2 : 3) | (InstType << 3);
		xOpWrite(to.GetPrefix16(), opcode, to, sibsrc);
	}

	void xImpl_Group1::operator()(const xRegisterInt& to, int imm) const
	{
		if (!to.Is8BitOp() && is_s8(imm))
		{
			xOpWrite(to.GetPrefix16(), 0x83, InstType, to);
			xWrite<s8>(imm);
		}
		else
		{
			if (to.IsAccumulator())
			{
				u8 opcode = (to.Is8BitOp() ? 4 : 5) | (InstType << 3);
				xOpAccWrite(to.GetPrefix16(), opcode, InstType, to);
			}
			else
			{
				u8 opcode = to.Is8BitOp() ? 0x80 : 0x81;
				xOpWrite(to.GetPrefix16(), opcode, InstType, to);
			}
			to.xWriteImm(imm);
		}
	}


	// =====================================================================================================
	//  Group 2 Instructions - SHR, SHL, etc.
	// =====================================================================================================

	void xImpl_Group2::operator()(const xRegisterInt& to, const xRegisterCL& /* from */) const
	{
		xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0xd2 : 0xd3, InstType, to);
	}

	void xImpl_Group2::operator()(const xRegisterInt& to, u8 imm) const
	{
		if (imm == 0)
			return;

		if (imm == 1)
		{
			// special encoding of 1's
			xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0xd0 : 0xd1, InstType, to);
		}
		else
		{
			xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0xc0 : 0xc1, InstType, to);
			xWrite8(imm);
		}
	}

	void xImpl_Group2::operator()(const xIndirect64orLess& sibdest, const xRegisterCL& /* from */) const
	{
		xOpWrite(sibdest.GetPrefix16(), sibdest.Is8BitOp() ? 0xd2 : 0xd3, InstType, sibdest);
	}

	void xImpl_Group2::operator()(const xIndirect64orLess& sibdest, u8 imm) const
	{
		if (imm == 0)
			return;

		if (imm == 1)
		{
			// special encoding of 1's
			xOpWrite(sibdest.GetPrefix16(), sibdest.Is8BitOp() ? 0xd0 : 0xd1, InstType, sibdest);
		}
		else
		{
			xOpWrite(sibdest.GetPrefix16(), sibdest.Is8BitOp() ? 0xc0 : 0xc1, InstType, sibdest, 1);
			xWrite8(imm);
		}
	}



	// =====================================================================================================
	//  Group 3 Instructions - NOT, NEG, MUL, DIV
	// =====================================================================================================

	static void _g3_EmitOp(G3Type InstType, const xRegisterInt& from)
	{
		xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0xf6 : 0xf7, InstType, from);
	}

	static void _g3_EmitOp(G3Type InstType, const xIndirect64orLess& from)
	{
		xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0xf6 : 0xf7, InstType, from);
	}

	void xImpl_Group3::operator()(const xRegisterInt& from) const
	{
		xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0xf6 : 0xf7, InstType, from);
	}

	void xImpl_Group3::operator()(const xIndirect64orLess& from) const
	{
		xOpWrite(from.GetPrefix16(), from.Is8BitOp() ? 0xf6 : 0xf7, InstType, from);
	}

	template <typename SrcType>
	static void _imul_ImmStyle(const xRegisterInt& param1, const SrcType& param2, int imm)
	{
		pxAssert(param1.GetOperandSize() == param2.GetOperandSize());

		xOpWrite0F(param1.GetPrefix16(), is_s8(imm) ? 0x6b : 0x69, param1, param2, is_s8(imm) ? 1 : param1.GetImmSize());

		if (is_s8(imm))
			xWrite8((u8)imm);
		else
			param1.xWriteImm(imm);
	}

	void xImpl_iMul::operator()(const xRegister32& to, const xRegister32& from) const { xOpWrite0F(0xaf, to, from); }
	void xImpl_iMul::operator()(const xRegister32& to, const xIndirectVoid& src) const { xOpWrite0F(0xaf, to, src); }
	void xImpl_iMul::operator()(const xRegister16& to, const xRegister16& from) const { xOpWrite0F(0x66, 0xaf, to, from); }
	void xImpl_iMul::operator()(const xRegister16& to, const xIndirectVoid& src) const { xOpWrite0F(0x66, 0xaf, to, src); }

	void xImpl_iMul::operator()(const xRegister32& to, const xRegister32& from, s32 imm) const { _imul_ImmStyle(to, from, imm); }
	void xImpl_iMul::operator()(const xRegister32& to, const xIndirectVoid& from, s32 imm) const { _imul_ImmStyle(to, from, imm); }
	void xImpl_iMul::operator()(const xRegister16& to, const xRegister16& from, s16 imm) const { _imul_ImmStyle(to, from, imm); }
	void xImpl_iMul::operator()(const xRegister16& to, const xIndirectVoid& from, s16 imm) const { _imul_ImmStyle(to, from, imm); }


	// =====================================================================================================
	//  Group 8 Instructions
	// =====================================================================================================

	void xImpl_Group8::operator()(const xRegister16or32or64& bitbase, const xRegister16or32or64& bitoffset) const
	{
		pxAssert(bitbase->GetOperandSize() == bitoffset->GetOperandSize());
		xOpWrite0F(bitbase->GetPrefix16(), 0xa3 | (InstType << 3), bitbase, bitoffset);
	}
	void xImpl_Group8::operator()(const xIndirect64& bitbase, u8 bitoffset) const { xOpWrite0F(0xba, InstType, bitbase, bitoffset); }
	void xImpl_Group8::operator()(const xIndirect32& bitbase, u8 bitoffset) const { xOpWrite0F(0xba, InstType, bitbase, bitoffset); }
	void xImpl_Group8::operator()(const xIndirect16& bitbase, u8 bitoffset) const { xOpWrite0F(0x66, 0xba, InstType, bitbase, bitoffset); }

	void xImpl_Group8::operator()(const xRegister16or32or64& bitbase, u8 bitoffset) const
	{
		xOpWrite0F(bitbase->GetPrefix16(), 0xba, InstType, bitbase, bitoffset);
	}

	void xImpl_Group8::operator()(const xIndirectVoid& bitbase, const xRegister16or32or64& bitoffset) const
	{
		xOpWrite0F(bitoffset->GetPrefix16(), 0xa3 | (InstType << 3), bitoffset, bitbase);
	}




} // End namespace x86Emitter
