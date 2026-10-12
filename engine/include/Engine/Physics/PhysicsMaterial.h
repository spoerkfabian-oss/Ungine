#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Engine {

inline constexpr std::string_view kPhysicsMaterialExtension = ".uphysmat";

// Physics material asset (.uphysmat, JSON): how a surface collides and what it is called. Colliders
// reference it (Collider::material, per glTF material of a mesh collider in meshMaterials); hits and
// contacts report the surface (e.g. footstep or impact sounds per surface).
// Contact friction = sqrt(f1 * f2), restitution = max(r1, r2).
struct PhysicsMaterialData {
    float       friction    = 0.5f;
    float       restitution = 0.0f;
    std::string surface     = "Default";

    bool operator==(const PhysicsMaterialData&) const = default;
};

// Reads through the virtual file system (packed games too). nullopt: missing or broken (error set).
[[nodiscard]] std::optional<PhysicsMaterialData> LoadPhysicsMaterial(const std::filesystem::path& file,
                                                                     std::string* error = nullptr);
bool SavePhysicsMaterial(const std::filesystem::path& file, const PhysicsMaterialData& material,
                         std::string* error = nullptr);

} // namespace Engine
