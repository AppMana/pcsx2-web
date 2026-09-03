// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The instruction objects of the emitter API. Each one names an operation by its x86 encoding
// data, which both backends interpret: the x86 encoder writes it out, the wasm backend lowers
// it. Keeping the tables in one place guarantees that xPADD.W means the same thing to both.

#include "common/emitter/x86types.h"
#include "common/emitter/instructions.h"

namespace x86Emitter
{
	// =====================================================================================================
	//  Group 1 / 2 / 3 / 8 Instructions
	// =====================================================================================================

	const xImpl_G1Logic xAND = {{G1Type_AND}, {SIMDInstructionInfo(0x54).commutative()}, {SIMDInstructionInfo(0x54).commutative().p66()}};
	const xImpl_G1Logic xOR  = {{G1Type_OR},  {SIMDInstructionInfo(0x56).commutative()}, {SIMDInstructionInfo(0x56).commutative().p66()}};
	const xImpl_G1Logic xXOR = {{G1Type_XOR}, {SIMDInstructionInfo(0x57).commutative()}, {SIMDInstructionInfo(0x57).commutative().p66()}};

	const xImpl_G1Arith xADD = {{G1Type_ADD}, {SIMDInstructionInfo(0x58).commutative()}, {SIMDInstructionInfo(0x58).commutative().p66()}, {SIMDInstructionInfo(0x58).pf3()}, {SIMDInstructionInfo(0x58).pf2()}};
	const xImpl_G1Arith xSUB = {{G1Type_SUB}, {SIMDInstructionInfo(0x5c)}, {SIMDInstructionInfo(0x5c).p66()}, {SIMDInstructionInfo(0x5c).pf3()}, {SIMDInstructionInfo(0x5c).pf2()}};

	const xImpl_Group1 xADC = {G1Type_ADC};
	const xImpl_Group1 xSBB = {G1Type_SBB};
	const xImpl_Group1 xCMP = {G1Type_CMP};

	const xImpl_Group2 xROL = {G2Type_ROL};
	const xImpl_Group2 xROR = {G2Type_ROR};
	const xImpl_Group2 xRCL = {G2Type_RCL};
	const xImpl_Group2 xRCR = {G2Type_RCR};
	const xImpl_Group2 xSHL = {G2Type_SHL};
	const xImpl_Group2 xSHR = {G2Type_SHR};
	const xImpl_Group2 xSAR = {G2Type_SAR};

	const xImpl_Group3 xNOT = {G3Type_NOT};
	const xImpl_Group3 xNEG = {G3Type_NEG};
	const xImpl_Group3 xUMUL = {G3Type_MUL};
	const xImpl_Group3 xUDIV = {G3Type_DIV};

	const xImpl_iDiv xDIV = {{G3Type_iDIV}, {SIMDInstructionInfo(0x5e)}, {SIMDInstructionInfo(0x5e).p66()}, {SIMDInstructionInfo(0x5e).pf3()}, {SIMDInstructionInfo(0x5e).pf2()}};
	const xImpl_iMul xMUL = {{G3Type_iMUL}, {SIMDInstructionInfo(0x59).commutative()}, {SIMDInstructionInfo(0x59).commutative().p66()}, {SIMDInstructionInfo(0x59).pf3()}, {SIMDInstructionInfo(0x59).pf2()}};

	const xImpl_Group8 xBT = {G8Type_BT};
	const xImpl_Group8 xBTR = {G8Type_BTR};
	const xImpl_Group8 xBTS = {G8Type_BTS};
	const xImpl_Group8 xBTC = {G8Type_BTC};

	const xImpl_Test xTEST = {};

	const xImpl_BitScan xBSF = {0xbc};
	const xImpl_BitScan xBSR = {0xbd};

	const xImpl_IncDec xINC = {false};
	const xImpl_IncDec xDEC = {true};

	const xImpl_DwordShift xSHLD = {0xa4};
	const xImpl_DwordShift xSHRD = {0xac};

	// =====================================================================================================
	//  MOV / CMOV / SETcc
	// =====================================================================================================

