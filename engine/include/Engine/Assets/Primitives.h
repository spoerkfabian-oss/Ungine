#pragma once
#include "Engine/Assets/Model.h"

#include <string>

namespace Engine {

// Generated geometry as ModelData (CPU only); hand it to AssetManager::CreateModel.

// Square in the XZ plane facing +Y, centered at the origin. UVs repeat once per `uvScale` units.
[[nodiscard]] ModelData MakePlane(std::string name, float size, const MaterialData& material, float uvScale = 1.0f);

} // namespace Engine
