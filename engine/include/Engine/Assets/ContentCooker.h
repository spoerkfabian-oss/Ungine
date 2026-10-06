#pragma once
#include "Engine/Assets/MeshOptimizer.h"
#include "Engine/Assets/Model.h"
#include "Engine/Assets/TextureCooker.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Engine {

class Project;

// --- Cooked models -----------------------------------------------------------------------------------
// ModelData after OptimizeMeshes (vertex order, LODs) in a binary form. External texture files are
// stored relative to the model's directory; `modelFile` resolves them again when reading.

[[nodiscard]] std::vector<std::byte> SerializeModelData(const ModelData& data, const std::filesystem::path& modelFile,
                                                        const MeshOptimizeSettings& settings);
// Throws std::runtime_error on bad or truncated data. `settingsMatch`: cooked with the same settings.
[[nodiscard]] ModelData DeserializeModelData(std::span<const std::byte> bytes, const std::filesystem::path& modelFile,
                                             const MeshOptimizeSettings& settings, bool* settingsMatch = nullptr);
// The cooked model of `file` from a mounted pak (":cooked/models/<relative path>"); nullopt when
// there is none. Throws when it is corrupt.
[[nodiscard]] std::optional<ModelData> LoadCookedModel(const std::filesystem::path& file,
                                                       const MeshOptimizeSettings& settings);

// --- Content cooking ---------------------------------------------------------------------------------

struct CookOptions {
    // Must match the player's AssetManager settings for the cooked data to be used.
    TextureCookSettings  textures{.compress = true, .cacheDirectory = {}, .quality = 0};
    MeshOptimizeSettings meshes{};
    std::uint32_t        threads = 0; // texture encoding; 0: hardware threads
    std::function<void(float progress, const std::string& step)> progress; // optional, any thread
};

struct CookReport {
    struct Entry {
        std::string   path; // pak entry
        std::uint64_t bytes = 0;
        std::string   kind; // "raw", "model", "texture"
    };
    std::vector<Entry>       entries;
    std::vector<std::string> warnings; // missing references, unreferenced images cooked as color, ...
    std::vector<std::string> errors;   // files that could not be cooked: the build is not usable
    std::uint32_t            rawFiles = 0, replacedFiles = 0, cookedModels = 0, cookedTextures = 0;
    std::uint64_t            rawBytes = 0, cookedBytes = 0, pakBytes = 0;
    double                   seconds = 0.0;

    [[nodiscard]] bool        Ok() const { return errors.empty(); }
    [[nodiscard]] std::string Text() const; // human-readable build report
};

// Everything below the project's Content/ into one pak (entry paths relative to the project root):
// scenes, prefabs, blueprints, types and sounds as they are; glTF / GLB models cooked (their
// .gltf / .glb / external .bin files are left out); every texture they use and every other image
// (as color) also as a cooked KTX2. References in scenes and prefabs to missing files are reported.
// The pak is reproducible: same content and settings give the same bytes.
CookReport CookProjectContent(const Project& project, const std::filesystem::path& pakFile, const CookOptions& options = {});

} // namespace Engine