	const xImpl_Mov xMOV;
	const xImpl_MovImm64 xMOV64;

	const xImpl_MovExtend xMOVSX = {true};
	const xImpl_MovExtend xMOVZX = {false};

	const xImpl_CMov xCMOVA = {Jcc_Above};
	const xImpl_CMov xCMOVAE = {Jcc_AboveOrEqual};
	const xImpl_CMov xCMOVB = {Jcc_Below};
	const xImpl_CMov xCMOVBE = {Jcc_BelowOrEqual};

	const xImpl_CMov xCMOVG = {Jcc_Greater};
	const xImpl_CMov xCMOVGE = {Jcc_GreaterOrEqual};
	const xImpl_CMov xCMOVL = {Jcc_Less};
	const xImpl_CMov xCMOVLE = {Jcc_LessOrEqual};

	const xImpl_CMov xCMOVZ = {Jcc_Zero};
	const xImpl_CMov xCMOVE = {Jcc_Equal};
	const xImpl_CMov xCMOVNZ = {Jcc_NotZero};
	const xImpl_CMov xCMOVNE = {Jcc_NotEqual};

	const xImpl_CMov xCMOVO = {Jcc_Overflow};
	const xImpl_CMov xCMOVNO = {Jcc_NotOverflow};
	const xImpl_CMov xCMOVC = {Jcc_Carry};
	const xImpl_CMov xCMOVNC = {Jcc_NotCarry};

	const xImpl_CMov xCMOVS = {Jcc_Signed};
	const xImpl_CMov xCMOVNS = {Jcc_Unsigned};
	const xImpl_CMov xCMOVPE = {Jcc_ParityEven};
	const xImpl_CMov xCMOVPO = {Jcc_ParityOdd};


	const xImpl_Set xSETA = {Jcc_Above};
	const xImpl_Set xSETAE = {Jcc_AboveOrEqual};
	const xImpl_Set xSETB = {Jcc_Below};
	const xImpl_Set xSETBE = {Jcc_BelowOrEqual};

	const xImpl_Set xSETG = {Jcc_Greater};
	const xImpl_Set xSETGE = {Jcc_GreaterOrEqual};
	const xImpl_Set xSETL = {Jcc_Less};
	const xImpl_Set xSETLE = {Jcc_LessOrEqual};

	const xImpl_Set xSETZ = {Jcc_Zero};
	const xImpl_Set xSETE = {Jcc_Equal};
	const xImpl_Set xSETNZ = {Jcc_NotZero};
	const xImpl_Set xSETNE = {Jcc_NotEqual};

	const xImpl_Set xSETO = {Jcc_Overflow};
	const xImpl_Set xSETNO = {Jcc_NotOverflow};
	const xImpl_Set xSETC = {Jcc_Carry};
	const xImpl_Set xSETNC = {Jcc_NotCarry};

	const xImpl_Set xSETS = {Jcc_Signed};
	const xImpl_Set xSETNS = {Jcc_Unsigned};
	const xImpl_Set xSETPE = {Jcc_ParityEven};
	const xImpl_Set xSETPO = {Jcc_ParityOdd};

	// =====================================================================================================
	//  JMP / CALL
	// =====================================================================================================

	const xImpl_JmpCall xJMP = {true};
	const xImpl_JmpCall xCALL = {false};
	const xImpl_FastCall xFastCall = {};

	// =====================================================================================================
	//  BMI
	// =====================================================================================================

	const xImplBMI_RVM xMULX = {0xF2, 0x38, 0xF6};
	const xImplBMI_RVM xPDEP = {0xF2, 0x38, 0xF5};
	const xImplBMI_RVM xPEXT = {0xF3, 0x38, 0xF5};
	const xImplBMI_RVM xANDN_S = {0x00, 0x38, 0xF2};

	// =====================================================================================================
	//  SIMD
	// =====================================================================================================

	// clang-format off

