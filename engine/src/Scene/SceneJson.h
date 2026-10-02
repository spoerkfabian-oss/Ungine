#pragma once
// Engine-private: entities <-> JSON, shared by scene files, snapshots and prefabs.
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine {

class AssetManager;
class Registry;
class Scene;

namespace SceneJson {

using json = nlohmann::json;

[[nodiscard]] std::string           ToUtf8(const std::filesystem::path& path); // generic separators
[[nodiscard]] std::filesystem::path FromUtf8(const std::string& s);
// Absolute, normalized (symlinks resolved where the path exists): the form prefab comparisons use.
[[nodiscard]] std::string NormalizedFile(const std::filesystem::path& path);

// How model references and file paths (scripts, sounds) are written and read.
enum class PathMode {
    Memory,   // models as raw handles, paths unchanged (undo snapshots; no reference counting)
    File,     // models by file / primitive recipe, paths relative to baseDir (scene files)
    Absolute, // like File, but every path absolute and normalized (prefab comparisons)
};

struct ModelRefs {
    PathMode              mode        = PathMode::Memory;
    AssetManager*         assets      = nullptr; // reading in File / Absolute mode (null: models skipped)
    const AssetManager*   constAssets = nullptr; // writing in File / Absolute mode (null: models skipped)
    std::filesystem::path baseDir;               // File mode: the scene / prefab directory
    // File / primitive key -> handle acquired while reading (shared between nested readers, e.g.
    // the prefabs of a scene file: each model is acquired once).
    std::unordered_map<std::string, ModelHandle>  ownAcquired;
    std::unordered_map<std::string, ModelHandle>* acquired = &ownAcquired;
    std::unordered_set<std::uint32_t>             warned; // unsaveable models (writing)

    ModelRefs() = default;
    ModelRefs(const ModelRefs&)            = delete;
    ModelRefs& operator=(const ModelRefs&) = delete;

    [[nodiscard]] bool Memory() const { return mode == PathMode::Memory; }

    [[nodiscard]] json        Write(ModelHandle handle);
    [[nodiscard]] ModelHandle Read(const json& j);
    [[nodiscard]] std::string WritePath(const std::string& path) const;
    [[nodiscard]] std::string ReadPath(const std::string& path) const;
};

// UUID, name, transform and every serialized component. Prefab links ("prefabInstance",
// "prefabLink") only in Memory mode: files store instances differently.
[[nodiscard]] json EntityToJson(const Registry& r, Entity e, ModelRefs& models);
// Overwrites the components with the JSON's; components missing there are removed (prefab
// components only in Memory mode). Name / transform only if present.
void ApplyComponents(Scene& scene, Entity e, const json& j, ModelRefs& models);
// Pre-order (parents first) over the subtree of `root`, children in their order.
void CollectSubtree(const Registry& r, Entity root, std::vector<Entity>& out);
[[nodiscard]] std::uint64_t UuidOf(const Registry& r, Entity e); // 0 for NullEntity

// A model reference ({"file"} / {"primitive"} / {"handle"}) in the form EntityToJson writes in
// Absolute mode (file paths must be absolute or relative to the working directory).
[[nodiscard]] json CanonicalModelRef(const json& model);

// Rewrites the file paths of an entity JSON (or a merge patch of one) in place: mesh / model
// instance files, script graph, audio sound.
template <class F>
void TransformPaths(json& entity, F&& fn)
{
    const auto apply = [&](json* value) {
        if (value && value->is_string() && !value->get_ref<const std::string&>().empty())
            *value = fn(value->get<std::string>());
    };
    const auto at = [](json& j, std::initializer_list<const char*> keys) -> json* {
        json* p = &j;
        for (const char* k : keys) {
            if (!p->is_object())
                return nullptr;
            const auto it = p->find(k);
            if (it == p->end())
                return nullptr;
            p = &*it;
        }
        return p;
    };
    apply(at(entity, {"mesh", "model", "file"}));
    apply(at(entity, {"modelInstance", "model", "file"}));
    apply(at(entity, {"script", "graph"}));
    apply(at(entity, {"audioSource", "sound"}));
}

// Rewrites entity references by UUID inside components (script entity variables) in place.
template <class F>
void RemapEntityRefs(json& entity, F&& fn)
{
    const auto script = entity.find("script");
    if (script == entity.end() || !script->is_object())
        return;
    const auto vars = script->find("variables");
    if (vars == script->end() || !vars->is_object())
        return;
    for (auto& v : *vars)
        if (v.is_object())
            if (const auto uuid = v.find("uuid"); uuid != v.end() && uuid->is_number_unsigned())
                *uuid = fn(uuid->get<std::uint64_t>());
}

} // namespace SceneJson
} // namespace Engine
