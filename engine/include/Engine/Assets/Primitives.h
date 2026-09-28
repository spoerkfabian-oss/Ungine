#pragma once
#include "Engine/Assets/Model.h"

#include <string>

namespace Engine {

// Generated geometry as ModelData (CPU only); hand it to AssetManager::CreateModel.

// Square in the XZ plane facing +Y, centered at the origin. UVs repeat once per `uvScale` units.
[[nodiscard]] ModelData MakePlane(std::string name, float size, const MaterialData& material, float uvScale = 1.0f);

// Cube of edge `size` centered at the origin, 24 vertices (flat faces), UV 0..1 per face.
[[nodiscard]] ModelData MakeBox(std::string name, float size, const MaterialData& material);

enum class PrimitiveShape : std::uint8_t { Plane, Box };

// Serializable recipe of a generated model (AssetManager::CreatePrimitive, scene files).
struct PrimitiveDesc {
    PrimitiveShape shape = PrimitiveShape::Box;
    float          size  = 1.0f;
    glm::vec4      baseColor{0.8f, 0.8f, 0.8f, 1.0f};
    float          metallic  = 0.0f;
    float          roughness = 0.6f;

    bool operator==(const PrimitiveDesc&) const = default;
};

[[nodiscard]] const char* ToString(PrimitiveShape shape);
[[nodiscard]] ModelData   MakePrimitive(const PrimitiveDesc& desc);
[[nodiscard]] std::string PrimitiveKey(const PrimitiveDesc& desc); // canonical, used as cache key

} // namespace Engine