	const xImplSimd_3Arg xPAND  = {SIMDInstructionInfo(0xdb).i().p66().commutative()};
	const xImplSimd_3Arg xPANDN = {SIMDInstructionInfo(0xdf).i().p66()};
	const xImplSimd_3Arg xPOR   = {SIMDInstructionInfo(0xeb).i().p66().commutative()};
	const xImplSimd_3Arg xPXOR  = {SIMDInstructionInfo(0xef).i().p66().commutative()};

	// [SSE-4.1] Performs a bitwise AND of dest against src, and sets the ZF flag
	// only if all bits in the result are 0.  PTEST also sets the CF flag according
	// to the following condition: (xmm2/m128 AND NOT xmm1) == 0;
	const xImplSimd_2Arg xPTEST = {SIMDInstructionInfo(0x17).p66().m0f38()};

	const xImplSimd_ShiftWithoutQ xPSRA =
	{
		{SIMDInstructionInfo(0xe1).p66().i(), SIMDInstructionInfo(0x71, 4).p66().i()}, // W
		{SIMDInstructionInfo(0xe2).p66().i(), SIMDInstructionInfo(0x72, 4).p66().i()}, // D
	};

	const xImplSimd_Shift xPSRL =
	{
		{SIMDInstructionInfo(0xd1).p66().i(), SIMDInstructionInfo(0x71, 2).p66().i()}, // W
		{SIMDInstructionInfo(0xd2).p66().i(), SIMDInstructionInfo(0x72, 2).p66().i()}, // D
		{SIMDInstructionInfo(0xd3).p66().i(), SIMDInstructionInfo(0x73, 2).p66().i()}, // Q
	};

	const xImplSimd_Shift xPSLL =
	{
		{SIMDInstructionInfo(0xf1).p66().i(), SIMDInstructionInfo(0x71, 6).p66().i()}, // W
		{SIMDInstructionInfo(0xf2).p66().i(), SIMDInstructionInfo(0x72, 6).p66().i()}, // D
		{SIMDInstructionInfo(0xf3).p66().i(), SIMDInstructionInfo(0x73, 6).p66().i()}, // Q
	};

	const xImplSimd_AddSub xPADD =
	{
		{SIMDInstructionInfo(0xfc).p66().i().commutative()}, // B
		{SIMDInstructionInfo(0xfd).p66().i().commutative()}, // W
		{SIMDInstructionInfo(0xfe).p66().i().commutative()}, // D
		{SIMDInstructionInfo(0xd4).p66().i().commutative()}, // Q

		{SIMDInstructionInfo(0xec).p66().i().commutative()}, // SB
		{SIMDInstructionInfo(0xed).p66().i().commutative()}, // SW
		{SIMDInstructionInfo(0xdc).p66().i().commutative()}, // USB
		{SIMDInstructionInfo(0xdd).p66().i().commutative()}, // USW
	};

	const xImplSimd_AddSub xPSUB =
	{
		{SIMDInstructionInfo(0xf8).p66().i()}, // B
		{SIMDInstructionInfo(0xf9).p66().i()}, // W
		{SIMDInstructionInfo(0xfa).p66().i()}, // D
		{SIMDInstructionInfo(0xfb).p66().i()}, // Q

		{SIMDInstructionInfo(0xe8).p66().i()}, // SB
		{SIMDInstructionInfo(0xe9).p66().i()}, // SW
		{SIMDInstructionInfo(0xd8).p66().i()}, // USB
		{SIMDInstructionInfo(0xd9).p66().i()}, // USW
	};

	const xImplSimd_PMul xPMUL =
	{
		{SIMDInstructionInfo(0xd5).p66().i().commutative()}, // LW
		{SIMDInstructionInfo(0xe5).p66().i().commutative()}, // HW
		{SIMDInstructionInfo(0xe4).p66().i().commutative()}, // HUW
		{SIMDInstructionInfo(0xf4).p66().i().commutative()}, // UDQ

		{SIMDInstructionInfo(0x0b).p66().m0f38().i().commutative()}, // HRSW
		{SIMDInstructionInfo(0x40).p66().m0f38().i().commutative()}, // LD
		{SIMDInstructionInfo(0x28).p66().m0f38().i().commutative()}, // DQ
	};

