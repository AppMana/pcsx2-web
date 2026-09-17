// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// XTRACE: records the entry point calls made to the x86 emitter as XRecords, with absolute
// addresses symbolised through a relocation table, so that a trace can be replayed into any
// backend and the results compared (tests/ctest/common/emitter_diff). Recording is a per-thread
// switch and costs one predictable branch per entry point when off.

#pragma once

#include "common/emitter/x86types.h"
#include "common/emitter/wasm/WasmIR.h"

#include <span>
#include <string>
#include <vector>

#if defined(ARCH_X86) && !defined(X86EMITTER_WASM_BACKEND) && !defined(X86EMITTER_NO_XTRACE)
#define X86EMITTER_XTRACE 1
#endif

namespace x86Emitter::xtrace
{
	struct Symbol
	{
		std::string name;
		u64 size = 0;
		// 0 = data, 1 = function, 2 = executable segment.
		u8 kind = 0;
	};

	// Bytes of a symbol captured when the trace ended, for the memory the emitted code reads
	// through absolute addresses.
	struct Chunk
	{
		u32 symbol = 0;
		u64 offset = 0;
		std::vector<u8> data;
	};

	struct Trace
	{
		std::vector<Symbol> symbols;
		std::vector<XRecord> records;
		std::vector<Chunk> chunks;
		u32 num_labels = 0;
		u32 num_code_ptrs = 0;
		u32 num_slots = 0;

		u64 Hash() const;
	};

	bool Serialize(const Trace& trace, std::vector<u8>& out);
	bool Deserialize(std::span<const u8> in, Trace& trace, std::string* error);
	std::string Disassemble(const Trace& trace);
	std::string FormatRecord(const Trace& trace, const XRecord& rec);

	// Symbol resolution used by the recorder: explicit regions first, then the dynamic symbol
	// table (dladdr), then the executable's own load segments.
	void RegisterRegion(const char* name, const void* base, size_t size);
	void ClearRegions();

	// Recording control for the calling thread.
	bool IsEnabled();
	void Begin();
	Trace End();
	void Abandon();

	// Recompiler hooks: a block sink receives every trace recorded between BeginBlock/EndBlock.
	using BlockSink = void (*)(const char* kind, u32 pc, Trace&& trace);
	void SetBlockSink(BlockSink sink);
	bool HasBlockSink();
	void BeginBlock(const char* kind, u32 pc);
	void EndBlock();

	// Operand conversion helpers used by the XTRACE macro.
	struct Label
	{
		s32 id;
	};
	struct CodePtr
	{
		const void* target;
	};
	struct FuncPtr
	{
		const void* target;
	};
	struct Addr
	{
		const void* address;
	};
	struct Imm
	{
		s64 value;
	};

	XOperand Conv(const xRegisterBase& reg);
	XOperand Conv(const xRegister16or32or64& reg);
	XOperand Conv(const xRegister32or64& reg);
	XOperand Conv(const xIndirectVoid& mem);
	XOperand Conv(JccComparisonType cond);
	XOperand Conv(Label label);
	XOperand Conv(CodePtr ptr);
	XOperand Conv(FuncPtr ptr);
	XOperand Conv(Addr addr);
	XOperand Conv(Imm imm);
	inline XOperand Conv(int v) { return Conv(Imm{v}); }
	inline XOperand Conv(unsigned v) { return Conv(Imm{static_cast<s64>(v)}); }
	inline XOperand Conv(long v) { return Conv(Imm{v}); }
	inline XOperand Conv(unsigned long v) { return Conv(Imm{static_cast<s64>(v)}); }
	inline XOperand Conv(long long v) { return Conv(Imm{v}); }
	inline XOperand Conv(unsigned long long v) { return Conv(Imm{static_cast<s64>(v)}); }
	inline XOperand Conv(u8 v) { return Conv(Imm{v}); }
	inline XOperand Conv(s8 v) { return Conv(Imm{v}); }
	inline XOperand Conv(u16 v) { return Conv(Imm{v}); }
	inline XOperand Conv(s16 v) { return Conv(Imm{v}); }

	// Labels are identified by the pointer the x86 backend hands out for them (the displacement
	// byte of the jump); the recorder maps those to trace-local ids.
	s32 NewLabel(const void* key);
	s32 FindLabel(const void* key);
	s32 NewSlot(const void* key);

	void Record(XOp op, u8 flags, const XOperand* ops, u32 nops);

	extern thread_local bool s_enabled;
	extern thread_local u32 s_depth;

	// One per traced entry point: records the call when it is the outermost emitter call on this
	// thread, and suppresses the entry points it calls internally.
	class Scope
	{
	public:
		template <typename... Args>
		explicit Scope(XOp op, u8 flags, const Args&... args)
		{
			m_active = s_enabled && (s_depth++ == 0);
			if (m_active)
			{
				const XOperand ops[] = {Conv(args)..., XOperand{}};
				Record(op, flags, ops, sizeof...(Args));
			}
		}
		explicit Scope(XOp op)
		{
			m_active = s_enabled && (s_depth++ == 0);
			if (m_active)
				Record(op, 0, nullptr, 0);
		}
		~Scope() { s_depth--; }

		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		bool m_active;
	};
} // namespace x86Emitter::xtrace

#ifdef X86EMITTER_XTRACE
#define XTRACE(...) const ::x86Emitter::xtrace::Scope _xtrace_scope(__VA_ARGS__)
#else
#define XTRACE(...) \
	do \
	{ \
	} while (0)
#endif
