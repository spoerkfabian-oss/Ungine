#include "Engine/Script/ScriptCondition.h"

#include <cctype>
#include <cmath>
#include <stdexcept>

namespace Engine {

namespace {

bool IsNumeric(const ScriptValue& v)
{
    return std::holds_alternative<bool>(v) || std::holds_alternative<std::int32_t>(v) || std::holds_alternative<float>(v);
}

float Number(const ScriptValue& v) { return std::get<float>(Convert(v, PinType::Float)); }

bool Truthy(const ScriptValue& v)
{
    if (IsNumeric(v))
        return Number(v) != 0.0f;
    if (const auto* s = std::get_if<std::string>(&v))
        return !s->empty();
    return !ValuesEqual(v, DefaultValue(TypeOf(v)));
}

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Recursive descent; evaluates while parsing (a missing variable is an error).
class Parser {
public:
    Parser(std::string_view text, const std::function<const ScriptValue*(std::string_view)>& variable)
        : m_Text(text), m_Variable(variable)
    {
    }

    ScriptValue Run()
    {
        ScriptValue v = Or();
        Skip();
        if (m_Pos != m_Text.size())
            throw ParseError("unexpected '" + std::string(m_Text.substr(m_Pos, 12)) + "'");
        return v;
    }

private:
    void Skip()
    {
        while (m_Pos < m_Text.size() && std::isspace(static_cast<unsigned char>(m_Text[m_Pos])))
            ++m_Pos;
    }
    bool Eat(std::string_view token)
    {
        Skip();
        if (m_Text.substr(m_Pos, token.size()) != token)
            return false;
        // "<" must not eat the start of "<=" etc.
        if (token.size() == 1 && m_Pos + 1 < m_Text.size() && m_Text[m_Pos + 1] == '=' && std::string_view("<>!=").find(token[0]) != std::string_view::npos)
            return false;
        m_Pos += token.size();
        return true;
    }

    ScriptValue Or()
    {
        ScriptValue v = And();
        while (Eat("||")) {
            const ScriptValue rhs = And();
            v                     = Truthy(v) || Truthy(rhs);
        }
        return v;
    }
    ScriptValue And()
    {
        ScriptValue v = Equality();
        while (Eat("&&")) {
            const ScriptValue rhs = Equality();
            v                     = Truthy(v) && Truthy(rhs);
        }
        return v;
    }
    ScriptValue Equality()
    {
        ScriptValue v = Relational();
        for (;;) {
            const bool eq = Eat("==");
            if (!eq && !Eat("!="))
                return v;
            const ScriptValue rhs   = Relational();
            const bool        equal = IsNumeric(v) && IsNumeric(rhs) ? Number(v) == Number(rhs)
                                      : std::holds_alternative<std::string>(v) || std::holds_alternative<std::string>(rhs)
                                          ? ToDisplayString(v) == ToDisplayString(rhs)
                                          : ValuesEqual(v, rhs);
            v = eq ? equal : !equal;
        }
    }
    ScriptValue Relational()
    {
        ScriptValue v = Additive();
        for (;;) {
            int op = 0;
            if (Eat("<="))
                op = 1;
            else if (Eat(">="))
                op = 2;
            else if (Eat("<"))
                op = 3;
            else if (Eat(">"))
                op = 4;
            else
                return v;
            const ScriptValue rhs = Additive();
            int               cmp = 0;
            if (IsNumeric(v) && IsNumeric(rhs)) {
                const float a = Number(v), b = Number(rhs);
                cmp           = a < b ? -1 : a > b ? 1 : 0;
            } else {
                cmp = ToDisplayString(v).compare(ToDisplayString(rhs));
            }
            v = op == 1 ? cmp <= 0 : op == 2 ? cmp >= 0 : op == 3 ? cmp < 0 : cmp > 0;
        }
    }
    ScriptValue Additive()
    {
        ScriptValue v = Multiplicative();
        for (;;) {
            const bool plus = Eat("+");
            if (!plus && !Eat("-"))
                return v;
            const ScriptValue rhs = Multiplicative();
            if (plus && !(IsNumeric(v) && IsNumeric(rhs)))
                v = ToDisplayString(v) + ToDisplayString(rhs); // text concatenation
            else
                v = plus ? Number(v) + Number(rhs) : Number(v) - Number(rhs);
        }
    }
    ScriptValue Multiplicative()
    {
        ScriptValue v = Unary();
        for (;;) {
            char op = 0;
            if (Eat("*"))
                op = '*';
            else if (Eat("/"))
                op = '/';
            else if (Eat("%"))
                op = '%';
            else
                return v;
            const float a = Number(v), b = Number(Unary());
            v = op == '*' ? a * b : b == 0.0f ? 0.0f : op == '/' ? a / b : std::fmod(a, b);
        }
    }
    ScriptValue Unary()
    {
        if (Eat("!"))
            return !Truthy(Unary());
        if (Eat("-"))
            return -Number(Unary());
        return Primary();
    }
    ScriptValue Primary()
    {
        Skip();
        if (m_Pos >= m_Text.size())
            throw ParseError("expression ends early");
        const char c = m_Text[m_Pos];
        if (c == '(') {
            ++m_Pos;
            ScriptValue v = Or();
            if (!Eat(")"))
                throw ParseError("missing ')'");
            return v;
        }
        if (c == '"') {
            std::string s;
            for (++m_Pos; m_Pos < m_Text.size() && m_Text[m_Pos] != '"'; ++m_Pos) {
                if (m_Text[m_Pos] == '\\' && m_Pos + 1 < m_Text.size())
                    ++m_Pos;
                s += m_Text[m_Pos];
            }
            if (m_Pos >= m_Text.size())
                throw ParseError("missing '\"'");
            ++m_Pos;
            return s;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            const std::size_t start = m_Pos;
            while (m_Pos < m_Text.size() && (std::isdigit(static_cast<unsigned char>(m_Text[m_Pos])) || m_Text[m_Pos] == '.'))
                ++m_Pos;
            try {
                return std::stof(std::string(m_Text.substr(start, m_Pos - start)));
            } catch (const std::exception&) {
                throw ParseError("bad number");
            }
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const std::size_t start = m_Pos;
            while (m_Pos < m_Text.size() && (std::isalnum(static_cast<unsigned char>(m_Text[m_Pos])) || m_Text[m_Pos] == '_'))
                ++m_Pos;
            const std::string_view name = m_Text.substr(start, m_Pos - start);
            if (name == "true" || name == "false")
                return name == "true";
            if (const ScriptValue* v = m_Variable(name))
                return *v;
            throw ParseError("unknown variable '" + std::string(name) + "'");
        }
        throw ParseError(std::string("unexpected '") + c + "'");
    }

    std::string_view                                            m_Text;
    const std::function<const ScriptValue*(std::string_view)>& m_Variable;
    std::size_t                                                 m_Pos = 0;
};

} // namespace

std::optional<bool> EvaluateScriptCondition(std::string_view expression,
                                            const std::function<const ScriptValue*(std::string_view)>& variable,
                                            std::string* error)
{
    try {
        return Truthy(Parser(expression, variable).Run());
    } catch (const ParseError& e) {
        if (error)
            *error = e.what();
        return std::nullopt;
    }
}

std::string CheckScriptCondition(std::string_view expression)
{
    static const ScriptValue any = 0.0f;
    std::string              error;
    (void)EvaluateScriptCondition(expression, [](std::string_view) { return &any; }, &error);
    return error;
}

} // namespace Engine