	const xImplSimd_rSqrt xRSQRT =
	{
		{SIMDInstructionInfo(0x52)},       // PS
		{SIMDInstructionInfo(0x52).pf3()}, // SS
	};

	const xImplSimd_rSqrt xRCP =
	{
		{SIMDInstructionInfo(0x53)},       // PS
		{SIMDInstructionInfo(0x53).pf3()}, // SS
	};

	const xImplSimd_Sqrt xSQRT =
	{
		{SIMDInstructionInfo(0x51)},       // PS
		{SIMDInstructionInfo(0x51).pf3()}, // SS
		{SIMDInstructionInfo(0x51).p66()}, // PD
		{SIMDInstructionInfo(0x51).pf2()}, // SS
	};

	const xImplSimd_AndNot xANDN =
	{
		{SIMDInstructionInfo(0x55)},       // PS
		{SIMDInstructionInfo(0x55).p66()}, // PD
	};

	const xImplSimd_PAbsolute xPABS =
	{
		{SIMDInstructionInfo(0x1c).p66().m0f38().i()}, // B
		{SIMDInstructionInfo(0x1d).p66().m0f38().i()}, // W
		{SIMDInstructionInfo(0x1e).p66().m0f38().i()}, // D
	};

	const xImplSimd_PSign xPSIGN =
	{
		{SIMDInstructionInfo(0x08).p66().m0f38().i()}, // B
		{SIMDInstructionInfo(0x09).p66().m0f38().i()}, // W
		{SIMDInstructionInfo(0x0a).p66().m0f38().i()}, // D
	};

	const xImplSimd_PMultAdd xPMADD =
	{
		{SIMDInstructionInfo(0xf5).p66().i().commutative()},         // WD
		{SIMDInstructionInfo(0x04).p66().m0f38().i().commutative()}, // UBSW
	};

	const xImplSimd_HorizAdd xHADD =
	{
		{SIMDInstructionInfo(0x7c).pf2()}, // PS
		{SIMDInstructionInfo(0x7c).p66()}, // PD
	};

	const xImplSimd_DotProduct xDP =
	{
		{SIMDInstructionInfo(0x40).p66().m0f3a().commutative()}, // PS
		{SIMDInstructionInfo(0x41).p66().m0f3a().commutative()}, // PD
	};

	const xImplSimd_Round xROUND =
	{
		{SIMDInstructionInfo(0x08).p66().m0f3a()}, // PS
		{SIMDInstructionInfo(0x09).p66().m0f3a()}, // PD
		{SIMDInstructionInfo(0x0a).p66().m0f3a()}, // SS
		{SIMDInstructionInfo(0x0b).p66().m0f3a()}, // SD
	};

	const xImplSimd_MinMax xMIN =
	{
		{SIMDInstructionInfo(0x5d).f()},       // PS
		{SIMDInstructionInfo(0x5d).d().p66()}, // PD
		{SIMDInstructionInfo(0x5d).f().pf3()}, // SS
		{SIMDInstructionInfo(0x5d).d().pf2()}, // SD
	};

	const xImplSimd_MinMax xMAX =
	{
		{SIMDInstructionInfo(0x5f).f()},       // PS
		{SIMDInstructionInfo(0x5f).d().p66()}, // PD
		{SIMDInstructionInfo(0x5f).f().pf3()}, // SS
		{SIMDInstructionInfo(0x5f).d().pf2()}, // SD
	};

	const xImplSimd_Compare xCMPEQ = {SSE2_Equal};
	const xImplSimd_Compare xCMPLT = {SSE2_Less};
	const xImplSimd_Compare xCMPLE = {SSE2_LessOrEqual};
	const xImplSimd_Compare xCMPUNORD = {SSE2_Unordered};
	const xImplSimd_Compare xCMPNE = {SSE2_NotEqual};
	const xImplSimd_Compare xCMPNLT = {SSE2_NotLess};
	const xImplSimd_Compare xCMPNLE = {SSE2_NotLessOrEqual};
	const xImplSimd_Compare xCMPORD = {SSE2_Ordered};

