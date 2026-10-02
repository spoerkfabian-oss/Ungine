#pragma once
// Engine-private: how scene files store prefab instances (SceneSerializer <-> Prefab).
#include "SceneJson.h"

#include <filesystem>
#include <utility>
#include <vector>

namespace Engine {

class AssetManager;
class Registry;
class Scene;

namespace PrefabDetail {

// A member of an instance (not its root): regenerated from the prefab when a scene file loads.
[[nodiscard]] bool IsMember(const Registry& r, Entity e);

// {"file", "members": [[source, uuid], ...], "overrides": {source: patch | null}} with paths
// relative to sceneDir.
[[nodiscard]] SceneJson::json InstanceToFileJson(const Scene& scene, const AssetManager* assets, Entity root,
                                                 const std::filesystem::path& sceneDir);
// Builds the instance on `root` (created with its UUID, name and transform) from that data. Models
// are acquired into refs.acquired. A missing prefab leaves the root alone (kept for saving, warned).
// Returns the members by their recorded UUIDs (children saved below them refer to those).
std::vector<std::pair<std::uint64_t, Entity>> InstanceFromFileJson(Scene& scene, AssetManager* assets, Entity root,
                                                                   const SceneJson::json& data,
                                                                   const std::filesystem::path& sceneDir,
                                                                   SceneJson::ModelRefs& refs);

} // namespace PrefabDetail
} // namespace Engine
