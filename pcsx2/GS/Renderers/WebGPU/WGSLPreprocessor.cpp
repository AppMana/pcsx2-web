// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/WebGPU/WGSLPreprocessor.h"

#include "fmt/format.h"

#include <cctype>
#include <vector>

namespace
{
	constexpr u32 MAX_MACRO_DEPTH = 16;

	bool IsIdentStart(char c)
	{
		return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
	}

	bool IsIdentChar(char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
	}

	std::string_view Trim(std::string_view s)
	{
		while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
			s.remove_prefix(1);
		while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
			s.remove_suffix(1);
		return s;
	}

	std::string_view StripLineComment(std::string_view s)
	{
		const size_t pos = s.find("//");
		return (pos == std::string_view::npos) ? s : s.substr(0, pos);
	}

	class ExpressionParser
	{
	public:
		ExpressionParser(std::string_view expr, const WGSLPreprocessor::DefineMap& defines, u32 depth)
			: m_expr(expr)
			, m_defines(defines)
			, m_depth(depth)
		{
		}

		bool Parse(s64* result)
		{
			SkipSpace();
			if (!ParseTernary(result))
				return false;
			SkipSpace();
			if (m_pos != m_expr.size())
				return Fail(fmt::format("unexpected trailing characters at position {}", m_pos));
			return true;
		}

		const std::string& GetError() const { return m_error; }

	private:
		bool Fail(std::string msg)
		{
			if (m_error.empty())
				m_error = std::move(msg);
			return false;
		}

		void SkipSpace()
		{
			while (m_pos < m_expr.size() && std::isspace(static_cast<unsigned char>(m_expr[m_pos])))
				m_pos++;
		}

		bool Peek(std::string_view tok)
		{
			SkipSpace();
			return m_expr.substr(m_pos, tok.size()) == tok;
		}

		bool Accept(std::string_view tok)
		{
			if (!Peek(tok))
				return false;
			m_pos += tok.size();
			return true;
		}

		bool ParseTernary(s64* result)
		{
			s64 cond;
			if (!ParseLogicalOr(&cond))
				return false;
			if (!Accept("?"))
			{
				*result = cond;
				return true;
			}
			s64 a, b;
			if (!ParseTernary(&a))
				return false;
			if (!Accept(":"))
				return Fail("expected ':' in ternary expression");
			if (!ParseTernary(&b))
				return false;
			*result = cond ? a : b;
			return true;
		}

		bool ParseLogicalOr(s64* result)
		{
			if (!ParseLogicalAnd(result))
				return false;
			while (Accept("||"))
			{
				s64 rhs;
				if (!ParseLogicalAnd(&rhs))
					return false;
				*result = (*result != 0 || rhs != 0) ? 1 : 0;
			}
			return true;
		}

		bool ParseLogicalAnd(s64* result)
		{
			if (!ParseBitOr(result))
				return false;
			while (Accept("&&"))
			{
				s64 rhs;
				if (!ParseBitOr(&rhs))
					return false;
				*result = (*result != 0 && rhs != 0) ? 1 : 0;
			}
			return true;
		}

		bool ParseBitOr(s64* result)
		{
			if (!ParseBitXor(result))
				return false;
			for (;;)
			{
				SkipSpace();
				if (m_expr.substr(m_pos, 2) == "||" || !Accept("|"))
					break;
				s64 rhs;
				if (!ParseBitXor(&rhs))
					return false;
				*result |= rhs;
			}
			return true;
		}

		bool ParseBitXor(s64* result)
		{
			if (!ParseBitAnd(result))
				return false;
			while (Accept("^"))
			{
				s64 rhs;
				if (!ParseBitAnd(&rhs))
					return false;
				*result ^= rhs;
			}
			return true;
		}

		bool ParseBitAnd(s64* result)
		{
			if (!ParseEquality(result))
				return false;
			for (;;)
			{
				SkipSpace();
				if (m_expr.substr(m_pos, 2) == "&&" || !Accept("&"))
					break;
				s64 rhs;
				if (!ParseEquality(&rhs))
					return false;
				*result &= rhs;
			}
			return true;
		}

		bool ParseEquality(s64* result)
		{
			if (!ParseRelational(result))
				return false;
			for (;;)
			{
				if (Accept("=="))
				{
					s64 rhs;
					if (!ParseRelational(&rhs))
						return false;
					*result = (*result == rhs) ? 1 : 0;
				}
				else if (Accept("!="))
				{
					s64 rhs;
					if (!ParseRelational(&rhs))
						return false;
					*result = (*result != rhs) ? 1 : 0;
				}
				else
				{
					break;
				}
			}
			return true;
		}

