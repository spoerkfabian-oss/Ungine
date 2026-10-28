#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Material/MaterialGraph.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

// Compiled shader variants of a material (shared by every instance with the same code).
struct MaterialShaders {
    std::array<std::vector<std::uint32_t>, static_cast<std::size_t>(MaterialShader::Count)> spirv{};
    std::uint64_t key = 0; // code + defines + engine shader sources
    [[nodiscard]] const std::vector<std::uint32_t>& Get(MaterialShader shader) const
    {
        return spirv[static_cast<std::size_t>(shader)];
    }
};

// A loaded .umat / .uminst (AssetManager::LoadMaterial): settings, shaders and the default
// parameter block (texture slots hold texture table entries as float bits).
struct MaterialAsset {
    std::string                            graphFile; // the .umat at the root of the instance chain
    MaterialSettings                       settings{};
    std::shared_ptr<const MaterialShaders> shaders;
    std::vector<MaterialSlot>              slots;
    std::vector<MaterialParameter>         parameters; // instance overrides applied
    std::vector<glm::vec4>                 block;      // one vec4 per slot
    std::vector<TextureHandle>             textures;   // held by the asset
    std::vector<MaterialProblem>           warnings;
    bool                                   shaderCacheHit = false; // memory or disk

    // Slot of a scalar / vector parameter (~0u: none).
    [[nodiscard]] std::uint32_t ParameterSlot(std::string_view name) const;
};

using MaterialHandle = AssetHandle<MaterialAsset>;

struct MaterialInfo {
    MaterialHandle handle;
    AssetState     state    = AssetState::Invalid;
    std::uint32_t  refCount = 0;
    std::string    path;
    std::string    error;
    std::uint32_t  revision  = 0;
    bool           reloading = false;
    bool           instance  = false; // .uminst
    std::string    graphFile;
    MaterialBlendMode blendMode = MaterialBlendMode::Opaque;
    std::uint32_t  slots     = 0;
    std::uint32_t  textures  = 0;
    bool           shaderCacheHit = false;
};

} // namespace Engine
