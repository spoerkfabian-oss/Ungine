#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

class AssetManager;
class FlyCamera;
class Scene;
struct PhysicsSettings;
class SceneRenderer;

// Scene files (JSON, "version": 1): entities in parent-before-child order with their UUIDs,
// Name / Transform / MeshRenderer / Light, optionally the renderer settings and the camera.
// Models are referenced by file (relative to the scene file when possible) or by primitive
// recipe; models made with AssetManager::CreateModel cannot be saved (skipped with a warning).
struct SceneFileOptions {
    SceneRenderer*   renderer = nullptr; // settings saved / restored when set
    FlyCamera*       camera   = nullptr;
    PhysicsSettings* physics  = nullptr; // gravity, steps, interpolation, layer matrix
};

// Prefab instances are stored as their root + member UUIDs + overrides (see Prefab.h).
// Throws std::runtime_error on I/O errors.
void SaveSceneFile(const std::filesystem::path& file, const Scene& scene, const AssetManager& assets,
                   const SceneFileOptions& options = {});
// Without an asset manager (tools, tests): mesh renderers are skipped.
void SaveSceneFile(const std::filesystem::path& file, const Scene& scene, const AssetManager* assets,
                   const SceneFileOptions& options = {});

// Adds the file's entities to `scene` (Clear() it first to replace). Each distinct model is
// acquired once (LoadModel / CreatePrimitive, loads asynchronously): the caller owns the returned
// handles and releases them when the scene is done. Throws std::runtime_error on I/O or format
// errors (the scene is left unchanged then).
[[nodiscard]] std::vector<ModelHandle> LoadSceneFile(const std::filesystem::path& file, Scene& scene,
                                                     AssetManager& assets, const SceneFileOptions& options = {});
// Without an asset manager (tools, tests): no models are loaded.
[[nodiscard]] std::vector<ModelHandle> LoadSceneFile(const std::filesystem::path& file, Scene& scene,
                                                     AssetManager* assets, const SceneFileOptions& options = {});

// --- Asynchronous loading (level streaming, Open Level with a loading screen) ---
// PrepareSceneFile reads and parses a scene file on any thread (no Scene, no AssetManager access);
// it also collects the models the scene and its prefabs use. On the main thread
// AcquireSceneModels starts loading them (handles go into `models`), and once they are ready
// InstantiatePreparedScene creates the entities: no pop-in. LoadSceneFile = all three at once.
struct PreparedScene;
// Model key (normalized file or primitive recipe) -> handle; the caller releases the handles.
using SceneModels = std::unordered_map<std::string, ModelHandle>;

// Throws std::runtime_error on I/O or format errors.
[[nodiscard]] std::shared_ptr<const PreparedScene> PrepareSceneFile(const std::filesystem::path& file);
[[nodiscard]] const std::filesystem::path&       PreparedSceneFile(const PreparedScene& scene);
// Main thread. Models already in `models` are not acquired again.
void AcquireSceneModels(const PreparedScene& prepared, AssetManager& assets, SceneModels& models);
// Main thread: adds the entities (models not yet in `models` are acquired into it). Returns the
// created root entities (the file's top-level entities). Throws std::runtime_error on format
// errors; the entities created so far are destroyed again (`models` keeps its handles).
std::vector<Entity> InstantiatePreparedScene(const PreparedScene& prepared, Scene& scene, AssetManager* assets,
                                             SceneModels& models, const SceneFileOptions& options = {});

// --- In-memory snapshots (editor undo / duplicate) ---
// Models are stored as raw handles: no reference counting, stale handles render nothing.

// Whole subtrees of `roots` (none may be a descendant of another). Each root records its parent
// and sibling index.
[[nodiscard]] std::string SnapshotEntities(const Scene& scene, std::span<const Entity> roots);

enum class RestoreMode {
    Original,  // same UUIDs, back at the recorded parent + sibling index (undo of a delete)
    Duplicate, // fresh UUIDs, appended to the recorded parent; references inside the copy (script
               // entity variables) point into the copy; prefab members of a duplicated instance
               // follow its new root, members duplicated without their root become plain
};
// Returns the restored roots. A recorded parent that no longer exists makes the root a root.
std::vector<Entity> RestoreEntities(Scene& scene, const std::string& snapshot, RestoreMode mode);

// Components of one entity (name, transform, all serialized components), not its place in the hierarchy.
[[nodiscard]] std::string SnapshotEntityState(const Scene& scene, Entity entity);
void                      ApplyEntityState(Scene& scene, Entity entity, const std::string& state);

// Multi-editing: applies what changed between two states of one entity (before -> after) to
// `target`: changed values (vectors per element), added and removed components. Values the edit
// did not touch keep the target's; name and identity stay. Returns true if the target changed.
bool ApplyEntityStateDiff(Scene& scene, Entity target, const std::string& before, const std::string& after);

} // namespace Engine
