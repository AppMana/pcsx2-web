// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// C++ equivalents of the interpreter unpack functions that x86/Vif_UnpackSSE.cpp and
// arm64/Vif_UnpackNEON.cpp generate at runtime (VifUnpackSSE_Simple with IsAligned set and
// UnpkLoopIteration zero), plus the dVif* entry points, which route to the C unpack loop.

#include "Common.h"
#include "MTVU.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"

#include <cstring>

namespace
{
	template <bool usn>
	__fi u32 Load8(const u8* src, int i)
	{
		return usn ? static_cast<u32>(src[i]) : static_cast<u32>(static_cast<s32>(static_cast<s8>(src[i])));
	}

	template <bool usn>
	__fi u32 Load16(const u8* src, int i)
	{
		u16 v;
		std::memcpy(&v, src + i * 2, sizeof(v));
		return usn ? static_cast<u32>(v) : static_cast<u32>(static_cast<s32>(static_cast<s16>(v)));
	}

	__fi u32 Load32(const u8* src, int i)
	{
		u32 v;
		std::memcpy(&v, src + i * 4, sizeof(v));
		return v;
	}

	template <bool usn, int upk>
	__fi void Unpack(u32 (&d)[4], const u8* src)
	{
		switch (upk)
		{
			case 0: // S-32
				d[0] = d[1] = d[2] = d[3] = Load32(src, 0);
				break;
			case 1: // S-16
				d[0] = d[1] = d[2] = d[3] = Load16<usn>(src, 0);
				break;
			case 2: // S-8
				d[0] = d[1] = d[2] = d[3] = Load8<usn>(src, 0);
				break;
			case 4: // V2-32
				d[0] = Load32(src, 0);
				d[1] = Load32(src, 1);
				d[2] = d[0];
				d[3] = 0;
				break;
			case 5: // V2-16
				d[0] = Load16<usn>(src, 0);
				d[1] = Load16<usn>(src, 1);
				d[2] = d[0];
				d[3] = d[1];
				break;
			case 6: // V2-8
				d[0] = Load8<usn>(src, 0);
				d[1] = Load8<usn>(src, 1);
				d[2] = d[0];
				d[3] = d[1];
				break;
			case 8: // V3-32
				d[0] = Load32(src, 0);
				d[1] = Load32(src, 1);
				d[2] = Load32(src, 2);
				d[3] = 0;
				break;
			case 9: // V3-16
				d[0] = Load16<usn>(src, 0);
				d[1] = Load16<usn>(src, 1);
				d[2] = Load16<usn>(src, 2);
				d[3] = 0;
				break;
			case 10: // V3-8
				d[0] = Load8<usn>(src, 0);
				d[1] = Load8<usn>(src, 1);
				d[2] = Load8<usn>(src, 2);
				d[3] = 0;
				break;
			case 12: // V4-32
				d[0] = Load32(src, 0);
				d[1] = Load32(src, 1);
				d[2] = Load32(src, 2);
				d[3] = Load32(src, 3);
				break;
			case 13: // V4-16
				d[0] = Load16<usn>(src, 0);
				d[1] = Load16<usn>(src, 1);
				d[2] = Load16<usn>(src, 2);
				d[3] = Load16<usn>(src, 3);
				break;
			case 14: // V4-8
				d[0] = Load8<usn>(src, 0);
				d[1] = Load8<usn>(src, 1);
				d[2] = Load8<usn>(src, 2);
				d[3] = Load8<usn>(src, 3);
				break;
			case 15: // V4-5
			{
				u32 work = Load32(src, 0) << 3;
				d[0] = work;
				work = (work >> 8) << 3;
				d[1] = work;
				work = (work >> 8) << 3;
				d[2] = work;
				work = (work >> 8) << 7;
				d[3] = work;
				for (u32& v : d)
					v &= 0xff;
				break;
			}
			default:
				break;
		}
	}

	template <bool usn, bool doMask, int curCycle, int upk>
	u32 UnpackC(void* dest, const void* src)
	{
		u32 d[4];
		Unpack<usn, upk>(d, static_cast<const u8*>(src));

		u32* const out = static_cast<u32*>(dest);
		if (doMask)
		{
			constexpr int offX = std::min(curCycle, 3);
			for (int i = 0; i < 4; i++)
				out[i] = (d[i] & nVifMask[0][offX][i]) | (out[i] & nVifMask[1][offX][i]) | nVifMask[2][offX][i];
		}
		else
		{
			std::memcpy(out, d, sizeof(d));
		}
		return 0;
	}

	template <bool usn, bool doMask, int curCycle>
	void FillUnpackTable()
	{
		constexpr int base = ((usn ? 1 : 0) * 2 * 16) + ((doMask ? 1 : 0) * 16);
		nVifCall* const table = &nVifUpk[base * 4];
		static constexpr nVifCall functions[16] = {
			UnpackC<usn, doMask, curCycle, 0>,
			UnpackC<usn, doMask, curCycle, 1>,
			UnpackC<usn, doMask, curCycle, 2>,
			nullptr,
			UnpackC<usn, doMask, curCycle, 4>,
			UnpackC<usn, doMask, curCycle, 5>,
			UnpackC<usn, doMask, curCycle, 6>,
			nullptr,
			UnpackC<usn, doMask, curCycle, 8>,
			UnpackC<usn, doMask, curCycle, 9>,
			UnpackC<usn, doMask, curCycle, 10>,
			nullptr,
			UnpackC<usn, doMask, curCycle, 12>,
			UnpackC<usn, doMask, curCycle, 13>,
			UnpackC<usn, doMask, curCycle, 14>,
			UnpackC<usn, doMask, curCycle, 15>,
		};
		for (int i = 0; i < 16; i++)
			table[(i * 4) + curCycle] = functions[i];
	}

	template <bool usn, bool doMask>
	void FillUnpackCycles()
	{
		FillUnpackTable<usn, doMask, 0>();
		FillUnpackTable<usn, doMask, 1>();
		FillUnpackTable<usn, doMask, 2>();
		FillUnpackTable<usn, doMask, 3>();
	}
} // namespace

void VifUnpackSSE_Init()
{
	DevCon.WriteLn("Installing C++ unpacking functions for VIF interpreters...");
	FillUnpackCycles<false, false>();
	FillUnpackCycles<false, true>();
	FillUnpackCycles<true, false>();
	FillUnpackCycles<true, true>();
}

void dVifReset(int idx)
{
	nVif[idx].vifBlocks.reset();
}

void dVifRelease(int idx)
{
	nVif[idx].vifBlocks.clear();
}

_vifT __fi void dVifUnpack(const u8* data, bool isFill)
{
	VIFregisters& vifRegs = MTVU_VifXRegs;
	_nVifUnpack(idx, data, vifRegs.mode, isFill);
}

template void dVifUnpack<0>(const u8* data, bool isFill);
template void dVifUnpack<1>(const u8* data, bool isFill);