		bool ParseRelational(s64* result)
		{
			if (!ParseShift(result))
				return false;
			for (;;)
			{
				SkipSpace();
				if (Accept("<="))
				{
					s64 rhs;
					if (!ParseShift(&rhs))
						return false;
					*result = (*result <= rhs) ? 1 : 0;
				}
				else if (Accept(">="))
				{
					s64 rhs;
					if (!ParseShift(&rhs))
						return false;
					*result = (*result >= rhs) ? 1 : 0;
				}
				else if (m_expr.substr(m_pos, 2) != "<<" && Accept("<"))
				{
					s64 rhs;
					if (!ParseShift(&rhs))
						return false;
					*result = (*result < rhs) ? 1 : 0;
				}
				else if (m_expr.substr(m_pos, 2) != ">>" && Accept(">"))
				{
					s64 rhs;
					if (!ParseShift(&rhs))
						return false;
					*result = (*result > rhs) ? 1 : 0;
				}
				else
				{
					break;
				}
			}
			return true;
		}

		bool ParseShift(s64* result)
		{
			if (!ParseAdditive(result))
				return false;
			for (;;)
			{
				if (Accept("<<"))
				{
					s64 rhs;
					if (!ParseAdditive(&rhs))
						return false;
					*result = static_cast<s64>(static_cast<u64>(*result) << (rhs & 63));
				}
				else if (Accept(">>"))
				{
					s64 rhs;
					if (!ParseAdditive(&rhs))
						return false;
					*result = *result >> (rhs & 63);
				}
				else
				{
					break;
				}
			}
			return true;
		}

		bool ParseAdditive(s64* result)
		{
			if (!ParseMultiplicative(result))
				return false;
			for (;;)
			{
				if (Accept("+"))
				{
					s64 rhs;
					if (!ParseMultiplicative(&rhs))
						return false;
					*result += rhs;
				}
				else if (Accept("-"))
				{
					s64 rhs;
					if (!ParseMultiplicative(&rhs))
						return false;
					*result -= rhs;
				}
				else
				{
					break;
				}
			}
			return true;
		}

		bool ParseMultiplicative(s64* result)
		{
			if (!ParseUnary(result))
				return false;
			for (;;)
			{
				if (Accept("*"))
				{
					s64 rhs;
					if (!ParseUnary(&rhs))
						return false;
					*result *= rhs;
				}
				else if (Accept("/"))
				{
					s64 rhs;
					if (!ParseUnary(&rhs))
						return false;
					if (rhs == 0)
						return Fail("division by zero");
					*result /= rhs;
				}
				else if (Accept("%"))
				{
					s64 rhs;
					if (!ParseUnary(&rhs))
						return false;
					if (rhs == 0)
						return Fail("modulo by zero");
					*result %= rhs;
				}
				else
				{
					break;
				}
			}
			return true;
		}

		bool ParseUnary(s64* result)
		{
			if (Accept("!"))
			{
				if (!ParseUnary(result))
					return false;
				*result = (*result == 0) ? 1 : 0;
				return true;
			}
			if (Accept("~"))
			{
				if (!ParseUnary(result))
					return false;
				*result = ~*result;
				return true;
			}
			if (Accept("-"))
			{
				if (!ParseUnary(result))
					return false;
				*result = -*result;
				return true;
			}
			if (Accept("+"))
				return ParseUnary(result);
			return ParsePrimary(result);
		}

