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
#include <functional>

// ------------------------------------------------------------------------
// Notes on Thread Local Storage:
//  * TLS is pretty simple, and "just works" from a programmer perspective, with only
//    some minor additional computational overhead (see performance notes below).
//
//  * MSVC and GCC handle TLS differently internally, but behavior to the programmer is
//    generally identical.
//
// Performance Considerations:
//  * GCC's implementation involves an extra dereference from normal storage (possibly
//    applies to x86-32 only -- x86-64 is untested).
//
//  * MSVC's implementation involves *two* extra dereferences from normal storage because
//    it has to look up the TLS heap pointer from the Windows Thread Storage Area.  (in
//    generated ASM code, this dereference is denoted by access to the fs:[2ch] address),
//
//  * However, in either case, the optimizer usually optimizes it to a register so the
//    extra overhead is minimal over a series of instructions.
//
// MSVC Notes:
//  * Important!! the Full Optimization [/Ox] option effectively disables TLS optimizations
//    in MSVC 2008 and earlier, causing generally significant code bloat.  Not tested in
//    VC2010 yet.
//
//  * VC2010 generally does a superior job of optimizing TLS across inlined functions and
//    class methods, compared to predecessors.
//


static thread_local u8* xTextPtr;

bool x86Emitter::use_avx;

namespace x86Emitter
{
	thread_local u8* x86Ptr;

	template void xWrite<u8>(u8 val);
	template void xWrite<u16>(u16 val);
	template void xWrite<u32>(u32 val);
	template void xWrite<u64>(u64 val);
	template void xWrite<u128>(u128 val);

	__fi void xWrite8(u8 val)
	{
		xWrite(val);
	}

	__fi void xWrite16(u16 val)
	{
		xWrite(val);
	}

	__fi void xWrite32(u32 val)
	{
		xWrite(val);
	}

	__fi void xWrite64(u64 val)
	{
		xWrite(val);
	}



	//////////////////////////////////////////////////////////////////////////////////////////
	// Performance note: VC++ wants to use byte/word register form for the following
	// ModRM/SibSB constructors when we use xWrite<u8>, and furthermore unrolls the
	// the shift using a series of ADDs for the following results:
	//   add cl,cl
	//   add cl,cl
	//   add cl,cl
	//   or  cl,bl
	//   add cl,cl
	//  ... etc.
	//
	// This is unquestionably bad optimization by Core2 standard, an generates tons of
	// register aliases and false dependencies. (although may have been ideal for early-
	// brand P4s with a broken barrel shifter?).  The workaround is to do our own manual
	// x86Ptr access and update using a u32 instead of u8.  Thanks to little endianness,
	// the same end result is achieved and no false dependencies are generated.  The draw-
	// back is that it clobbers 3 bytes past the end of the write, which could cause a
	// headache for someone who himself is doing some kind of headache-inducing amount of
	// recompiler SMC.  So we don't do a work-around, and just hope for the compiler to
	// stop sucking someday instead. :)
	//
	// (btw, I know this isn't a critical performance item by any means, but it's
	//  annoying simply because it *should* be an easy thing to optimize)

	static __fi void ModRM(uint mod, uint reg, uint rm)
	{
		xWrite8((mod << 6) | (reg << 3) | rm);
	}

	static __fi void SibSB(u32 ss, u32 index, u32 base)
	{
		xWrite8((ss << 6) | (index << 3) | base);
	}