	const xImplSimd_COMI xCOMI =
	{
		{SIMDInstructionInfo(0x2f)},       // SS
		{SIMDInstructionInfo(0x2f).p66()}, // SD
	};

	const xImplSimd_COMI xUCOMI =
	{
		{SIMDInstructionInfo(0x2e)},       // SS
		{SIMDInstructionInfo(0x2e).p66()}, // SD
	};

	const xImplSimd_PCompare xPCMP =
	{
		{SIMDInstructionInfo(0x74).i().p66().commutative()}, // EQB
		{SIMDInstructionInfo(0x75).i().p66().commutative()}, // EQW
		{SIMDInstructionInfo(0x76).i().p66().commutative()}, // EQD

		{SIMDInstructionInfo(0x64).i().p66()}, // GTB
		{SIMDInstructionInfo(0x65).i().p66()}, // GTW
		{SIMDInstructionInfo(0x66).i().p66()}, // GTD
	};

	const xImplSimd_PMinMax xPMIN =
	{
		{SIMDInstructionInfo(0xda).i().p66().commutative()},         // UB
		{SIMDInstructionInfo(0xea).i().p66().commutative()},         // SW
		{SIMDInstructionInfo(0x38).i().p66().m0f38().commutative()}, // SB
		{SIMDInstructionInfo(0x39).i().p66().m0f38().commutative()}, // SD
		{SIMDInstructionInfo(0x3a).i().p66().m0f38().commutative()}, // UW
		{SIMDInstructionInfo(0x3b).i().p66().m0f38().commutative()}, // UD
	};

	const xImplSimd_PMinMax xPMAX =
	{
		{SIMDInstructionInfo(0xde).i().p66().commutative()},         // UB
		{SIMDInstructionInfo(0xee).i().p66().commutative()},         // SW
		{SIMDInstructionInfo(0x3c).i().p66().m0f38().commutative()}, // SB
		{SIMDInstructionInfo(0x3d).i().p66().m0f38().commutative()}, // SD
		{SIMDInstructionInfo(0x3e).i().p66().m0f38().commutative()}, // UW
		{SIMDInstructionInfo(0x3f).i().p66().m0f38().commutative()}, // UD
	};

	const xImplSimd_Shuffle xSHUF = {};

	const xImplSimd_PShuffle xPSHUF =
	{
		{SIMDInstructionInfo(0x70).i().p66()},         // D
		{SIMDInstructionInfo(0x70).i().pf2()},         // LW
		{SIMDInstructionInfo(0x70).i().pf3()},         // HW
		{SIMDInstructionInfo(0x00).i().p66().m0f38()}, // B
	};

	const SimdImpl_PUnpack xPUNPCK =
	{
		{SIMDInstructionInfo(0x60).i().p66()}, // LBW
		{SIMDInstructionInfo(0x61).i().p66()}, // LWD
		{SIMDInstructionInfo(0x62).i().p66()}, // LDQ
		{SIMDInstructionInfo(0x6c).i().p66()}, // LQDQ

		{SIMDInstructionInfo(0x68).i().p66()}, // HBW
		{SIMDInstructionInfo(0x69).i().p66()}, // HWD
		{SIMDInstructionInfo(0x6a).i().p66()}, // HDQ
		{SIMDInstructionInfo(0x6d).i().p66()}, // HQDQ
	};

	const SimdImpl_Pack xPACK =
	{
		{SIMDInstructionInfo(0x63).i().p66()},         // SSWB
		{SIMDInstructionInfo(0x6b).i().p66()},         // SSDW
		{SIMDInstructionInfo(0x67).i().p66()},         // USWB
		{SIMDInstructionInfo(0x2b).i().p66().m0f38()}, // USDW
	};

