// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/emitter/xtrace.h"
#include "common/Assertions.h"
#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#endif


namespace x86Emitter::xtrace
{
	thread_local bool s_enabled = false;
	thread_local u32 s_depth = 0;

	namespace
	{
		struct Region
		{
			std::string name;
			uptr base;
			size_t size;
			u8 kind;
		};

		struct Recorder
		{
			Trace trace;
			std::unordered_map<uptr, u32> symbol_ids; // symbol base -> index + 1
			std::unordered_map<const void*, s32> labels;
			std::unordered_map<uptr, s32> code_ptrs;
			std::unordered_map<const void*, s32> slots;
			std::string block_kind;
			u32 block_pc = 0;
			bool in_block = false;
		};

		std::mutex s_region_mutex;
		std::vector<Region> s_regions;
		BlockSink s_block_sink = nullptr;
		thread_local Recorder* s_recorder = nullptr;

		Recorder& Rec()
		{
			pxAssertRel(s_recorder, "XTRACE recording without a Begin()");
			return *s_recorder;
		}

		u32 InternSymbol(Recorder& rec, uptr base, std::string name, u64 size, u8 kind)
		{
			auto it = rec.symbol_ids.find(base);
			if (it != rec.symbol_ids.end())
				return it->second;

			rec.trace.symbols.push_back(Symbol{std::move(name), size, kind});
			const u32 id = static_cast<u32>(rec.trace.symbols.size());
			rec.symbol_ids.emplace(base, id);
			return id;
		}

		// Executable load segments are the fallback for file-local symbols that dladdr cannot see.
		struct Segment
		{
			uptr base;
			size_t size;
			bool exec;
		};

		const std::vector<Segment>& ExecutableSegments()
		{
			static std::vector<Segment> segments = []() {
				std::vector<Segment> ret;
#if defined(__linux__)
				dl_iterate_phdr(
					[](struct dl_phdr_info* info, size_t, void* data) {
						// The first object reported is the executable itself.
						auto* segs = static_cast<std::vector<Segment>*>(data);
						for (int i = 0; i < info->dlpi_phnum; i++)
						{
							const ElfW(Phdr)& ph = info->dlpi_phdr[i];
							if (ph.p_type != PT_LOAD)
								continue;
							segs->push_back(Segment{static_cast<uptr>(info->dlpi_addr + ph.p_vaddr),
								static_cast<size_t>(ph.p_memsz), (ph.p_flags & PF_X) != 0});
						}
						return 1;
					},
					&ret);
#endif
				return ret;
			}();
			return segments;
		}

		// Resolves an absolute address to (symbol id, offset). Returns 0 when it is not an address
		// the trace can relocate.
		u32 Symbolize(Recorder& rec, uptr addr, s64& offset)
		{
			{
				std::lock_guard lock(s_region_mutex);
				for (const Region& r : s_regions)
				{
					if (addr >= r.base && addr < (r.base + r.size))
					{
						offset = static_cast<s64>(addr - r.base);
						return InternSymbol(rec, r.base, r.name, r.size, r.kind);
					}
				}
			}

#if defined(__linux__)
			Dl_info info;
			ElfW(Sym)* sym = nullptr;
			if (dladdr1(reinterpret_cast<void*>(addr), &info, reinterpret_cast<void**>(&sym), RTLD_DL_SYMENT) != 0 &&
				info.dli_saddr && info.dli_sname && sym)
			{
				const uptr base = reinterpret_cast<uptr>(info.dli_saddr);
				const u64 size = sym->st_size;
				const bool func = (ELF64_ST_TYPE(sym->st_info) == STT_FUNC);
				if (addr >= base && (size == 0 ? addr == base : addr < (base + size)))
				{
					offset = static_cast<s64>(addr - base);
					return InternSymbol(rec, base, info.dli_sname, size, func ? 1 : 0);
				}
			}
#endif

			const std::vector<Segment>& segments = ExecutableSegments();
			for (size_t i = 0; i < segments.size(); i++)
			{
				const Segment& seg = segments[i];
				if (addr >= seg.base && addr < (seg.base + seg.size))
				{
					offset = static_cast<s64>(addr - seg.base);
					return InternSymbol(rec, seg.base, fmt::format("seg{}", i), seg.size, seg.exec ? 2 : 0);
				}
			}

			return 0;
		}

