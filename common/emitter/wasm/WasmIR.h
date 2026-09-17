// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The operation vocabulary of the emitter API, independent of any backend. Every public entry
// point of common/emitter maps to one XOp with a fixed operand list. The wasm backend records
// entry point calls as XRecords (its IR) and lowers them per function; the XTRACE recorder in
// the x86 backend writes the same records to a trace, which is what makes the two backends
// comparable instruction by instruction.

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstring>

namespace x86Emitter
{
	enum class XOp : u16
	{
		None = 0,

		// Group 1: ops[0] = destination (reg or mem), ops[1] = source (reg, mem or imm).
		ADD,
		OR,
		ADC,
		SBB,
		AND,
		SUB,
		XOR,
		CMP,

		// Group 2: ops[0] = destination (reg or mem), ops[1] = count (reg cl or imm).
		ROL,
		ROR,
		RCL,
		RCR,
		SHL,
		SHR,
		SAR,

		// Group 3: ops[0] = operand (reg or mem).
		NOT,
		NEG,
		UMUL,
		UDIV,
		IMUL,
		IDIV,

		// Two and three operand signed multiply: ops[0] = dst reg, ops[1] = src (reg or mem), [ops[2] = imm].
		IMUL2,
		IMUL3,

		// Group 8: ops[0] = bit base (reg or mem), ops[1] = bit offset (reg or imm).
		BT,
		BTS,
		BTR,
		BTC,

		// TEST: ops[0] = reg or mem, ops[1] = reg or imm.
		TEST,

		// Bit scan: ops[0] = dst reg, ops[1] = src (reg or mem).
		BSF,
		BSR,

		// ops[0] = reg or mem.
		INC,
		DEC,

		// Double shifts: ops[0] = dst (reg or mem), ops[1] = src reg, ops[2] = count (reg cl or imm).
		SHLD,
		SHRD,

		// MOV: ops[0] = dst (reg or mem), ops[1] = src (reg, mem or imm). flags bit 0 = preserve_flags.
		MOV,
		// MOV64: ops[0] = dst reg64, ops[1] = imm64. flags bit 0 = preserve_flags.
		MOV64,
		// CMOVcc: ops[0] = cond, ops[1] = dst reg, ops[2] = src (reg or mem).
		CMOV,
		// SETcc: ops[0] = cond, ops[1] = dst (reg8 or mem8).
		SETCC,
		// ops[0] = dst reg, ops[1] = src (reg or mem, sized).
		MOVSX,
		MOVZX,
		// LEA: ops[0] = dst reg, ops[1] = mem. flags bit 0 = preserve_flags.
		LEA,

		// ops[0] = reg, mem or imm.
		PUSH,
		POP,

		PUSHFD,
		POPFD,
		LEAVE,
		RET,
		CBW,
		CWD,
		CDQ,
		CWDE,
		CDQE,
		LAHF,
		SAHF,
		STC,
		CLC,
		NOP,
		// ops[0] = imm.
		INT,
		INTO,
		// ops[0] = reg.
		BSWAP,

		// JMP: ops[0] = target: reg, mem (native sized) or code pointer.
		JMP,
		// CALL: ops[0] = target: reg, mem (native sized) or host function.
		CALL,
		// Conditional jump to a known code pointer: ops[0] = cond, ops[1] = code pointer.
		JCC,
		// Forward jump to a label bound later by LABEL: ops[0] = cond, ops[1] = label.
		// flags: XRecord::LegacyJ8 / LegacyJ32 when it came through the legacy J*8/J*32 API.
		JCC_FWD,
		// ops[0] = label.
		LABEL,
		// xJcc32 with a patch slot: ops[0] = cond, ops[1] = slot.
		JCC_LINK,
		// xFastCall: ops[0] = host function or mem (native sized), ops[1] = a1 (reg, imm, mem32, address or empty),
		// ops[2] = a2 (reg, imm or empty).
		FASTCALL,

		// SIMD operations described by SIMDInstructionInfo bits in ops[0] (imm):
		// SIMD2: dst, src1. SIMD2I: dst, src1, imm. SIMD3: dst, src1, src2. SIMD3I: dst, src1, src2, imm.
		// SIMD4: dst, src1, src2, src3. Source operands are xmm registers or memory.
		SIMD2,
		SIMD2I,
		SIMD3,
		SIMD3I,
		SIMD4,

		// ops[0] = mem.
		STMXCSR,
		LDMXCSR,
		FXSAVE,
		FXRSTOR,
		VZEROUPPER,

		// BMI RVM form: ops[0] = imm (prefix | mb_prefix << 8 | opcode << 16), ops[1] = dst, ops[2] = src1, ops[3] = src2 (reg or mem).
		BMI_RVM,

		Count
	};

	enum class XOperandKind : u8
	{
		Empty = 0,
		// A general purpose register: reg = id (with 0x10 for the spl/bpl/sil/dil forms), size = operand size.
		Reg,
		// An xmm register: reg = id, size = 16 (32 for ymm).
		Xmm,
		// A memory operand: reg = base id or 0xff, index = index id or -1, scale = shift, imm = displacement,
		// symbol = relocation for an absolute displacement (the displacement is then relative to the symbol).
		Mem,
		// An immediate: imm = value; symbol = relocation when the value is an address.
		Imm,
		// A condition code: imm = JccComparisonType.
		Cond,
		// A trace-local jump label: index = label id.
		Label,
		// A trace-local code pointer (jump target in JIT code): index = id.
		CodePtr,
		// A trace-local xJcc32 patch slot: index = id.
		Slot,
		// A host function: symbol = its relocation.
		FuncPtr,
		// A host address passed by value (xFastCall(f, void*)): imm = address, symbol = relocation.
		Addr,
	};

	struct XOperand
	{
		u8 kind = 0;
		u8 size = 0;
		u8 reg = 0xff;
		u8 scale = 0;
		s32 index = -1;
		s64 imm = 0;
		u32 symbol = 0;

		XOperandKind Kind() const { return static_cast<XOperandKind>(kind); }
		bool IsEmpty() const { return Kind() == XOperandKind::Empty; }
		bool IsReg() const { return Kind() == XOperandKind::Reg; }
		bool IsXmm() const { return Kind() == XOperandKind::Xmm; }
		bool IsMem() const { return Kind() == XOperandKind::Mem; }
		bool IsImm() const { return Kind() == XOperandKind::Imm; }
		bool HasBase() const { return reg != 0xff; }
		bool HasIndex() const { return index >= 0; }
	};
	static_assert(sizeof(XOperand) == 24);

	static constexpr u32 XRECORD_MAX_OPERANDS = 5;

	struct XRecord
	{
		enum : u8
		{
			PreserveFlags = 1 << 0,
			LegacyJ8 = 1 << 1,
			LegacyJ32 = 1 << 2,
		};

		u16 op = 0;
		u8 nops = 0;
		u8 flags = 0;
		XOperand ops[XRECORD_MAX_OPERANDS];

		XOp Op() const { return static_cast<XOp>(op); }
	};

	// The on-disk record header; the operands follow it directly.
	struct XRecordHeader
	{
		u16 op;
		u8 nops;
		u8 flags;
	};
	static_assert(sizeof(XRecordHeader) == 4);

	const char* xGetOpName(XOp op);
} // namespace x86Emitter
