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
// - Prefabs nest: a prefab file stores prefab instances like a scene file does (root + members
//   + overrides), and loading expands them, so a change of the nested prefab reaches every
//   prefab and instance containing it except where a level overrides the value. In the scene a
//   nested instance is part of the outermost one (one PrefabInstance, links to its root);
//   PrefabLevels tells the prefabs an entity comes from, ApplyPrefabLevel writes into one of them.
// - A variant is a prefab whose root is an instance of another prefab (its base) with overrides
//   and added entities; creating a prefab from an instance root makes a variant of its prefab.
// - AssetManager pointers may be null (no models: mesh renderers are skipped with a warning).
// - Models are acquired once per call and appended to `models`: the caller owns those references.
// Throw std::runtime_error on unreadable / unwritable prefab files.

inline constexpr std::string_view kPrefabExtension = ".uprefab";

// Writes the subtree of `root` as a prefab and turns the subtree into an instance of it. Prefab
// instances in the subtree stay nested; if `root` is an instance root the new prefab is a variant
// of its prefab. Throws if the prefab would contain an instance of itself.
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
// scene's other instances of that prefab and of prefabs nesting it (their own overrides stay).
void ApplyPrefabInstance(Scene& scene, AssetManager* assets, Entity root, std::vector<ModelHandle>& models);

// The prefabs an instance entity comes from: [0] the instance in the scene, then the nested
// prefab instance it belongs to inside that prefab, and so on (empty for non-instance entities).
struct PrefabLevel {
    std::string   file;              // the prefab (absolute, normalized)
    Entity        root = NullEntity; // the scene entity that is this level's instance root (NullEntity: removed)
    std::uint64_t source = 0;        // the entity's UUID in that prefab
};
[[nodiscard]] std::vector<PrefabLevel> PrefabLevels(const Scene& scene, Entity entity);
// Writes the values of the level-`level` instance `entity` belongs to into that level's prefab
// and updates every instance depending on it (the scene keeps what it shows). Level 0 =
// ApplyPrefabInstance; inner levels take the component values of the entities that instance has
// (entities added or removed in the scene are applied only at level 0).
void ApplyPrefabLevel(Scene& scene, AssetManager* assets, Entity entity, std::size_t level,
                      std::vector<ModelHandle>& models);

// Writes a new prefab `file` that is a variant of `base` (no overrides yet).
void CreatePrefabVariant(const std::filesystem::path& file, const std::filesystem::path& base);
// The base prefab of a variant ("" if `file` is none or unreadable).
[[nodiscard]] std::string PrefabBase(const std::filesystem::path& file);
// Every prefab `file` nests (transitively, variants' bases included).
[[nodiscard]] std::vector<std::string> PrefabNestedFiles(const std::filesystem::path& file);
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