		bool LooksLikeAddress(s64 value)
		{
			return static_cast<u64>(value) >= 0x10000;
		}
	} // namespace

	void RegisterRegion(const char* name, const void* base, size_t size)
	{
		std::lock_guard lock(s_region_mutex);
		s_regions.push_back(Region{name, reinterpret_cast<uptr>(base), size, 0});
	}

	void ClearRegions()
	{
		std::lock_guard lock(s_region_mutex);
		s_regions.clear();
	}

	bool IsEnabled()
	{
		return s_enabled;
	}

	void Begin()
	{
		pxAssertRel(!s_recorder, "XTRACE Begin() while recording");
		s_recorder = new Recorder();
		s_enabled = true;
	}

	Trace End()
	{
		pxAssertRel(s_recorder, "XTRACE End() without Begin()");
		s_enabled = false;
		Recorder* rec = s_recorder;
		s_recorder = nullptr;

		// Capture the bytes behind every absolute memory operand, so that replays can see the
		// emit-time values (xLDMXCSR) and the data the code reads.
		for (const XRecord& r : rec->trace.records)
		{
			for (u32 i = 0; i < r.nops; i++)
			{
				const XOperand& op = r.ops[i];
				if (!op.IsMem() || op.symbol == 0 || op.HasBase() || op.HasIndex())
					continue;

				const Symbol& sym = rec->trace.symbols[op.symbol - 1];
				uptr base = 0;
				for (const auto& [b, id] : rec->symbol_ids)
				{
					if (id == op.symbol)
					{
						base = b;
						break;
					}
				}

				const u64 size = std::max<u64>(op.size, 16);
				if (sym.size != 0 && static_cast<u64>(op.imm) >= sym.size)
					continue;
				const u64 avail = (sym.size != 0) ? std::min<u64>(size, sym.size - op.imm) : size;

				bool covered = false;
				for (const Chunk& c : rec->trace.chunks)
				{
					if (c.symbol == op.symbol && static_cast<u64>(op.imm) >= c.offset &&
						(static_cast<u64>(op.imm) + avail) <= (c.offset + c.data.size()))
					{
						covered = true;
						break;
					}
				}
				if (covered)
					continue;

				Chunk chunk;
				chunk.symbol = op.symbol;
				chunk.offset = static_cast<u64>(op.imm);
				chunk.data.resize(avail);
				std::memcpy(chunk.data.data(), reinterpret_cast<const void*>(base + op.imm), avail);
				rec->trace.chunks.push_back(std::move(chunk));
			}
		}

		rec->trace.num_labels = static_cast<u32>(rec->labels.size());
		rec->trace.num_code_ptrs = static_cast<u32>(rec->code_ptrs.size());
		rec->trace.num_slots = static_cast<u32>(rec->slots.size());
		Trace ret = std::move(rec->trace);
		delete rec;
		return ret;
	}

	void Abandon()
	{
		s_enabled = false;
		delete s_recorder;
		s_recorder = nullptr;
	}

	void SetBlockSink(BlockSink sink)
	{
		s_block_sink = sink;
	}

	bool HasBlockSink()
	{
		return s_block_sink != nullptr;
	}

	void BeginBlock(const char* kind, u32 pc)
	{
		if (!s_block_sink || s_recorder)
			return;

		Begin();
		s_recorder->block_kind = kind;
		s_recorder->block_pc = pc;
		s_recorder->in_block = true;
	}

	void EndBlock()
	{
		if (!s_recorder || !s_recorder->in_block)
			return;

		const std::string kind = s_recorder->block_kind;
		const u32 pc = s_recorder->block_pc;
		Trace trace = End();
		if (s_block_sink)
			s_block_sink(kind.c_str(), pc, std::move(trace));
	}

	XOperand Conv(const xRegisterBase& reg)
	{
		XOperand op;
		if (reg.IsEmpty() || reg.IsInvalid())
		{
			op.kind = static_cast<u8>(XOperandKind::Empty);
			return op;
		}

		const uint size = reg.GetOperandSize();
		op.kind = static_cast<u8>((size >= 16) ? XOperandKind::Xmm : XOperandKind::Reg);
		op.size = static_cast<u8>(size);
		op.reg = static_cast<u8>(reg.Id);
		return op;
	}

