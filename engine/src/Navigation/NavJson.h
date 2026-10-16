#pragma once
#include "Engine/Navigation/NavTypes.h"

#include <nlohmann/json.hpp>

namespace Engine {

// .ungineproj -> "navigation". Missing fields keep their value; the result is sanitized.
[[nodiscard]] nlohmann::json NavSettingsToJson(const NavSettings& settings);
void                         NavSettingsFromJson(const nlohmann::json& j, NavSettings& settings);

} // namespace Engine
