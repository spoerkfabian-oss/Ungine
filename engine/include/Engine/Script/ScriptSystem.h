#pragma once
#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptNodes.h"

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
class AudioSystem;
struct UiEvent;
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

// Where the debugger stopped (a breakpoint or a step): the next node to run, and the function
// calls it is in (outermost first: the Call nodes).
struct ScriptDebugFrame {
    std::string   file;     // ScriptSystem::Key of the graph (of a library for its copied nodes)
    std::uint32_t node = 0; // node id
    std::string   function; // empty: event graph
    Entity        entity = NullEntity;
    std::vector<ScriptDebugFrame> callers;
};

// A breakpoint for SetBreakpoints (ScriptBreakpointOptions: condition, hit count).
struct ScriptBreakpoint {
    std::uint32_t           node = 0;
    ScriptBreakpointOptions options;
};

// Values of one script instance for the debugger.
struct ScriptWatch {
    Entity                                                       entity = NullEntity;
    std::vector<std::pair<std::string, ScriptValue>>             variables;
    std::string                                                  function; // of `locals` (the paused one)
    std::vector<std::pair<std::string, ScriptValue>>             locals;
    std::map<std::pair<std::uint32_t, std::string>, ScriptValue> pins; // last value of each data output
};

// Asked for by Open Level / Quit Game; the application acts on it after the update.
struct ScriptLevelRequest {
    std::string scene; // relative to the working directory (= the project root)
    bool        quit = false;
};

struct ScriptStats {
    std::uint32_t instances      = 0;
    std::uint32_t waiting        = 0; // latent threads (Delay)
    std::uint32_t nodesExecuted  = 0; // last Update (impure nodes)
    std::uint32_t eventsFired    = 0; // last Update
    std::uint32_t errors         = 0; // since Begin
    std::uint32_t timers         = 0; // active
    std::uint32_t queued         = 0; // chains waiting while the debugger pauses
    std::uint32_t ticking        = 0; // timelines / tweens running
};

// Runs the visual scripts of a scene (entities with ScriptComponent) while playing. Main thread.
// Begin: loads and validates the graphs (graphs with errors do not run), BeginPlay. Update (per
// frame): scripts added / removed since, finished Delays, collision events (from the EventBus,
// published by PhysicsWorld), key events, Tick. End: EndPlay, everything dropped.
//
// Execution: an event starts a chain along the exec links; impure nodes run when reached, pure
// nodes are evaluated whenever an input reads them. Latent nodes (Delay) park the chain and let
// the rest continue (like UE). Loops and Sequence resume after their chain ends. A chain longer
// than maxStepsPerEvent is aborted (infinite loop). Function calls run the function's chain (own
// loop stack, locals reset per call) and continue after its Return; pure functions run whenever an
// output is read. Timers fire Custom Events.
//
// Debugger: a chain reaching a breakpoint (or the next node after DebugStep) stops; the whole
// system then waits (Update does nothing, new chains queue up) until DebugContinue / DebugStep.
class ScriptSystem {
public:
    // input / physics / assets / audio are optional (their nodes report errors or do nothing without).
    ScriptSystem(EventBus& events, const Input* input = nullptr, PhysicsWorld* physics = nullptr,
                 AssetManager* assets = nullptr, AudioSystem* audio = nullptr);
    ~ScriptSystem();

    ScriptSystem(const ScriptSystem&)            = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    void Begin(Scene& scene);
    // acceptInput: key events / Is Key Down see the keyboard (false while the editor UI has it).
    void Update(Scene& scene, float dt, bool acceptInput = true);
    // Dispatches a named Custom Event on the entity owning an interactive UI widget.
    void DispatchUiEvent(Scene& scene, const UiEvent& event);
    void End(Scene& scene);
    [[nodiscard]] bool Running() const;

    // Graph used instead of the file (editor: unsaved edits run in play mode); nullopt removes it.
    void Provide(const std::filesystem::path& file, std::optional<ScriptGraph> graph);
    [[nodiscard]] const ScriptDebugInfo* Debug(const std::filesystem::path& file) const;

    [[nodiscard]] std::span<const ScriptMessage> Messages() const; // recent prints, until their duration ends
    [[nodiscard]] const ScriptStats&             Stats() const;
    [[nodiscard]] double                         Time() const; // seconds since Begin

    std::uint32_t maxStepsPerEvent = 100000;

    // Construction scripts (Event.Construction): run on a temporary instance, outside play too
    // (editor: after edits) and by Begin before BeginPlay. Entities they create get
    // ConstructionOwned and are destroyed when the owner's script runs again (or the owner is
    // gone); latent nodes, timers and timelines do not run there. RunConstruction: one entity
    // (false: no construction script); RunAllConstruction: every scripted entity, returns how many ran.
    // Each run starts from the owner as it was before the last run (the script's own changes to it
    // are undone, edits made since are kept), like UE.
    bool        RunConstruction(Scene& scene, Entity entity);
    std::size_t RunAllConstruction(Scene& scene);
    // Back to the state without construction scripts (owners restored, constructed entities
    // destroyed), e.g. to save the scene; run them again afterwards.
    void ResetConstructed(Scene& scene);

    // Game view rectangle in window pixels (mouse / camera nodes).
    void SetViewport(const ScriptViewport& viewport);
    // Project input actions / axes (Input Action events, Is Action Down, Get Axis).
    void SetInputMap(InputMap map);
    // Where save game slots are written (<slot>.sav); empty: slots live in memory only.
    void SetSaveDirectory(std::filesystem::path directory);
    // Open Level / Quit Game since the last call (the latest wins).
    [[nodiscard]] std::optional<ScriptLevelRequest> TakeLevelRequest();
    void SetCurrentLevel(std::string scene); // Get Current Level

    // --- Debugger. Breakpoints start as the graph's saved ones; SetBreakpoints replaces them for a
    // graph while running (also before Begin compiles it).
    void SetBreakpoints(const std::filesystem::path& file, std::vector<std::uint32_t> nodes);
    void SetBreakpointList(const std::filesystem::path& file, const std::vector<ScriptBreakpoint>& breakpoints); // with options
    [[nodiscard]] bool                            DebugPaused() const;
    [[nodiscard]] std::optional<ScriptDebugFrame> PausedAt() const;
    void DebugContinue(Scene& scene); // runs on until the next breakpoint
    void DebugStep(Scene& scene);     // step into: runs the paused node, stops at the next one that runs
    void DebugStepOver(Scene& scene); // stops at the next node of this chain outside the called functions
    void DebugStepOut(Scene& scene);  // stops after the current function returned (or in another chain)
    // Entities running a graph, and the values of one of them.
    [[nodiscard]] std::vector<Entity>        InstancesOf(const std::filesystem::path& file) const;
    [[nodiscard]] std::optional<ScriptWatch> Watch(const std::filesystem::path& file, Entity entity) const;

    // Canonical key of a graph file (absolute, normalized). Libraries registered without a file are
    // "library:<name>" (debug info, breakpoints of their nodes).
    [[nodiscard]] static std::string Key(const std::filesystem::path& file);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Engine
