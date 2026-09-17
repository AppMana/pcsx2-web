// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/emitter/internal.h"

namespace x86Emitter
{


	void xImplBMI_RVM::operator()(const xRegisterInt& to, const xRegisterInt& from1, const xRegisterInt& from2) const
	{
		XTRACE(XOp::BMI_RVM, 0, xtrace::Imm{Prefix | (MbPrefix << 8) | (Opcode << 16)}, to, from1, from2);
		xOpWriteC4(Prefix, MbPrefix, Opcode, to, from1, from2);
	}
	void xImplBMI_RVM::operator()(const xRegisterInt& to, const xRegisterInt& from1, const xIndirectVoid& from2) const
	{
		XTRACE(XOp::BMI_RVM, 0, xtrace::Imm{Prefix | (MbPrefix << 8) | (Opcode << 16)}, to, from1, from2);
		xOpWriteC4(Prefix, MbPrefix, Opcode, to, from1, from2);
	}
} // namespace x86Emitter