	XOperand Conv(const xRegister16or32or64& reg)
	{
		return Conv(static_cast<const xRegisterBase&>(reg));
	}

	XOperand Conv(const xRegister32or64& reg)
	{
		return Conv(static_cast<const xRegisterBase&>(reg));
	}

	XOperand Conv(const xIndirectVoid& mem)
	{
		XOperand op;
		op.kind = static_cast<u8>(XOperandKind::Mem);
		op.size = static_cast<u8>(mem.GetRawOperandSize());
		op.reg = mem.Base.IsEmpty() ? 0xff : static_cast<u8>(mem.Base.Id);
		op.index = mem.Index.IsEmpty() ? -1 : mem.Index.Id;
		op.scale = static_cast<u8>(mem.Scale);
		op.imm = mem.Displacement;
		if (s_recorder && (op.reg == 0xff && op.index < 0 ? mem.Displacement != 0 : LooksLikeAddress(mem.Displacement)))
		{
			s64 offset = 0;
			const u32 sym = Symbolize(*s_recorder, static_cast<uptr>(mem.Displacement), offset);
			if (sym != 0)
			{
				op.symbol = sym;
				op.imm = offset;
			}
		}
		return op;
	}

	XOperand Conv(JccComparisonType cond)
	{
		XOperand op;
		op.kind = static_cast<u8>(XOperandKind::Cond);
		op.imm = static_cast<s64>(cond);
		return op;
	}

	XOperand Conv(Label label)
	{
		XOperand op;
		op.kind = static_cast<u8>(XOperandKind::Label);
		op.index = label.id;
		return op;
	}

	XOperand Conv(CodePtr ptr)
	{
		XOperand op;
		s64 offset = 0;
		const u32 sym = s_recorder ? Symbolize(*s_recorder, reinterpret_cast<uptr>(ptr.target), offset) : 0;
		if (sym != 0)
		{
			// A jump to host code: keep the symbol so the replay can route it like a call.
			op.kind = static_cast<u8>(XOperandKind::FuncPtr);
			op.symbol = sym;
			op.imm = offset;
			return op;
		}

		op.kind = static_cast<u8>(XOperandKind::CodePtr);
		if (s_recorder)
		{
			Recorder& rec = *s_recorder;
			const uptr key = reinterpret_cast<uptr>(ptr.target);
			auto it = rec.code_ptrs.find(key);
			if (it == rec.code_ptrs.end())
				it = rec.code_ptrs.emplace(key, static_cast<s32>(rec.code_ptrs.size())).first;
			op.index = it->second;
		}
		return op;
	}

	XOperand Conv(FuncPtr ptr)
	{
		XOperand op;
		op.kind = static_cast<u8>(XOperandKind::FuncPtr);
		s64 offset = 0;
		op.symbol = s_recorder ? Symbolize(*s_recorder, reinterpret_cast<uptr>(ptr.target), offset) : 0;
		op.imm = (op.symbol != 0) ? offset : static_cast<s64>(reinterpret_cast<uptr>(ptr.target));
		return op;
	}

	XOperand Conv(Addr addr)
	{
		XOperand op;
		op.kind = static_cast<u8>(XOperandKind::Addr);
		s64 offset = 0;
		op.symbol = s_recorder ? Symbolize(*s_recorder, reinterpret_cast<uptr>(addr.address), offset) : 0;
		op.imm = (op.symbol != 0) ? offset : static_cast<s64>(reinterpret_cast<uptr>(addr.address));
		return op;
	}

	XOperand Conv(Imm imm)
	{
		XOperand op;
		op.kind = static_cast<u8>(XOperandKind::Imm);
		op.imm = imm.value;
		if (s_recorder && LooksLikeAddress(imm.value))
		{
			s64 offset = 0;
			const u32 sym = Symbolize(*s_recorder, static_cast<uptr>(imm.value), offset);
			if (sym != 0)
			{
				op.symbol = sym;
				op.imm = offset;
			}
		}
		return op;
	}

