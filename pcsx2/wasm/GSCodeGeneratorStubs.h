// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSFunctionMap.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"

#include "common/Assertions.h"

// Satisfies the GSCodeGeneratorFunctionMap members of GSDrawScanline; the software rasterizer never
// instantiates these on wasm32 because ENABLE_JIT_RASTERIZER is off and the C++ scanline path is used.

class GSSetupPrimCodeGenerator
{
public:
	GSSetupPrimCodeGenerator(u64 key, void* code, size_t maxsize) { pxFailRel("No scanline JIT on wasm32."); }
	void Generate() {}
	size_t GetSize() const { return 0; }
	const u8* GetCode() const { return nullptr; }
};

class GSDrawScanlineCodeGenerator
{
public:
	GSDrawScanlineCodeGenerator(u64 key, void* code, size_t maxsize) { pxFailRel("No scanline JIT on wasm32."); }
	void Generate() {}
	size_t GetSize() const { return 0; }
	const u8* GetCode() const { return nullptr; }
};
