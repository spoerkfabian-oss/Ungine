#pragma once
#include "Engine/Assets/Model.h"

#include <filesystem>

namespace Engine {

// Parses .gltf/.glb (external, embedded and data-URI buffers). Textures are returned as sources
// (embedded bytes or external files); the AssetManager decodes and compresses them.
// Supported: triangle meshes, metallic-roughness materials, node hierarchy of the default scene.
// Not yet: skins, animations, morph targets, KHR_texture_basisu/webp, multiple UV sets.
// Throws std::runtime_error on unrecoverable errors.
[[nodiscard]] ModelData LoadGltf(const std::filesystem::path& path);

} // namespace Engine