	const xImplSimd_Unpack xUNPCK =
	{
		{SIMDInstructionInfo(0x15).f()},       // HPS
		{SIMDInstructionInfo(0x15).d().p66()}, // HPD
		{SIMDInstructionInfo(0x14).f()},       // LPS
		{SIMDInstructionInfo(0x14).d().p66()}, // LPD
	};

	const xImplSimd_PInsert xPINSR;
	const SimdImpl_PExtract xPEXTR;

	const xImplSimd_MoveSSE xMOVAPS = {
		SIMDInstructionInfo(0x28).mov(), SIMDInstructionInfo(0x29).mov(),
		SIMDInstructionInfo(0x28).mov(), SIMDInstructionInfo(0x29).mov(),
	};
	const xImplSimd_MoveSSE xMOVUPS = {
		SIMDInstructionInfo(0x28).mov(), SIMDInstructionInfo(0x29).mov(),
		SIMDInstructionInfo(0x10).mov(), SIMDInstructionInfo(0x11).mov(),
	};

	const xImplSimd_MoveSSE xMOVDQA = {
		SIMDInstructionInfo(0x6f).p66().mov(), SIMDInstructionInfo(0x7f).p66().mov(),
		SIMDInstructionInfo(0x6f).p66().mov(), SIMDInstructionInfo(0x7f).p66().mov(),
	};
	const xImplSimd_MoveSSE xMOVDQU = {
		SIMDInstructionInfo(0x6f).p66().mov(), SIMDInstructionInfo(0x7f).p66().mov(),
		SIMDInstructionInfo(0x6f).pf3().mov(), SIMDInstructionInfo(0x7f).pf3().mov(),
	};

	const xImplSimd_MoveSSE xMOVAPD = {
		SIMDInstructionInfo(0x28).p66().mov(), SIMDInstructionInfo(0x29).p66().mov(),
		SIMDInstructionInfo(0x28).p66().mov(), SIMDInstructionInfo(0x29).p66().mov(),
	};
	const xImplSimd_MoveSSE xMOVUPD = {
		SIMDInstructionInfo(0x28).p66().mov(), SIMDInstructionInfo(0x29).p66().mov(),
		SIMDInstructionInfo(0x10).p66().mov(), SIMDInstructionInfo(0x11).p66().mov(),
	};


	const xImplSimd_MovHL xMOVH = {SIMDInstructionInfo(0x16)};
	const xImplSimd_MovHL xMOVL = {SIMDInstructionInfo(0x12)};

	const xImplSimd_MovHL_RtoR xMOVLH = {SIMDInstructionInfo(0x16)};
	const xImplSimd_MovHL_RtoR xMOVHL = {SIMDInstructionInfo(0x12)};

	const xImplSimd_PBlend xPBLEND =
	{
		{SIMDInstructionInfo(0x0e).i().p66().m0f3a()}, // W
		{SIMDInstructionInfo(0x10).i().p66().m0f38(), SIMDInstructionInfo(0x4c).i().p66().m0f3a()}, // VB
	};

	const xImplSimd_Blend xBLEND =
	{
		{SIMDInstructionInfo(0x0c).p66().f().m0f3a()}, // PS
		{SIMDInstructionInfo(0x0d).p66().d().m0f3a()}, // PD
		{SIMDInstructionInfo(0x14).p66().f().m0f38(), SIMDInstructionInfo(0x4a).f().p66().m0f3a()}, // VPS
		{SIMDInstructionInfo(0x15).p66().d().m0f38(), SIMDInstructionInfo(0x4b).d().p66().m0f3a()}, // VPD
	};

	const xImplSimd_PMove xPMOVSX = {SIMDInstructionInfo(0x20).p66().m0f38().mov()};
	const xImplSimd_PMove xPMOVZX = {SIMDInstructionInfo(0x30).p66().m0f38().mov()};

	// [SSE-3]
	const xImplSimd_2Arg xMOVSLDUP = {SIMDInstructionInfo(0x12).pf3()};

	// [SSE-3]
	const xImplSimd_2Arg xMOVSHDUP = {SIMDInstructionInfo(0x16).pf3()};

	// clang-format on
} // namespace x86Emitter