	s32 NewLabel(const void* key)
	{
		Recorder& rec = Rec();
		const s32 id = static_cast<s32>(rec.labels.size());
		rec.labels[key] = id;
		return id;
	}

	s32 FindLabel(const void* key)
	{
		Recorder& rec = Rec();
		auto it = rec.labels.find(key);
		pxAssertRel(it != rec.labels.end(), "XTRACE: label target set for a jump this trace did not record");
		return it->second;
	}

	s32 NewSlot(const void* key)
	{
		Recorder& rec = Rec();
		const s32 id = static_cast<s32>(rec.slots.size());
		rec.slots[key] = id;
		return id;
	}

	void Record(XOp op, u8 flags, const XOperand* ops, u32 nops)
	{
		pxAssert(nops <= XRECORD_MAX_OPERANDS);
		XRecord rec;
		rec.op = static_cast<u16>(op);
		rec.nops = static_cast<u8>(nops);
		rec.flags = flags;
		for (u32 i = 0; i < nops; i++)
			rec.ops[i] = ops[i];
		Rec().trace.records.push_back(rec);
	}

	// --------------------------------------------------------------------------------------
	//  Serialization
	// --------------------------------------------------------------------------------------

	namespace
	{
		constexpr u32 TRACE_MAGIC = 0x43525458; // "XTRC"
		constexpr u32 TRACE_VERSION = 1;

		struct FileHeader
		{
			u32 magic;
			u32 version;
			u32 num_symbols;
			u32 num_records;
			u32 num_chunks;
			u32 num_labels;
			u32 num_code_ptrs;
			u32 num_slots;
		};

		template <typename T>
		void Put(std::vector<u8>& out, const T& v)
		{
			const size_t pos = out.size();
			out.resize(pos + sizeof(T));
			std::memcpy(out.data() + pos, &v, sizeof(T));
		}

		struct Reader
		{
			std::span<const u8> in;
			size_t pos = 0;

			template <typename T>
			bool Get(T& v)
			{
				if (pos + sizeof(T) > in.size())
					return false;
				std::memcpy(&v, in.data() + pos, sizeof(T));
				pos += sizeof(T);
				return true;
			}

			bool GetBytes(void* dst, size_t len)
			{
				if (pos + len > in.size())
					return false;
				std::memcpy(dst, in.data() + pos, len);
				pos += len;
				return true;
			}
		};
	} // namespace

	bool Serialize(const Trace& trace, std::vector<u8>& out)
	{
		FileHeader hdr = {TRACE_MAGIC, TRACE_VERSION, static_cast<u32>(trace.symbols.size()),
			static_cast<u32>(trace.records.size()), static_cast<u32>(trace.chunks.size()), trace.num_labels,
			trace.num_code_ptrs, trace.num_slots};
		Put(out, hdr);
		for (const Symbol& sym : trace.symbols)
		{
			Put(out, static_cast<u32>(sym.name.size()));
			out.insert(out.end(), sym.name.begin(), sym.name.end());
			Put(out, sym.size);
			Put(out, sym.kind);
		}
		for (const XRecord& rec : trace.records)
		{
			const XRecordHeader rh = {rec.op, rec.nops, rec.flags};
			Put(out, rh);
			for (u32 i = 0; i < rec.nops; i++)
				Put(out, rec.ops[i]);
		}
		for (const Chunk& chunk : trace.chunks)
		{
			Put(out, chunk.symbol);
			Put(out, chunk.offset);
			Put(out, static_cast<u32>(chunk.data.size()));
			out.insert(out.end(), chunk.data.begin(), chunk.data.end());
		}
		return true;
	}

