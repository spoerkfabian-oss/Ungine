#pragma once
#include "Engine/Assets/Model.h"

#include <optional>
#include <string>
#include <string_view>

namespace Engine {

// Generated geometry as ModelData (CPU only); hand it to AssetManager::CreateModel.

// Square in the XZ plane facing +Y, centered at the origin. UVs repeat once per `uvScale` units.
[[nodiscard]] ModelData MakePlane(std::string name, float size, const MaterialData& material, float uvScale = 1.0f);

// Cube of edge `size` centered at the origin, 24 vertices (flat faces), UV 0..1 per face.
[[nodiscard]] ModelData MakeBox(std::string name, float size, const MaterialData& material);

// Y-up capsule: hemispheres of `radius` joined by a cylinder of height 2 * halfHeight (0 = sphere).
// Smooth normals, UV u around the axis, v from top to bottom.
[[nodiscard]] ModelData MakeCapsule(std::string name, float radius, float halfHeight, const MaterialData& material,
                                    std::uint32_t segments = 32, std::uint32_t rings = 16);

// size: plane edge, box edge, sphere diameter, capsule diameter (total height 2 * size, like
// the default Collider).
enum class PrimitiveShape : std::uint8_t { Plane, Box, Sphere, Capsule };

// Serializable recipe of a generated model (AssetManager::CreatePrimitive, scene files).
struct PrimitiveDesc {
    PrimitiveShape shape = PrimitiveShape::Box;
    float          size  = 1.0f;
    glm::vec4      baseColor{0.8f, 0.8f, 0.8f, 1.0f};
    float          metallic  = 0.0f;
    float          roughness = 0.6f;

    bool operator==(const PrimitiveDesc&) const = default;
};

[[nodiscard]] const char*                    ToString(PrimitiveShape shape);
[[nodiscard]] std::optional<PrimitiveShape> PrimitiveShapeFromString(std::string_view name);
[[nodiscard]] ModelData   MakePrimitive(const PrimitiveDesc& desc);
[[nodiscard]] std::string PrimitiveKey(const PrimitiveDesc& desc); // canonical, used as cache key

} // namespace Engine