		bool ParseNumber(s64* result)
		{
			const size_t start = m_pos;
			s64 value = 0;
			if (m_expr.substr(m_pos, 2) == "0x" || m_expr.substr(m_pos, 2) == "0X")
			{
				m_pos += 2;
				const size_t digits_start = m_pos;
				while (m_pos < m_expr.size() && std::isxdigit(static_cast<unsigned char>(m_expr[m_pos])))
				{
					const char c = m_expr[m_pos++];
					const s64 digit = std::isdigit(static_cast<unsigned char>(c)) ? (c - '0') : (std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
					value = static_cast<s64>((static_cast<u64>(value) << 4) | static_cast<u64>(digit));
				}
				if (m_pos == digits_start)
					return Fail(fmt::format("malformed hex literal at position {}", start));
			}
			else
			{
				while (m_pos < m_expr.size() && std::isdigit(static_cast<unsigned char>(m_expr[m_pos])))
					value = value * 10 + (m_expr[m_pos++] - '0');
			}
			while (m_pos < m_expr.size() && (m_expr[m_pos] == 'u' || m_expr[m_pos] == 'U' || m_expr[m_pos] == 'l' || m_expr[m_pos] == 'L'))
				m_pos++;
			if (m_pos < m_expr.size() && IsIdentChar(m_expr[m_pos]))
				return Fail(fmt::format("malformed number at position {}", start));
			*result = value;
			return true;
		}

		bool ParseIdentifier(std::string_view* ident)
		{
			SkipSpace();
			const size_t start = m_pos;
			if (m_pos >= m_expr.size() || !IsIdentStart(m_expr[m_pos]))
				return Fail(fmt::format("expected identifier at position {}", start));
			while (m_pos < m_expr.size() && IsIdentChar(m_expr[m_pos]))
				m_pos++;
			*ident = m_expr.substr(start, m_pos - start);
			return true;
		}

		bool ParsePrimary(s64* result)
		{
			SkipSpace();
			if (m_pos >= m_expr.size())
				return Fail("unexpected end of expression");

			if (Accept("("))
			{
				if (!ParseTernary(result))
					return false;
				if (!Accept(")"))
					return Fail("expected ')'");
				return true;
			}

			const char c = m_expr[m_pos];
			if (std::isdigit(static_cast<unsigned char>(c)))
				return ParseNumber(result);

			if (!IsIdentStart(c))
				return Fail(fmt::format("unexpected character '{}' at position {}", c, m_pos));

			std::string_view ident;
			if (!ParseIdentifier(&ident))
				return false;

			if (ident == "defined")
			{
				const bool paren = Accept("(");
				std::string_view name;
				if (!ParseIdentifier(&name))
					return false;
				if (paren && !Accept(")"))
					return Fail("expected ')' after defined");
				*result = (m_defines.find(std::string(name)) != m_defines.end()) ? 1 : 0;
				return true;
			}

			if (ident == "true")
			{
				*result = 1;
				return true;
			}
			if (ident == "false")
			{
				*result = 0;
				return true;
			}

			const auto it = m_defines.find(std::string(ident));
			if (it == m_defines.end())
			{
				*result = 0;
				return true;
			}

			const std::string_view value = Trim(it->second);
			if (value.empty())
			{
				*result = 1;
				return true;
			}

			if (m_depth >= MAX_MACRO_DEPTH)
				return Fail(fmt::format("macro expansion too deep at '{}'", ident));

			ExpressionParser sub(value, m_defines, m_depth + 1);
			if (!sub.Parse(result))
				return Fail(fmt::format("in expansion of '{}': {}", ident, sub.GetError()));
			return true;
		}

		std::string_view m_expr;
		const WGSLPreprocessor::DefineMap& m_defines;
		size_t m_pos = 0;
		u32 m_depth;
		std::string m_error;
	};

	struct ConditionalState
	{
		bool parent_active;
		bool active;
		bool taken;
		bool seen_else;
	};

