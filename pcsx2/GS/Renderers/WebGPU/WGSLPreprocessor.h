// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <string_view>
#include <unordered_map>

class WGSLPreprocessor
{
public:
	using DefineMap = std::unordered_map<std::string, std::string>;

	WGSLPreprocessor();
	~WGSLPreprocessor();

	void Define(std::string_view name, std::string_view value);
	void Define(std::string_view name, s64 value);
	void Undefine(std::string_view name);
	bool IsDefined(std::string_view name) const;
	void Clear();

	const DefineMap& GetDefines() const { return m_defines; }

	bool Process(std::string_view source, std::string* output, std::string* error) const;

	static bool EvaluateExpression(std::string_view expression, const DefineMap& defines, s64* result, std::string* error);

private:
	DefineMap m_defines;
};
