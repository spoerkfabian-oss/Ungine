#pragma once
#include "Engine/Script/ScriptGraph.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

class AssetManager;
class EventBus;
class Input;
class PhysicsWorld;
class Scene;

struct ScriptMessage {
    std::string text;
    double      time     = 0.0; // ScriptSystem::Time() when printed
    float       duration = 2.0f;
    bool        error    = false;
};

// Per graph file, for the editor: when nodes ran and exec links fired (ScriptSystem::Time()), and
// the diagnostics (validation + runtime errors).
struct ScriptDebugInfo {
    std::unordered_map<std::uint32_t, double>              nodeTimes;
    std::map<std::pair<std::uint32_t, std::string>, double> linkTimes; // (from node, exec output pin)
    std::vector<ScriptDiagnostic>                          diagnostics;
};

struct ScriptStats {
    std::uint32_t instances      = 0;
    std::uint32_t waiting        = 0; // latent threads (Delay)
    std::uint32_t nodesExecuted  = 0; // last Update (impure nodes)
    std::uint32_t eventsFired    = 0; // last Update
    std::uint32_t errors         = 0; // since Begin
};

// Runs the visual scripts of a scene (entities with ScriptComponent) while playing. Main thread.
// Begin: loads and validates the graphs (graphs with errors do not run), BeginPlay. Update (per
// frame): scripts added / removed since, finished Delays, collision events (from the EventBus,
// published by PhysicsWorld), key events, Tick. End: EndPlay, everything dropped.
//
// Execution: an event starts a chain along the exec links; impure nodes run when reached, pure
// nodes are evaluated whenever an input reads them. Latent nodes (Delay) park the chain and let
// the rest continue (like UE). Loops and Sequence resume after their chain ends. A chain longer
// than maxStepsPerEvent is aborted (infinite loop).
class ScriptSystem {
public:
    // input / physics / assets are optional (their nodes report errors or do nothing without).
    ScriptSystem(EventBus& events, const Input* input = nullptr, PhysicsWorld* physics = nullptr,
                 AssetManager* assets = nullptr);
    ~ScriptSystem();

    ScriptSystem(const ScriptSystem&)            = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    void Begin(Scene& scene);
    // acceptInput: key events / Is Key Down see the keyboard (false while the editor UI has it).
    void Update(Scene& scene, float dt, bool acceptInput = true);
    void End(Scene& scene);
    [[nodiscard]] bool Running() const;

    // Graph used instead of the file (editor: unsaved edits run in play mode); nullopt removes it.
    void Provide(const std::filesystem::path& file, std::optional<ScriptGraph> graph);
    [[nodiscard]] const ScriptDebugInfo* Debug(const std::filesystem::path& file) const;

    [[nodiscard]] std::span<const ScriptMessage> Messages() const; // recent prints, until their duration ends
    [[nodiscard]] const ScriptStats&             Stats() const;
    [[nodiscard]] double                         Time() const; // seconds since Begin

    std::uint32_t maxStepsPerEvent = 100000;

    // Canonical key of a graph file (absolute, normalized).
    [[nodiscard]] static std::string Key(const std::filesystem::path& file);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Engine