	void SubstituteMacros(std::string_view line, const WGSLPreprocessor::DefineMap& defines, std::string* out)
	{
		std::string current(line);
		for (u32 pass = 0; pass < MAX_MACRO_DEPTH; pass++)
		{
			std::string next;
			next.reserve(current.size());
			bool changed = false;
			size_t i = 0;
			while (i < current.size())
			{
				const char c = current[i];
				if (IsIdentStart(c))
				{
					size_t j = i + 1;
					while (j < current.size() && IsIdentChar(current[j]))
						j++;
					const std::string_view ident(current.data() + i, j - i);
					const auto it = defines.find(std::string(ident));
					if (it != defines.end() && !it->second.empty())
					{
						next.append(it->second);
						changed = true;
					}
					else
					{
						next.append(ident);
					}
					i = j;
				}
				else if (std::isdigit(static_cast<unsigned char>(c)))
				{
					size_t j = i + 1;
					while (j < current.size() && (IsIdentChar(current[j]) || current[j] == '.'))
						j++;
					next.append(current, i, j - i);
					i = j;
				}
				else
				{
					next.push_back(c);
					i++;
				}
			}
			current = std::move(next);
			if (!changed)
				break;
		}
		out->append(current);
	}
} // namespace

WGSLPreprocessor::WGSLPreprocessor() = default;

WGSLPreprocessor::~WGSLPreprocessor() = default;

void WGSLPreprocessor::Define(std::string_view name, std::string_view value)
{
	m_defines[std::string(name)] = std::string(Trim(value));
}

void WGSLPreprocessor::Define(std::string_view name, s64 value)
{
	m_defines[std::string(name)] = fmt::format("{}", value);
}

void WGSLPreprocessor::Undefine(std::string_view name)
{
	m_defines.erase(std::string(name));
}

bool WGSLPreprocessor::IsDefined(std::string_view name) const
{
	return m_defines.find(std::string(name)) != m_defines.end();
}

void WGSLPreprocessor::Clear()
{
	m_defines.clear();
}

bool WGSLPreprocessor::EvaluateExpression(std::string_view expression, const DefineMap& defines, s64* result, std::string* error)
{
	ExpressionParser parser(StripLineComment(expression), defines, 0);
	if (!parser.Parse(result))
	{
		if (error)
			*error = parser.GetError();
		return false;
	}
	return true;
}

bool WGSLPreprocessor::Process(std::string_view source, std::string* output, std::string* error) const
{
	DefineMap defines = m_defines;
	std::vector<ConditionalState> stack;
	output->clear();
	output->reserve(source.size());

	const auto fail = [&](u32 line_number, std::string msg) {
		if (error)
			*error = fmt::format("line {}: {}", line_number, msg);
		return false;
	};

	const auto is_active = [&]() { return stack.empty() || stack.back().active; };

	u32 line_number = 0;
	size_t pos = 0;
	while (pos <= source.size())
	{
		const size_t eol = source.find('\n', pos);
		const std::string_view line = source.substr(pos, (eol == std::string_view::npos) ? std::string_view::npos : (eol - pos));
		const bool last_line = (eol == std::string_view::npos);
		line_number++;

		const std::string_view trimmed = Trim(line);
		if (!trimmed.empty() && trimmed.front() == '#')
		{
			std::string_view directive = Trim(StripLineComment(trimmed.substr(1)));
			size_t name_end = 0;
			while (name_end < directive.size() && IsIdentChar(directive[name_end]))
				name_end++;
			const std::string_view name = directive.substr(0, name_end);
			const std::string_view args = Trim(directive.substr(name_end));

			if (name == "if")
			{
				const bool parent_active = is_active();
				bool value = false;
				if (parent_active)
				{
					s64 result;
					std::string expr_error;
					if (!EvaluateExpression(args, defines, &result, &expr_error))
						return fail(line_number, fmt::format("#if: {}", expr_error));
					value = (result != 0);
				}
				stack.push_back({parent_active, parent_active && value, value, false});
			}
			else if (name == "ifdef" || name == "ifndef")
			{
				const bool parent_active = is_active();
				if (args.empty() || !IsIdentStart(args.front()))
					return fail(line_number, fmt::format("#{} requires an identifier", name));
				const bool defined = (defines.find(std::string(args)) != defines.end());
				const bool value = (name == "ifdef") ? defined : !defined;
				stack.push_back({parent_active, parent_active && value, value, false});
			}
			else if (name == "elif")
			{
				if (stack.empty())
					return fail(line_number, "#elif without #if");
				ConditionalState& state = stack.back();
				if (state.seen_else)
					return fail(line_number, "#elif after #else");
				bool value = false;
				if (state.parent_active && !state.taken)
				{
					s64 result;
					std::string expr_error;
					if (!EvaluateExpression(args, defines, &result, &expr_error))
						return fail(line_number, fmt::format("#elif: {}", expr_error));
					value = (result != 0);
				}
				state.active = state.parent_active && !state.taken && value;
				state.taken = state.taken || value;
			}
			else if (name == "else")
			{
				if (stack.empty())
					return fail(line_number, "#else without #if");
				ConditionalState& state = stack.back();
				if (state.seen_else)
					return fail(line_number, "duplicate #else");
				state.seen_else = true;
				state.active = state.parent_active && !state.taken;
				state.taken = true;
			}
			else if (name == "endif")
			{
				if (stack.empty())
					return fail(line_number, "#endif without #if");
				stack.pop_back();
			}
			else if (name == "define")
			{
				if (is_active())
				{
					size_t ident_end = 0;
					while (ident_end < args.size() && IsIdentChar(args[ident_end]))
						ident_end++;
					if (ident_end == 0 || !IsIdentStart(args.front()))
						return fail(line_number, "#define requires an identifier");
					if (ident_end < args.size() && args[ident_end] == '(')
						return fail(line_number, "function-like macros are not supported");
					defines[std::string(args.substr(0, ident_end))] = std::string(Trim(args.substr(ident_end)));
				}
			}
			else if (name == "undef")
			{
				if (is_active())
				{
					if (args.empty() || !IsIdentStart(args.front()))
						return fail(line_number, "#undef requires an identifier");
					defines.erase(std::string(args));
				}
			}
			else if (name == "error")
			{
				if (is_active())
					return fail(line_number, fmt::format("#error {}", args));
			}
			else
			{
				if (is_active())
					return fail(line_number, fmt::format("unsupported directive '#{}'", name));
			}
		}
		else if (is_active())
		{
			SubstituteMacros(line, defines, output);
		}

		if (!last_line)
			output->push_back('\n');

		if (last_line)
			break;
		pos = eol + 1;
	}

	if (!stack.empty())
		return fail(line_number, "unterminated conditional block");

	return true;
}
