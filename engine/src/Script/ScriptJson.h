#pragma once
// Engine-private: script values in JSON (graphs, scene files).
#include "Engine/Script/ScriptValue.h"

#include <nlohmann/json.hpp>

namespace Engine {

// Scalars as JSON values, vec3 as [x, y, z], arrays as lists; entities as null (runtime values).
[[nodiscard]] nlohmann::json ScriptValueToJson(const ScriptValue& value);
// A value of `type` (wrong JSON types give the default).
[[nodiscard]] ScriptValue ScriptValueFromJson(const nlohmann::json& j, PinType type);

} // namespace Engine
