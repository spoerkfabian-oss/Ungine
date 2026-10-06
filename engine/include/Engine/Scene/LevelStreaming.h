#pragma once
#include "Engine/Core/MoveOnlyFunction.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Scene/SceneSerializer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Engine {

class AssetManager;
class EventBus;
class Scene;
class ThreadPool;

// Unloaded -> Preparing (file read + parsed on the thread pool) -> Loading (its models load in
// the background) -> Ready (prepared, not in a scene) -> Loaded (entities created). Failed: the
// file could not be read or instantiated (Error()).
enum class LevelState : std::uint8_t { Unloaded, Preparing, Loading, Ready, Loaded, Failed };
[[nodiscard]] const char* LevelStateName(LevelState state);

// Published on the EventBus when a streamed level was added to or removed from the scene, or
// failed to load. `level` as requested: relative to the working directory (= the project root)
// when inside it, else absolute.
struct LevelStreamedEvent {
    std::string level;
    bool        loaded = false; // false: unloaded (or failed)
    bool        failed = false;
    std::string error;
};

// Full level switch in the background (Open Level with a loading screen): Begin reads and parses
// the scene on the thread pool, Update (main thread) acquires and waits for its models; once
// State() is Ready, Take() hands over the prepared scene and its model handles for
// InstantiatePreparedScene. The current level stays untouched until then.
class LevelLoader {
public:
    LevelLoader(ThreadPool& jobs, AssetManager* assets);
    ~LevelLoader(); // waits for a running parse; releases models not taken
    LevelLoader(const LevelLoader&)            = delete;
    LevelLoader& operator=(const LevelLoader&) = delete;

    bool Begin(const std::filesystem::path& file); // false: a level is already being prepared
    void Update();
    void Cancel(); // models released; a running parse finishes in the background

    struct Result {
        std::shared_ptr<const PreparedScene> scene;
        SceneModels                          models; // owned by the caller from now on
    };
    [[nodiscard]] std::optional<Result> Take(); // Ready: the prepared level (the loader is idle again)

    [[nodiscard]] LevelState                   State() const;
    [[nodiscard]] float                        Progress() const; // 0..1
    [[nodiscard]] const std::string&           Error() const;
    [[nodiscard]] const std::filesystem::path& File() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

struct StreamedLevelInfo {
    std::string level; // display path (see LevelStreamedEvent)
    std::string key;   // normalized absolute file
    LevelState  state    = LevelState::Unloaded;
    float       progress = 0.0f;
    bool        requested = false; // Load() without Unload()
    bool        byVolume  = false; // a streaming volume wants it
    std::size_t roots     = 0;     // top-level entities while loaded
    std::string error;
};

// Additive sub-levels. A level is wanted while it is requested (Load / Blueprint Load Stream
// Level) or a LevelStreamingVolume wants it (a streaming source inside, see the component); it is
// prepared in the background and its entities are created in one step once its models are
// ready (no pop-in). Its top-level entities get StreamedLevel and are destroyed when it is no
// longer wanted (the unload hook runs first: EndPlay of their scripts). A level is loaded at most
// once; entity references into it resolve by UUID (null while it is unloaded). Main thread only.
class LevelStreamer {
public:
    LevelStreamer(ThreadPool& jobs, AssetManager* assets, EventBus* events = nullptr);
    ~LevelStreamer(); // waits for running parses, releases models; the scene is not touched
    LevelStreamer(const LevelStreamer&)            = delete;
    LevelStreamer& operator=(const LevelStreamer&) = delete;

    // Runs with a level's top-level entities right before they are destroyed.
    void SetUnloadHook(MoveOnlyFunction<void(Scene&, std::span<const Entity>)> hook);

    void Load(const std::filesystem::path& level);   // request (idempotent; retries a failed level)
    void Unload(const std::filesystem::path& level); // drops the request (volumes may still want it)
    // Evaluates the volumes (sources: StreamingSource entities, else `fallbackSource`), advances
    // preparations, creates ready levels and destroys unwanted ones.
    void Update(Scene& scene, std::optional<glm::vec3> fallbackSource = std::nullopt);
    void UnloadAll(Scene& scene); // now (unload hook, models released); requests dropped
    void Reset();                 // the scene was cleared: forget every level, release its models

    [[nodiscard]] LevelState                     State(const std::filesystem::path& level) const;
    [[nodiscard]] bool                           IsLoaded(const std::filesystem::path& level) const;
    [[nodiscard]] bool                           Busy() const; // a level is being prepared
    [[nodiscard]] std::vector<StreamedLevelInfo> Levels() const;

    // Normalized absolute form of a level path (relative: to the working directory).
    [[nodiscard]] static std::string Key(const std::filesystem::path& level);
    // Relative to the working directory when inside it, else absolute (UTF-8).
    [[nodiscard]] static std::string DisplayPath(const std::filesystem::path& level);

    bool volumesEnabled = true; // false: only requests count (editor edit mode: previews)

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Engine
