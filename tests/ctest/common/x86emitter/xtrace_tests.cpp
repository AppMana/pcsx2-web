// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <gtest/gtest.h>
#include <common/emitter/x86emitter.h>
#include <common/emitter/xtrace.h>

using namespace x86Emitter;

namespace
{
	alignas(16) u32 s_trace_data[8];

	xtrace::Trace RecordTrace(void (*emit)())
	{
		u8 code[4096];
		xSetPtr(code);
		xtrace::Begin();
		emit();
		return xtrace::End();
	}
} // namespace

TEST(XTrace, RecordsOutermostEntryPointsOnly)
{
	const xtrace::Trace trace = RecordTrace([]() {
		xMOV(eax, 5);
		xADD(rax, rcx);
		xMOVSS(xmm0, xmm1, xmm1); // collapses to a movaps inside the emitter: one record
		xPADD.W(xmm2, ptr[&s_trace_data[0]]);
		xForwardJZ8 skip;
		xCMP(ptr32[&s_trace_data[4]], 3);
		skip.SetTarget();
		xFastCall((void*)&s_trace_data, 1, ecx);
	});

	ASSERT_EQ(trace.records.size(), 8u);
	EXPECT_EQ(trace.records[0].Op(), XOp::MOV);
	EXPECT_EQ(trace.records[1].Op(), XOp::ADD);
	EXPECT_EQ(trace.records[2].Op(), XOp::SIMD3);
	EXPECT_EQ(trace.records[3].Op(), XOp::SIMD3);
	EXPECT_EQ(trace.records[4].Op(), XOp::JCC_FWD);
	EXPECT_EQ(trace.records[5].Op(), XOp::CMP);
	EXPECT_EQ(trace.records[6].Op(), XOp::LABEL);
	EXPECT_EQ(trace.records[7].Op(), XOp::FASTCALL);

	// The label bound by SetTarget is the one the jump created.
	EXPECT_EQ(trace.records[4].ops[1].Kind(), XOperandKind::Label);
	EXPECT_EQ(trace.records[6].ops[0].index, trace.records[4].ops[1].index);
	EXPECT_EQ(trace.num_labels, 1u);

	// Absolute addresses are symbolised and their bytes captured.
	const XOperand& mem = trace.records[3].ops[3];
	EXPECT_EQ(mem.Kind(), XOperandKind::Mem);
	EXPECT_NE(mem.symbol, 0u);
	EXPECT_EQ(mem.imm, 0);
	const XOperand& cmpmem = trace.records[5].ops[0];
	EXPECT_EQ(cmpmem.symbol, mem.symbol);
	EXPECT_EQ(cmpmem.imm, 16);
	EXPECT_EQ(cmpmem.size, 4u);
	EXPECT_FALSE(trace.chunks.empty());

	// A fastcall records its target and arguments, not the moves it expands to.
	EXPECT_EQ(trace.records[7].ops[0].Kind(), XOperandKind::FuncPtr);
	EXPECT_EQ(trace.records[7].ops[1].Kind(), XOperandKind::Imm);
	EXPECT_EQ(trace.records[7].ops[1].imm, 1);
	EXPECT_EQ(trace.records[7].ops[2].Kind(), XOperandKind::Reg);
	EXPECT_EQ(trace.records[7].ops[2].reg, ecx.Id);
}

TEST(XTrace, SerializeRoundTrip)
{
	const xtrace::Trace trace = RecordTrace([]() {
		xMOV(ptr32[&s_trace_data[1]], 0x1234);
		xSHL(eax, 3);
		xJcc32(Jcc_NotEqual);
		xMOV64(rax, (s64)&s_trace_data[2]);
	});

	std::vector<u8> bytes;
	ASSERT_TRUE(xtrace::Serialize(trace, bytes));

	xtrace::Trace copy;
	std::string error;
	ASSERT_TRUE(xtrace::Deserialize(bytes, copy, &error)) << error;
	EXPECT_EQ(copy.records.size(), trace.records.size());
	EXPECT_EQ(copy.symbols.size(), trace.symbols.size());
	EXPECT_EQ(copy.chunks.size(), trace.chunks.size());
	EXPECT_EQ(copy.Hash(), trace.Hash());
	EXPECT_EQ(xtrace::Disassemble(copy), xtrace::Disassemble(trace));
	EXPECT_EQ(copy.records[2].Op(), XOp::JCC_LINK);
	EXPECT_EQ(copy.num_slots, 1u);
	EXPECT_EQ(copy.records[3].ops[1].Kind(), XOperandKind::Imm);
	EXPECT_NE(copy.records[3].ops[1].symbol, 0u);
	EXPECT_EQ(copy.records[3].ops[1].imm, 8);
}

TEST(XTrace, DisabledRecordsNothing)
{
	u8 code[256];
	xSetPtr(code);
	EXPECT_FALSE(xtrace::IsEnabled());
	xMOV(eax, ecx);
	EXPECT_EQ(xGetPtr(), code + 2);
}