	bool Deserialize(std::span<const u8> in, Trace& trace, std::string* error)
	{
		Reader r{in};
		FileHeader hdr;
		if (!r.Get(hdr) || hdr.magic != TRACE_MAGIC || hdr.version != TRACE_VERSION)
		{
			if (error)
				*error = "bad trace header";
			return false;
		}

		trace = Trace();
		trace.num_labels = hdr.num_labels;
		trace.num_code_ptrs = hdr.num_code_ptrs;
		trace.num_slots = hdr.num_slots;
		for (u32 i = 0; i < hdr.num_symbols; i++)
		{
			Symbol sym;
			u32 len;
			if (!r.Get(len) || len > 4096)
				goto truncated;
			sym.name.resize(len);
			if (!r.GetBytes(sym.name.data(), len) || !r.Get(sym.size) || !r.Get(sym.kind))
				goto truncated;
			trace.symbols.push_back(std::move(sym));
		}
		for (u32 i = 0; i < hdr.num_records; i++)
		{
			XRecordHeader rh;
			if (!r.Get(rh) || rh.nops > XRECORD_MAX_OPERANDS || rh.op >= static_cast<u16>(XOp::Count))
				goto truncated;
			XRecord rec;
			rec.op = rh.op;
			rec.nops = rh.nops;
			rec.flags = rh.flags;
			for (u32 j = 0; j < rh.nops; j++)
			{
				if (!r.Get(rec.ops[j]))
					goto truncated;
				if (rec.ops[j].symbol > trace.symbols.size())
					goto truncated;
			}
			trace.records.push_back(rec);
		}
		for (u32 i = 0; i < hdr.num_chunks; i++)
		{
			Chunk chunk;
			u32 len;
			if (!r.Get(chunk.symbol) || !r.Get(chunk.offset) || !r.Get(len) || len > (64 * 1024 * 1024))
				goto truncated;
			chunk.data.resize(len);
			if (!r.GetBytes(chunk.data.data(), len))
				goto truncated;
			trace.chunks.push_back(std::move(chunk));
		}
		return true;

	truncated:
		if (error)
			*error = "truncated trace";
		return false;
	}

	u64 Trace::Hash() const
	{
		// FNV-1a over the symbol names and the records; chunks are derived data.
		u64 h = 0xcbf29ce484222325ULL;
		const auto mix = [&h](const void* data, size_t len) {
			const u8* p = static_cast<const u8*>(data);
			for (size_t i = 0; i < len; i++)
				h = (h ^ p[i]) * 0x100000001b3ULL;
		};
		for (const Symbol& sym : symbols)
			mix(sym.name.data(), sym.name.size());
		for (const XRecord& rec : records)
		{
			const XRecordHeader rh = {rec.op, rec.nops, rec.flags};
			mix(&rh, sizeof(rh));
			mix(rec.ops, sizeof(XOperand) * rec.nops);
		}
		return h;
	}

	// --------------------------------------------------------------------------------------
	//  Disassembly
	// --------------------------------------------------------------------------------------

