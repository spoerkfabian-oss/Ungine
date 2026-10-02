#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

class AssetManager;
class Scene;
struct Transform;

// Prefabs ("Blueprint classes"): a reusable entity subtree stored in a .uprefab file (the scene
// file format, paths relative to the prefab; the root's transform is not stored). An instance is
// a root entity with PrefabInstance plus members with PrefabLink. Scene files store an instance
// as its root (UUID, parent, name, transform), the members' UUIDs and per-member overrides (JSON
// merge patches of the components against the prefab; removed members as null), so changes of
// the prefab reach every instance except where a value was overridden.
//
// - Instance roots keep their own name and transform; the hierarchy inside an instance comes
//   from the prefab (reparenting members is not an override).
// - Entities added below an instance are ordinary entities: they survive updates of the instance
//   and become part of the prefab with ApplyPrefabInstance.
// - Nested prefab instances are flattened (unlinked) when a prefab is written.
// - AssetManager pointers may be null (no models: mesh renderers are skipped with a warning).
// - Models are acquired once per call and appended to `models`: the caller owns those references.
// Throw std::runtime_error on unreadable / unwritable prefab files.

inline constexpr std::string_view kPrefabExtension = ".uprefab";

// Writes the subtree of `root` as a prefab and turns the subtree into an instance of it.
void CreatePrefab(const std::filesystem::path& file, Scene& scene, const AssetManager* assets, Entity root);

// A new instance below `parent` (NullEntity: a root) with the given local transform.
Entity InstantiatePrefab(Scene& scene, AssetManager* assets, const std::filesystem::path& file, Entity parent,
                         const Transform& transform, std::vector<ModelHandle>& models);

// The instance root `entity` belongs to (itself for a root), NullEntity if it is not part of an instance.
[[nodiscard]] Entity PrefabInstanceRoot(const Scene& scene, Entity entity);

struct PrefabOverride {
    Entity                   entity = NullEntity; // NullEntity for removed members
    std::uint64_t            source = 0;          // the member's UUID in the prefab file
    std::vector<std::string> keys;                // overridden top-level keys ("name", "transform", "light", ...)
    bool                     removed = false;     // the member was deleted from the instance
};
// What the instance changes compared to its prefab (empty if the prefab cannot be read).
[[nodiscard]] std::vector<PrefabOverride> PrefabOverrides(const Scene& scene, const AssetManager* assets, Entity root);
// Overridden top-level keys of one member (empty for non-members).
[[nodiscard]] std::vector<std::string> PrefabOverriddenKeys(const Scene& scene, const AssetManager* assets,
                                                            Entity entity);

// Writes the instance into its prefab (overrides, added and removed entities) and updates the
// scene's other instances of that prefab (their own overrides stay).
void ApplyPrefabInstance(Scene& scene, AssetManager* assets, Entity root, std::vector<ModelHandle>& models);
// Drops overrides: all of the instance (key empty and entity = root), all of one member (key
// empty) or one key of one member. Reverting the whole instance restores removed members; added
// entities stay.
void RevertPrefabOverrides(Scene& scene, AssetManager* assets, Entity entity, std::string_view key,
                           std::vector<ModelHandle>& models);
// Makes the instance ordinary entities.
void UnlinkPrefabInstance(Scene& scene, Entity root);
// Updates instances whose prefab file changed on disk since they were built (overrides stay);
// returns how many were updated.
std::size_t RefreshPrefabInstances(Scene& scene, AssetManager* assets, std::vector<ModelHandle>& models);

} // namespace Engine
