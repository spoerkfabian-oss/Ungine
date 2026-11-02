#pragma once
// Worker side of material assets (AssetManager::LoadMaterial): resolves a .umat / .uminst chain,
// generates the code and compiles (or finds cached) shader variants. Engine-private.
#include "Engine/Material/MaterialAsset.h"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

// Compiled variants by key, shared across assets (weak: dropped with the last user) and backed
// by a directory of SPIR-V files. Thread-safe.
class MaterialShaderCache {
public:
    [[nodiscard]] std::shared_ptr<const MaterialShaders> Find(std::uint64_t key);
    void Store(const std::shared_ptr<const MaterialShaders>& shaders);

private:
    std::mutex                                                          m_Mutex;
    std::unordered_map<std::uint64_t, std::weak_ptr<const MaterialShaders>> m_Shaders;
};

struct MaterialBuildInput {
    std::filesystem::path file; // normalized
    // Unsaved editor content by normalized path (generic UTF-8).
    std::map<std::string, MaterialGraph>        providedGraphs;
    std::map<std::string, MaterialInstanceFile> providedInstances;
    std::filesystem::path shaderCacheDirectory; // empty: memory only
};

struct MaterialBuildOutput {
    std::unique_ptr<MaterialAsset>     material; // textures not acquired yet (block texture slots: 0)
    std::vector<std::filesystem::path> dependencies; // every file of the chain
    std::string                        error;
};

// Absolute + canonical (like the AssetManager's cache keys) and its generic UTF-8 key.
[[nodiscard]] std::filesystem::path NormalizeMaterialPath(const std::filesystem::path& path);
[[nodiscard]] std::string           MaterialPathKey(const std::filesystem::path& normalized);
[[nodiscard]] MaterialBuildOutput   BuildMaterial(const MaterialBuildInput& input, MaterialShaderCache& cache);

} // namespace Engine