	static const char* RegName(const XOperand& op)
	{
		static const char* const gpr8[] = {"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
		static const char* const gpr8x[] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"};
		static const char* const gpr16[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
		static const char* const gpr32[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
		static const char* const gpr64[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
		static const char* const xmm[] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};
		const u32 id = op.reg & 0xf;
		if (op.IsXmm())
			return xmm[id];
		switch (op.size)
		{
			case 1:
				return (op.reg & 0x10) ? gpr8x[id & 7] : gpr8[id];
			case 2:
				return gpr16[id];
			case 4:
				return gpr32[id];
			default:
				return gpr64[id];
		}
	}

	static std::string FormatOperand(const Trace& trace, const XOperand& op)
	{
		const auto symname = [&trace](u32 sym) -> std::string {
			return (sym == 0 || sym > trace.symbols.size()) ? "?" : trace.symbols[sym - 1].name;
		};
		switch (op.Kind())
		{
			case XOperandKind::Empty:
				return "-";
			case XOperandKind::Reg:
			case XOperandKind::Xmm:
				return RegName(op);
			case XOperandKind::Mem:
			{
				std::string s = fmt::format("ptr{}[", op.size * 8);
				bool first = true;
				if (op.HasBase())
				{
					XOperand b;
					b.kind = static_cast<u8>(XOperandKind::Reg);
					b.size = 8;
					b.reg = op.reg;
					s += RegName(b);
					first = false;
				}
				if (op.HasIndex())
				{
					XOperand ix;
					ix.kind = static_cast<u8>(XOperandKind::Reg);
					ix.size = 8;
					ix.reg = static_cast<u8>(op.index);
					s += fmt::format("{}{}*{}", first ? "" : "+", RegName(ix), 1u << op.scale);
					first = false;
				}
				if (op.symbol != 0)
					s += fmt::format("{}{}{:+#x}", first ? "" : "+", symname(op.symbol), op.imm);
				else if (op.imm != 0 || first)
					s += fmt::format("{}{:#x}", first ? "" : "+", op.imm);
				return s + "]";
			}
			case XOperandKind::Imm:
				return (op.symbol != 0) ? fmt::format("&{}{:+#x}", symname(op.symbol), op.imm) : fmt::format("{:#x}", static_cast<u64>(op.imm));
			case XOperandKind::Cond:
			{
				static const char* const names[] = {"o", "no", "b", "ae", "z", "nz", "be", "a", "s", "ns", "pe", "po", "l", "ge", "le", "g"};
				if (op.imm == Jcc_Unconditional)
					return "jmp";
				return (op.imm >= 0 && op.imm < 16) ? names[op.imm] : "?";
			}
			case XOperandKind::Label:
				return fmt::format("L{}", op.index);
			case XOperandKind::CodePtr:
				return fmt::format("code{}", op.index);
			case XOperandKind::Slot:
				return fmt::format("slot{}", op.index);
			case XOperandKind::FuncPtr:
				return (op.symbol != 0) ? fmt::format("fn:{}{:+#x}", symname(op.symbol), op.imm) : fmt::format("fn:{:#x}", static_cast<u64>(op.imm));
			case XOperandKind::Addr:
				return (op.symbol != 0) ? fmt::format("addr:{}{:+#x}", symname(op.symbol), op.imm) : fmt::format("addr:{:#x}", static_cast<u64>(op.imm));
		}
		return "?";
	}

	std::string FormatRecord(const Trace& trace, const XRecord& rec)
	{
		std::string s = xGetOpName(rec.Op());
		if (rec.flags)
			s += fmt::format("[{:#x}]", rec.flags);
		for (u32 i = 0; i < rec.nops; i++)
			s += fmt::format("{}{}", (i == 0) ? " " : ", ", FormatOperand(trace, rec.ops[i]));
		return s;
	}

	std::string Disassemble(const Trace& trace)
	{
		std::string s;
		for (size_t i = 0; i < trace.symbols.size(); i++)
			s += fmt::format("; sym{} {} size={:#x} kind={}\n", i + 1, trace.symbols[i].name, trace.symbols[i].size, trace.symbols[i].kind);
		for (size_t i = 0; i < trace.records.size(); i++)
			s += fmt::format("{:4}: {}\n", i, FormatRecord(trace, trace.records[i]));
		for (const Chunk& c : trace.chunks)
			s += fmt::format("; chunk sym{} +{:#x} {} bytes\n", c.symbol, c.offset, c.data.size());
		return s;
	}
} // namespace x86Emitter::xtrace

namespace x86Emitter
{
	const char* xGetOpName(XOp op)
	{
		static const char* const names[] = {
			"none",
			"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp",
			"rol", "ror", "rcl", "rcr", "shl", "shr", "sar",
			"not", "neg", "umul", "udiv", "imul", "idiv",
			"imul2", "imul3",
			"bt", "bts", "btr", "btc",
			"test",
			"bsf", "bsr",
			"inc", "dec",
			"shld", "shrd",
			"mov", "mov64", "cmov", "setcc", "movsx", "movzx", "lea",
			"push", "pop",
			"pushfd", "popfd", "leave", "ret", "cbw", "cwd", "cdq", "cwde", "cdqe", "lahf", "sahf", "stc", "clc", "nop", "int", "into", "bswap",
			"jmp", "call", "jcc", "jcc_fwd", "label", "jcc_link", "fastcall",
			"simd2", "simd2i", "simd3", "simd3i", "simd4",
			"stmxcsr", "ldmxcsr", "fxsave", "fxrstor", "vzeroupper",
			"bmi_rvm",
		};
		static_assert(std::size(names) == static_cast<size_t>(XOp::Count));
		const size_t i = static_cast<size_t>(op);
		return (i < std::size(names)) ? names[i] : "?";
	}
} // namespace x86Emitter