	void EmitSibMagic(uint regfield, const void* address, int extraRIPOffset)
	{
		sptr displacement = (sptr)address;
		sptr textRelative = (sptr)address - (sptr)xTextPtr;
		sptr ripRelative = (sptr)address - ((sptr)x86Ptr + sizeof(s8) + sizeof(s32) + extraRIPOffset);
		// Can we use an 8-bit offset from the text pointer?
		if (textRelative == (s8)textRelative && xTextPtr)
		{
			ModRM(1, regfield, RTEXTPTR.GetId());
			xWrite<s8>((s8)textRelative);
			return;
		}
		// Can we use a rip-relative address?  (Prefer this over eiz because it's a byte shorter)
		else if (ripRelative == (s32)ripRelative)
		{
			ModRM(0, regfield, ModRm_UseDisp32);
			displacement = ripRelative;
		}
		// How about from the text pointer?
		else if (textRelative == (s32)textRelative && xTextPtr)
		{
			ModRM(2, regfield, RTEXTPTR.GetId());
			displacement = textRelative;
		}
		else
		{
			pxAssertMsg(displacement == (s32)displacement, "SIB target is too far away, needs an indirect register");
			ModRM(0, regfield, ModRm_UseSib);
			SibSB(0, Sib_EIZ, Sib_UseDisp32);
		}

		xWrite<s32>((s32)displacement);
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	// returns TRUE if this instruction requires SIB to be encoded, or FALSE if the
	// instruction ca be encoded as ModRm alone.
	static __fi bool NeedsSibMagic(const xIndirectVoid& info)
	{
		// no registers? no sibs!
		// (xIndirectVoid::Reduce always places a register in Index, and optionally leaves
		// Base empty if only register is specified)
		if (info.Index.IsEmpty())
			return false;

		// A scaled register needs a SIB
		if (info.Scale != 0)
			return true;

		// two registers needs a SIB
		if (!info.Base.IsEmpty())
			return true;

		return false;
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	// Conditionally generates Sib encoding information!
	//
	// regfield - register field to be written to the ModRm.  This is either a register specifier
	//   or an opcode extension.  In either case, the instruction determines the value for us.
	//
	void EmitSibMagic(uint regfield, const xIndirectVoid& info, int extraRIPOffset)
	{
		// 3 bits also on x86_64 (so max is 8)
		// We might need to mask it on x86_64
		pxAssertMsg(regfield < 8, "Invalid x86 register identifier.");
		int displacement_size = (info.Displacement == 0) ? 0 :
                                                           ((info.IsByteSizeDisp()) ? 1 : 2);

		pxAssert(!info.Base.IsEmpty() || !info.Index.IsEmpty() || displacement_size == 2);
		// Displacement is only 64 bits for rip-relative addressing
		pxAssert(info.Displacement == (s32)info.Displacement || (info.Base.IsEmpty() && info.Index.IsEmpty()));

		if (!NeedsSibMagic(info))
		{
			// Use ModRm-only encoding, with the rm field holding an index/base register, if
			// one has been specified.  If neither register is specified then use Disp32 form,
			// which is encoded as "EBP w/o displacement" (which is why EBP must always be
			// encoded *with* a displacement of 0, if it would otherwise not have one).

			if (info.Index.IsEmpty())
			{
				EmitSibMagic(regfield, (void*)info.Displacement, extraRIPOffset);
				return;
			}
			else
			{
				if (info.Index == rbp && displacement_size == 0)
					displacement_size = 1; // forces [ebp] to be encoded as [ebp+0]!

				ModRM(displacement_size, regfield, info.Index.Id & 7);
			}
		}
		else
		{
			// In order to encode "just" index*scale (and no base), we have to encode
			// it as a special [index*scale + displacement] form, which is done by
			// specifying EBP as the base register and setting the displacement field
			// to zero. (same as ModRm w/o SIB form above, basically, except the
			// ModRm_UseDisp flag is specified in the SIB instead of the ModRM field).

			if (info.Base.IsEmpty())
			{
				ModRM(0, regfield, ModRm_UseSib);
				SibSB(info.Scale, info.Index.Id, Sib_UseDisp32);
				xWrite<s32>(info.Displacement);
				return;
			}
			else
			{
				if (info.Base == rbp && displacement_size == 0)
					displacement_size = 1; // forces [ebp] to be encoded as [ebp+0]!

				ModRM(displacement_size, regfield, ModRm_UseSib);
				SibSB(info.Scale, info.Index.Id & 7, info.Base.Id & 7);
			}
		}

		if (displacement_size != 0)
		{
			if (displacement_size == 1)
				xWrite<s8>(info.Displacement);
			else
				xWrite<s32>(info.Displacement);
		}
	}

	// Writes a ModRM byte for "Direct" register access forms, which is used for all
	// instructions taking a form of [reg,reg].
	void EmitSibMagic(uint reg1, const xRegisterBase& reg2, int)
	{
		xWrite8((Mod_Direct << 6) | (reg1 << 3) | (reg2.Id & 7));
	}

	void EmitSibMagic(const xRegisterBase& reg1, const xRegisterBase& reg2, int)
	{
		xWrite8((Mod_Direct << 6) | ((reg1.Id & 7) << 3) | (reg2.Id & 7));
	}

	void EmitSibMagic(const xRegisterBase& reg1, const void* src, int extraRIPOffset)
	{
		EmitSibMagic(reg1.Id & 7, src, extraRIPOffset);
	}

	void EmitSibMagic(const xRegisterBase& reg1, const xIndirectVoid& sib, int extraRIPOffset)
	{
		EmitSibMagic(reg1.Id & 7, sib, extraRIPOffset);
	}

	//////////////////////////////////////////////////////////////////////////////////////////
	__emitinline static void EmitRex(bool w, bool r, bool x, bool b, bool ext8bit = false)
	{
		const u8 rex = 0x40 | (w << 3) | (r << 2) | (x << 1) | (u8)b;
		if (rex != 0x40 || ext8bit)
			xWrite8(rex);
	}

	void EmitRex(uint regfield, const void* address)
	{
		pxAssert(0);
		bool w = false;
		bool r = false;
		bool x = false;
		bool b = false;
		EmitRex(w, r, x, b);
	}

	void EmitRex(uint regfield, const xIndirectVoid& info)
	{
		bool w = info.IsWide();
		bool r = false;
		bool x = info.Index.IsExtended();
		bool b = info.Base.IsExtended();
		if (!NeedsSibMagic(info))
		{
			b = x;
			x = false;
		}
		EmitRex(w, r, x, b);
	}

	void EmitRex(uint reg1, const xRegisterBase& reg2)
	{
		bool w = reg2.IsWide();
		bool r = false;
		bool x = false;
		bool b = reg2.IsExtended();
		EmitRex(w, r, x, b, reg2.IsExtended8Bit());
	}

	void EmitRex(const xRegisterBase& reg1, const xRegisterBase& reg2)
	{
		bool w = reg1.IsWide() || reg2.IsWide();
		bool r = reg1.IsExtended();
		bool x = false;
		bool b = reg2.IsExtended();
		EmitRex(w, r, x, b, reg2.IsExtended8Bit());
	}

	void EmitRex(const xRegisterBase& reg1, const void* src)
	{
		pxAssert(0); //see fixme
		bool w = reg1.IsWide();
		bool r = reg1.IsExtended();
		bool x = false;
		bool b = false; // FIXME src.IsExtended();
		EmitRex(w, r, x, b, reg1.IsExtended8Bit());
	}

	void EmitRex(const xRegisterBase& reg1, const xIndirectVoid& sib)
	{
		bool w = reg1.IsWide() || sib.IsWide();
		bool r = reg1.IsExtended();
		bool x = sib.Index.IsExtended();
		bool b = sib.Base.IsExtended();
		if (!NeedsSibMagic(sib))
		{
			b = x;
			x = false;
		}
		EmitRex(w, r, x, b, reg1.IsExtended8Bit());
	}

	void EmitRex(SIMDInstructionInfo info, const xRegisterBase& reg1, const xRegisterBase& reg2)
	{
		bool w = false;
		if (info.dst_w)
			w |= reg1.IsWide();
		if (info.src_w)
			w |= reg2.IsWide();
		bool r = reg1.IsExtended();
		bool x = false;
		bool b = reg2.IsExtended();
		EmitRex(w, r, x, b, reg2.IsExtended8Bit());
	}

	void EmitRex(SIMDInstructionInfo info, const xRegisterBase& reg1, const xIndirectVoid& sib)
	{
		bool w = false;
		if (info.dst_w)
			w |= reg1.IsWide();
		if (info.src_w)
			w |= sib.IsWide();
		bool r = reg1.IsExtended();
		bool x = sib.Index.IsExtended();
		bool b = sib.Base.IsExtended();
		if (!NeedsSibMagic(sib))
		{
			b = x;
			x = false;
		}
		EmitRex(w, r, x, b, reg1.IsExtended8Bit());
	}

	void EmitRex(SIMDInstructionInfo info, uint reg1, const xRegisterBase& reg2)
	{
		bool w = info.src_w ? reg2.IsWide() : false;
		bool r = false;
		bool x = false;
		bool b = reg2.IsExtended();
		EmitRex(w, r, x, b, reg2.IsExtended8Bit());
	}

	// For use by instructions that are implicitly wide
	void EmitRexImplicitlyWide(const xRegisterBase& reg)
	{
		bool w = false;
		bool r = false;
		bool x = false;
		bool b = reg.IsExtended();
		EmitRex(w, r, x, b);
	}

	void EmitRexImplicitlyWide(const xIndirectVoid& sib)
	{
		bool w = false;
		bool r = false;
		bool x = sib.Index.IsExtended();
		bool b = sib.Base.IsExtended();
		if (!NeedsSibMagic(sib))
		{
			b = x;
			x = false;
		}
		EmitRex(w, r, x, b);
	}

	__emitinline static u8 GetVEXRXB(u32 ext, const xRegisterBase& src2)
	{
		return src2.IsExtended() << 5;
	}

	__emitinline static u8 GetVEXRXB(const xRegisterBase& dst, const xIndirectVoid& src2)
	{
		bool r = dst.IsExtended();
		bool x = src2.Index.IsExtended();
		bool b = src2.Base.IsExtended();
		if (!NeedsSibMagic(src2))
		{
			b = x;
			x = false;
		}
		return (r << 7) | (x << 6) | (b << 5);
	}

	__emitinline static u8 GetVEXRXB(const xRegisterBase& dst, const xRegisterBase& src2)
	{
		return (dst.IsExtended() << 7) | (src2.IsExtended() << 5);
	}

	__emitinline static u8 GetL(const xRegisterBase& arg) { return arg.IsWideSIMD() ? 4 : 0; }
	__emitinline static u8 GetL(const xIndirectVoid& arg) { return 0; }
	__emitinline static u8 GetL(u32 ext) { return 0; }

	__emitinline static u8 GetVEXW(const xRegisterBase& arg) { return arg.GetOperandSize() == 8 ? 0x80 : 0; }
	__emitinline static u8 GetVEXW(const xIndirectVoid& arg) { return arg.GetOperandSize() == 8 ? 0x80 : 0; }
	__emitinline static u8 GetVEXW(u32 ext) { return 0; }

	template <typename D, typename S2>
	__emitinline void xOpWriteVEX(SIMDInstructionInfo info, D dst, u8 src1, const S2& src2, int extraRipOffset)
	{
		u8 m = static_cast<u8>(info.map);
		u8 p = static_cast<u8>(info.prefix);
		u8 w = 0;
		if (info.src_w || info.dst_w) {
			if (info.dst_w)
				w |= GetVEXW(dst);
			if (info.src_w)
				w |= GetVEXW(src2);
		} else {
			w = info.w_bit << 7;
		}
		u8 l = GetL(dst) | GetL(src2); // Needed for 256-bit movemask.
		u8 rxb = GetVEXRXB(dst, src2);
		u8 b2 = p | l | (src1 << 3);
		if (!w && info.map == SIMDInstructionInfo::Map::M0F && !(rxb & 0x7F))
		{
			// Can use a C5 VEX
			u8 b1 = rxb | b2;
			xWrite8(0xC5);
			xWrite8(b1 ^ 0xF8);
			xWrite8(info.opcode);
		}
		else
		{
			u8 b1 = rxb | m;
			b2 |= w;
			xWrite8(0xC4);
			xWrite8(b1 ^ 0xE0);
			xWrite8(b2 ^ 0x78);
			xWrite8(info.opcode);
		}
		EmitSibMagic(dst, src2, extraRipOffset);
	}

	void EmitVEX(SIMDInstructionInfo info, const xRegisterBase& dst, u8 src1, const xRegisterBase& src2, int extraRipOffset)
	{
		xOpWriteVEX(info, dst, src1, src2, extraRipOffset);
	}

	void EmitVEX(SIMDInstructionInfo info, const xRegisterBase& dst, u8 src1, const xIndirectVoid& src2, int extraRipOffset)
	{
		xOpWriteVEX(info, dst, src1, src2, extraRipOffset);
	}

	void EmitVEX(SIMDInstructionInfo info, u32 ext, u8 dst, const xRegisterBase& src2, int extraRipOffset)
	{
		xOpWriteVEX(info, ext, dst, src2, extraRipOffset);
	}


	// --------------------------------------------------------------------------------------
	//  xSetPtr / xAlignPtr / xGetPtr / xAdvancePtr
	// --------------------------------------------------------------------------------------

	// Assigns the current emitter buffer target address.
	// This is provided instead of using x86Ptr directly, since we may in the future find
	// a need to change the storage class system for the x86Ptr 'under the hood.'
	__emitinline void xSetPtr(void* ptr)
	{
		x86Ptr = (u8*)ptr;
	}

	// Assigns the current emitter text base address.
	__emitinline void xSetTextPtr(void* ptr)
	{
		xTextPtr = (u8*)ptr;
	}

	// Retrieves the current emitter buffer target address.
	// This is provided instead of using x86Ptr directly, since we may in the future find
	// a need to change the storage class system for the x86Ptr 'under the hood.'
	__emitinline u8* xGetPtr()
	{
		return x86Ptr;
	}

	// Retrieves the current emitter text base address.
	__emitinline u8* xGetTextPtr()
	{
		return xTextPtr;
	}

	__emitinline void xAlignPtr(uint bytes)
	{
		// forward align
		x86Ptr = (u8*)(((uptr)x86Ptr + bytes - 1) & ~(uptr)(bytes - 1));
	}

	// Performs best-case alignment for the target CPU, for use prior to starting a new
	// function.  This is not meant to be used prior to jump targets, since it doesn't
	// add padding (additionally, speed benefit from jump alignment is minimal, and often
	// a loss).
	__emitinline void xAlignCallTarget()
	{
		// Core2/i7 CPUs prefer unaligned addresses.  Checking for SSSE3 is a decent filter.
		// (also align in debug modes for disasm convenience)

		if constexpr (IsDebugBuild)
		{
			// - P4's and earlier prefer 16 byte alignment.
			// - AMD Athlons and Phenoms prefer 8 byte alignment, but I don't have an easy
			//   heuristic for it yet.
			// - AMD Phenom IIs are unknown (either prefer 8 byte, or unaligned).

			xAlignPtr(16);
		}
	}

	__emitinline u8* xGetAlignedCallTarget()
	{
		xAlignCallTarget();
		return x86Ptr;
	}

	__emitinline void xAdvancePtr(uint bytes)
	{
		if (IsDevBuild)
		{
			// common debugger courtesy: advance with INT3 as filler.
			for (uint i = 0; i < bytes; i++)
				xWrite8(0xcc);
		}
		else
			x86Ptr += bytes;
	}


	// ------------------------------------------------------------------------
	// Internal implementation of EmitSibMagic which has been custom tailored
	// to optimize special forms of the Lea instructions accordingly, such
	// as when a LEA can be replaced with a "MOV reg,imm" or "MOV reg,reg".
	//
	// preserve_flags - set to ture to disable use of SHL on [Index*Base] forms
	// of LEA, which alters flags states.
	//
	static void EmitLeaMagic(const xRegisterInt& to, const xIndirectVoid& src, bool preserve_flags)
	{
		int displacement_size = (src.Displacement == 0) ? 0 :
                                                          ((src.IsByteSizeDisp()) ? 1 : 2);

		// See EmitSibMagic for commenting on SIB encoding.

		if (!NeedsSibMagic(src) && src.Displacement == (s32)src.Displacement)
		{
			// LEA Land: means we have either 1-register encoding or just an offset.
			// offset is encodable as an immediate MOV, and a register is encodable
			// as a register MOV.

			if (src.Index.IsEmpty())
			{
				xMOV(to, src.Displacement);
				return;
			}
			else if (displacement_size == 0)
			{
				xMOV(to, src.Index.MatchSizeTo(to));
				return;
			}
			else if (!preserve_flags)
			{
				// encode as MOV and ADD combo.  Make sure to use the immediate on the
				// ADD since it can encode as an 8-bit sign-extended value.

				xMOV(to, src.Index.MatchSizeTo(to));
				xADD(to, src.Displacement);
				return;
			}
		}
		else
		{
			if (src.Base.IsEmpty())
			{
				if (!preserve_flags && (displacement_size == 0))
				{
					// Encode [Index*Scale] as a combination of Mov and Shl.
					// This is more efficient because of the bloated LEA format which requires
					// a 32 bit displacement, and the compact nature of the alternative.
					//
					// (this does not apply to older model P4s with the broken barrel shifter,
					//  but we currently aren't optimizing for that target anyway).

					xMOV(to, src.Index);
					xSHL(to, src.Scale);
					return;
				}
			}
			else
			{
				if (src.Scale == 0)
				{
					if (!preserve_flags)
					{
						if (src.Index == rsp)
						{
							// ESP is not encodable as an index (ix86 ignores it), thus:
							xMOV(to, src.Base.MatchSizeTo(to)); // will do the trick!
							if (src.Displacement)
								xADD(to, src.Displacement);
							return;
						}
						else if (src.Displacement == 0)
						{
							xMOV(to, src.Base.MatchSizeTo(to));
							xADD(to, src.Index.MatchSizeTo(to));
							return;
						}
					}
					else if ((src.Index == rsp) && (src.Displacement == 0))
					{
						// special case handling of ESP as Index, which is replaceable with
						// a single MOV even when preserve_flags is set! :D

						xMOV(to, src.Base.MatchSizeTo(to));
						return;
					}
				}
			}
		}

		xOpWrite(0, 0x8d, to, src);
	}

	__emitinline void xLEA(xRegister64 to, const xIndirectVoid& src, bool preserve_flags)
	{
		EmitLeaMagic(to, src, preserve_flags);
	}

	__emitinline void xLEA(xRegister32 to, const xIndirectVoid& src, bool preserve_flags)
	{
		EmitLeaMagic(to, src, preserve_flags);
	}

	__emitinline void xLEA(xRegister16 to, const xIndirectVoid& src, bool preserve_flags)
	{
		xWrite8(0x66);
		EmitLeaMagic(to, src, preserve_flags);
	}

	__emitinline u32* xLEA_Writeback(xAddressReg to)
	{
		xOpWrite(0, 0x8d, to, ptr[(void*)(0xdcdcdcd + (uptr)xGetPtr() + 7)]);

		return (u32*)xGetPtr() - 1;
	}

	// =====================================================================================================
	//  TEST / INC / DEC
	// =====================================================================================================
	void xImpl_Test::operator()(const xRegisterInt& to, const xRegisterInt& from) const
	{
		pxAssert(to.GetOperandSize() == from.GetOperandSize());
		xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0x84 : 0x85, from, to);
	}

	void xImpl_Test::operator()(const xIndirect64orLess& dest, int imm) const
	{
		xOpWrite(dest.GetPrefix16(), dest.Is8BitOp() ? 0xf6 : 0xf7, 0, dest, dest.GetImmSize());
		dest.xWriteImm(imm);
	}

	void xImpl_Test::operator()(const xRegisterInt& to, int imm) const
	{
		if (to.IsAccumulator())
		{
			xOpAccWrite(to.GetPrefix16(), to.Is8BitOp() ? 0xa8 : 0xa9, 0, to);
		}
		else
		{
			xOpWrite(to.GetPrefix16(), to.Is8BitOp() ? 0xf6 : 0xf7, 0, to);
		}
		to.xWriteImm(imm);
	}

	void xImpl_BitScan::operator()(const xRegister16or32or64& to, const xRegister16or32or64& from) const
	{
		pxAssert(to->GetOperandSize() == from->GetOperandSize());
		xOpWrite0F(from->GetPrefix16(), Opcode, to, from);
	}
	void xImpl_BitScan::operator()(const xRegister16or32or64& to, const xIndirectVoid& sibsrc) const
	{
		xOpWrite0F(to->GetPrefix16(), Opcode, to, sibsrc);
	}

	void xImpl_IncDec::operator()(const xRegisterInt& to) const
	{
		if (to.Is8BitOp())
		{
			u8 regfield = isDec ? 1 : 0;
			xOpWrite(to.GetPrefix16(), 0xfe, regfield, to);
		}
		else
		{
			xOpWrite(to.GetPrefix16(), 0xff, isDec ? 1 : 0, to);
		}
	}

	void xImpl_IncDec::operator()(const xIndirect64orLess& to) const
	{
		to.prefix16();
		xWrite8(to.Is8BitOp() ? 0xfe : 0xff);
		EmitSibMagic(isDec ? 1 : 0, to);
	}

	void xImpl_DwordShift::operator()(const xRegister16or32or64& to, const xRegister16or32or64& from, const xRegisterCL& /* clreg */) const
	{
		pxAssert(to->GetOperandSize() == from->GetOperandSize());
		xOpWrite0F(from->GetPrefix16(), OpcodeBase + 1, to, from);
	}

	void xImpl_DwordShift::operator()(const xRegister16or32or64& to, const xRegister16or32or64& from, u8 shiftcnt) const
	{
		pxAssert(to->GetOperandSize() == from->GetOperandSize());
		if (shiftcnt != 0)
			xOpWrite0F(from->GetPrefix16(), OpcodeBase, to, from, shiftcnt);
	}

	void xImpl_DwordShift::operator()(const xIndirectVoid& dest, const xRegister16or32or64& from, const xRegisterCL& /* clreg */) const
	{
		xOpWrite0F(from->GetPrefix16(), OpcodeBase + 1, from, dest);
	}

	void xImpl_DwordShift::operator()(const xIndirectVoid& dest, const xRegister16or32or64& from, u8 shiftcnt) const
	{
		if (shiftcnt != 0)
			xOpWrite0F(from->GetPrefix16(), OpcodeBase, from, dest, shiftcnt);
	}


	//////////////////////////////////////////////////////////////////////////////////////////
	// Push / Pop Emitters
	//
	// Note: pushad/popad implementations are intentionally left out.  The instructions are
	// invalid in x64, and are super slow on x32.  Use multiple Push/Pop instructions instead.

	__emitinline void xPOP(const xIndirectVoid& from)
	{
		EmitRexImplicitlyWide(from);
		xWrite8(0x8f);
		EmitSibMagic(0, from);
	}

	__emitinline void xPUSH(const xIndirectVoid& from)
	{
		EmitRexImplicitlyWide(from);
		xWrite8(0xff);
		EmitSibMagic(6, from);
	}

	__fi void xPOP(xRegister32or64 from)
	{
		EmitRexImplicitlyWide(from);
		xWrite8(0x58 | (from->Id & 7));
	}

	__fi void xPUSH(u32 imm)
	{
		if (is_s8(imm))
		{
			xWrite8(0x6a);
			xWrite8(imm);
		}
		else
		{
			xWrite8(0x68);
			xWrite32(imm);
		}
	}
	__fi void xPUSH(xRegister32or64 from)
	{
		EmitRexImplicitlyWide(from);
		xWrite8(0x50 | (from->Id & 7));
	}

	// pushes the EFLAGS register onto the stack
	__fi void xPUSHFD() { xWrite8(0x9C); }
	// pops the EFLAGS register from the stack
	__fi void xPOPFD() { xWrite8(0x9D); }


	//////////////////////////////////////////////////////////////////////////////////////////
	//

	__fi void xLEAVE() { xWrite8(0xC9); }
	__fi void xRET() { xWrite8(0xC3); }
	__fi void xCBW() { xWrite16(0x9866); }
	__fi void xCWD() { xWrite8(0x98); }
	__fi void xCDQ() { xWrite8(0x99); }
	__fi void xCWDE() { xWrite8(0x98); }
	__fi void xCDQE() { xWrite16(0x9848); }

	__fi void xLAHF() { xWrite8(0x9f); }
	__fi void xSAHF() { xWrite8(0x9e); }

	__fi void xSTC() { xWrite8(0xF9); }
	__fi void xCLC() { xWrite8(0xF8); }

	// NOP 1-byte
	__fi void xNOP() { xWrite8(0x90); }

	__fi void xINT(u8 imm)
	{
		if (imm == 3)
			xWrite8(0xcc);
		else
		{
			xWrite8(0xcd);
			xWrite8(imm);
		}
	}

	__fi void xINTO() { xWrite8(0xce); }

	__emitinline void xBSWAP(const xRegister32or64& to)
	{
		xWrite8(0x0F);
		xWrite8(0xC8 | to->Id);
	}

	alignas(16) static u64 xmm_data[iREGCNT_XMM * 2];

	__emitinline void xStoreReg(const xRegisterSSE& src)
	{
		xMOVDQA(ptr[&xmm_data[src.Id * 2]], src);
	}

	__emitinline void xRestoreReg(const xRegisterSSE& dest)
	{
		xMOVDQA(dest, ptr[&xmm_data[dest.Id * 2]]);
	}

//////////////////////////////////////////////////////////////////////////////////////////
// Helper object to handle ABI frame
// All x86-64 calling conventions ensure/require stack to be 16 bytes aligned
// I couldn't find documentation on when, but compilers would indicate it's before the call: https://gcc.godbolt.org/z/KzTfsz
#define ALIGN_STACK(v) xADD(rsp, v)

	static void stackAlign(int offset, bool moveDown)
	{
		int needed = (16 - (offset % 16)) % 16;
		if (moveDown)
		{
			needed = -needed;
		}
		ALIGN_STACK(needed);
	}

	xScopedStackFrame::xScopedStackFrame(bool base_frame, bool save_base_pointer, int offset)
	{
		m_base_frame = base_frame;
		m_save_base_pointer = save_base_pointer;
		m_offset = offset;

		m_offset += sizeof(void*); // Call stores the return address (4 bytes)

		// Note rbp can surely be optimized in 64 bits
		if (m_base_frame)
		{
			xPUSH(rbp);
			xMOV(rbp, rsp);
			m_offset += sizeof(void*);
		}
		else if (m_save_base_pointer)
		{
			xPUSH(rbp);
			m_offset += sizeof(void*);
		}

		xPUSH(rbx);
		xPUSH(r12);
		xPUSH(r13);
		xPUSH(r14);
		xPUSH(r15);
		m_offset += 40;
#ifdef _WIN32
		xPUSH(rdi);
		xPUSH(rsi);
		m_offset += 16;

		// Align for movaps, in addition to any following instructions
		stackAlign(m_offset, true);

		xSUB(rsp, 16 * 10);
		for (u32 i = 6; i < 16; i++)
			xMOVAPS(ptr128[rsp + (i - 6) * 16], xRegisterSSE(i));
		xSUB(rsp, 32); // Windows calling convention specifies additional space for the callee to spill registers
#else
		// Align for any following instructions
		stackAlign(m_offset, true);
#endif
		if (u8* ptr = xGetTextPtr())
			xLoadFarAddr(RTEXTPTR, ptr);
	}

	xScopedStackFrame::~xScopedStackFrame()
	{
		// Restore the register context
#ifdef _WIN32
		xADD(rsp, 32);
		for (u32 i = 6; i < 16; i++)
			xMOVAPS(xRegisterSSE::GetInstance(i), ptr[rsp + (i - 6) * 16]);
		xADD(rsp, 16 * 10);

		stackAlign(m_offset, false);
		xPOP(rsi);
		xPOP(rdi);
#else
		stackAlign(m_offset, false);
#endif
		xPOP(r15);
		xPOP(r14);
		xPOP(r13);
		xPOP(r12);
		xPOP(rbx);

		// Destroy the frame
		if (m_base_frame)
		{
			xLEAVE();
		}
		else if (m_save_base_pointer)
		{
			xPOP(rbp);
		}
	}

	xScopedSavedRegisters::xScopedSavedRegisters(std::initializer_list<std::reference_wrapper<const xAddressReg>> regs)
		: regs(regs)
	{
		for (auto reg : regs)
		{
			const xAddressReg& regRef = reg;
			xPUSH(regRef);
		}
		stackAlign(regs.size() * wordsize, true);
	}

	xScopedSavedRegisters::~xScopedSavedRegisters()
	{
		stackAlign(regs.size() * wordsize, false);
		for (auto it = regs.rbegin(); it < regs.rend(); ++it)
		{
			const xAddressReg& regRef = *it;
			xPOP(regRef);
		}
	}

	xAddressVoid xComplexAddress(const xAddressReg& tmpRegister, void* base, const xAddressVoid& offset)
	{
		if ((sptr)base == (s32)(sptr)base)
		{
			return offset + base;
		}
		if (u8* ptr = xGetTextPtr())
		{
			sptr tbase = (sptr)base - (sptr)ptr;
			if (tbase == (s32)tbase)
				return offset + RTEXTPTR + tbase;
		}
		xLEA(tmpRegister, ptr[base]);
		return offset + tmpRegister;
	}

	void xLoadFarAddr(const xAddressReg& dst, void* addr)
	{
		sptr iaddr = (sptr)addr;
		sptr rip = (sptr)xGetPtr() + 7; // LEA will be 7 bytes
		sptr disp = iaddr - rip;
		u8* textPtr = xGetTextPtr();
		sptr textdisp = iaddr - (sptr)textPtr;
		bool isRTextPtr = dst == RTEXTPTR;
		bool canUseTextPtr = textPtr && !isRTextPtr;
		if (disp == (s32)disp || (canUseTextPtr && (textdisp == (s32)textdisp)))
		{
			if (isRTextPtr && textPtr)
			{
				// Prevent LEA from trying to use RTEXTPTR to load RTEXTPTR
				xSetTextPtr(nullptr);
				xLEA(dst, ptr[addr]);
				xSetTextPtr(textPtr);
			}
			else
			{
				xLEA(dst, ptr[addr]);
			}
		}
		else
		{
			xMOV64(dst, iaddr);
		}
	}

	void xWriteImm64ToMem(u64* addr, const xAddressReg& tmp, u64 imm)
	{
		xImm64Op(xMOV, ptr64[addr], tmp, imm);
	}

} // End namespace x86Emitter
