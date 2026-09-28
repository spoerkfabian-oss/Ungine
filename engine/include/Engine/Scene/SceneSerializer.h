#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Engine {

class AssetManager;
class FlyCamera;
class Scene;
class SceneRenderer;

// Scene files (JSON, "version": 1): entities in parent-before-child order with their UUIDs,
// Name / Transform / MeshRenderer / Light, optionally the renderer settings and the camera.
// Models are referenced by file (relative to the scene file when possible) or by primitive
// recipe; models made with AssetManager::CreateModel cannot be saved (skipped with a warning).
struct SceneFileOptions {
    SceneRenderer* renderer = nullptr; // settings saved / restored when set
    FlyCamera*     camera   = nullptr;
};

// Throws std::runtime_error on I/O errors.
void SaveSceneFile(const std::filesystem::path& file, const Scene& scene, const AssetManager& assets,
                   const SceneFileOptions& options = {});

// Adds the file's entities to `scene` (Clear() it first to replace). Each distinct model is
// acquired once (LoadModel / CreatePrimitive, loads asynchronously): the caller owns the returned
// handles and releases them when the scene is done. Throws std::runtime_error on I/O or format
// errors (the scene is left unchanged then).
[[nodiscard]] std::vector<ModelHandle> LoadSceneFile(const std::filesystem::path& file, Scene& scene,
                                                     AssetManager& assets, const SceneFileOptions& options = {});

// --- In-memory snapshots (editor undo / duplicate) ---
// Models are stored as raw handles: no reference counting, stale handles render nothing.

// Whole subtrees of `roots` (none may be a descendant of another). Each root records its parent
// and sibling index.
[[nodiscard]] std::string SnapshotEntities(const Scene& scene, std::span<const Entity> roots);

enum class RestoreMode {
    Original,  // same UUIDs, back at the recorded parent + sibling index (undo of a delete)
    Duplicate, // fresh UUIDs, appended to the recorded parent
};
// Returns the restored roots. A recorded parent that no longer exists makes the root a root.
std::vector<Entity> RestoreEntities(Scene& scene, const std::string& snapshot, RestoreMode mode);

// Components of one entity (Name, Transform, MeshRenderer, Light), not its place in the hierarchy.
[[nodiscard]] std::string SnapshotEntityState(const Scene& scene, Entity entity);
void                      ApplyEntityState(Scene& scene, Entity entity, const std::string& state);

} // namespace Engine
