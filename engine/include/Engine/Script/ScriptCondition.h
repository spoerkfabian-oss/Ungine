#pragma once
#include "Engine/Script/ScriptValue.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Engine {

// Breakpoint conditions: an expression over the script's variables (a function's locals first),
// e.g. `health < 10 && !dead`, `name == "Bob"`, `count % 3 == 0`. Operators (by precedence):
// || / && / == != / < <= > >= / + - / * / % / unary ! -; parentheses; literals: numbers,
// "strings", true, false. Bools / ints / floats compute and compare as numbers, strings as text,
// other values (vectors, entities, containers) only with == / != or as text.

// Syntax check: the error, empty if the expression is fine (names are not checked).
[[nodiscard]] std::string CheckScriptCondition(std::string_view expression);
// The result; nullopt on errors (syntax, unknown variable: `variable` returns null), with `error`.
[[nodiscard]] std::optional<bool> EvaluateScriptCondition(std::string_view expression,
                                                          const std::function<const ScriptValue*(std::string_view)>& variable,
                                                          std::string* error = nullptr);

} // namespace Engine
