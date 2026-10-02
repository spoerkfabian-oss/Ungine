#pragma once
#include "Engine/Core/InputMap.h"
#include "Engine/Script/ScriptGraph.h"

#include <functional>
#include <optional>
#include <utility>
#include <span>
#include <string>
#include <vector>

namespace Engine {

class AssetManager;
class AudioSystem;
class Input;
class PhysicsWorld;
class Scene;

// Pure: no exec pins, evaluated whenever one of its outputs is read. Impure: runs when its exec
// input fires and continues along one exec output. Event: entry point fired by ScriptSystem.
enum class NodeKind : std::uint8_t { Event, Impure, Pure };

// What the node's param means (the editor shows a matching widget).
//   Function     name of a function of the graph (Entry / Return / Call nodes)
//   ElementType  element type of the array pins ("bool".."entity", "enum:X", "struct:Y"; set on connect)
//   TypeAndCount "<element type>:<n>" (Make Array, Select)
//   PinType      a value type ("float", "map:string:int", ...; reroutes, map nodes)
//   StructType   struct name (Make / Break Struct)        EnumType   enum name (Switch on Enum)
//   EnumValue    "<enum>.<value>" (enum literal)          StructField "<struct>.<field>" (Set Field)
//   Cases        comma separated case values (Switch on Int / String)
//   Macro        a macro of the graph or "<Library>.<Macro>"   LibraryFunction "<Library>.<Function>"
//   Interface    interface name             InterfaceFunction "<Interface>.<Function>"
//   Dispatcher   an event dispatcher of the graph       Timeline   a timeline of the graph
//   InputAction / InputAxis  a project input action / axis (InputMap)
enum class ParamKind : std::uint8_t {
    None, Text, Variable, Key, Count, Choice, Function, ElementType, TypeAndCount, PinType,
    StructType, EnumType, EnumValue, StructField, Cases, Macro, LibraryFunction, Interface, InterfaceFunction,
    Dispatcher, Timeline, InputAction, InputAxis
};

// How the param adapts when a link is made to the node (ScriptGraph::Connect):
//   PinType      the param becomes the connected pin's type (reroutes)
//   ElementType  array pins: the param becomes the connected array's element type
//   MapType      map pins: the param becomes the connected map type
//   UserType     struct / enum pins: the param becomes the connected struct / enum name
enum class ParamInference : std::uint8_t { None, PinType, ElementType, MapType, UserType };

// NodeDesc::execute results besides an exec output pin index.
inline constexpr int kScriptStop = -1; // the chain ends here (a pushed continuation resumes)
// NodeDesc::execute entry for a continuation pushed by the node (PushContinuation / Suspend).
inline constexpr int kScriptResume = -2;
// NodeDesc::execute result of a Return node: back to the caller of the function.
inline constexpr int kScriptReturn = -3;
// NodeDesc::execute entry once per frame while the node ticks (ScriptContext::SetTicking).
inline constexpr int kScriptTick = -4;

// Where the game view is in the window (pixels) - mouse and camera nodes work relative to it.
struct ScriptViewport {
    glm::vec2 origin{0.0f};
    glm::vec2 size{1280.0f, 720.0f};
};

// Runtime services for node implementations (implemented by ScriptSystem). Pins are indices into
// the node's resolved pin list (NodePins order).
class ScriptContext {
public:
    virtual ~ScriptContext() = default;

    [[nodiscard]] virtual ScriptValue In(int pin) = 0; // connected: the source's value, else the default
    virtual void Out(int pin, ScriptValue value) = 0;
    [[nodiscard]] virtual bool Connected(int pin) const = 0;
    [[nodiscard]] virtual const std::string& Param() const = 0;
    [[nodiscard]] virtual std::span<const PinInfo> Pins() const = 0; // the node's resolved pins

    [[nodiscard]] bool          InBool(int pin) { return std::get<bool>(Convert(In(pin), PinType::Bool)); }
    [[nodiscard]] std::int32_t  InInt(int pin) { return std::get<std::int32_t>(Convert(In(pin), PinType::Int)); }
    [[nodiscard]] float         InFloat(int pin) { return std::get<float>(Convert(In(pin), PinType::Float)); }
    [[nodiscard]] glm::vec3     InVec3(int pin) { return std::get<glm::vec3>(Convert(In(pin), PinType::Vec3)); }
    [[nodiscard]] std::string   InString(int pin) { return std::get<std::string>(Convert(In(pin), PinType::String)); }
    // Unconnected entity inputs mean the script's own entity (like "self" targets in UE).
    [[nodiscard]] Entity InEntity(int pin)
    {
        return Connected(pin) ? std::get<Entity>(Convert(In(pin), PinType::Entity)) : Self();
    }

    // Per script instance and node (DoOnce, FlipFlop, Gate, Delay, ForEach).
    struct NodeState {
        std::int32_t counter = 0;
        bool         flag    = false;
        bool         flag2   = false;
        ScriptValue  value   = false; // e.g. the array a ForEach iterates
        double       time    = 0.0;   // timelines, tweens
        std::vector<ScriptValue> values;
    };
    [[nodiscard]] virtual NodeState& State() = 0;

    [[nodiscard]] virtual Entity        Self() const = 0;
    [[nodiscard]] virtual Scene&        GetScene() = 0;
    [[nodiscard]] virtual double        Time() const = 0; // seconds since the script system began
    [[nodiscard]] virtual PhysicsWorld* Physics() = 0;    // may be null
    [[nodiscard]] virtual AssetManager* Assets() = 0;     // may be null
    [[nodiscard]] virtual AudioSystem*  Audio() = 0;      // may be null
    [[nodiscard]] virtual const Input*  GetInput() = 0;   // null while the game does not have the input
    // A variable of this instance: the running function's locals first, then the graph's. Null:
    // no such variable.
    [[nodiscard]] virtual ScriptValue*  Variable(const std::string& name) = 0;
    // Variables / custom events of another entity's script (null / false without one).
    [[nodiscard]] virtual ScriptValue*  InstanceVariable(Entity entity, const std::string& name) = 0;
    virtual bool                        CallEventOn(Entity entity, const std::string& name) = 0;
    [[nodiscard]] virtual ScriptViewport Viewport() const = 0;

    virtual void Print(std::string text, float seconds) = 0;
    virtual void Error(std::string message) = 0; // logged once per node, shown in the editor
    // Flow control: execute(kScriptResume) runs with ResumeData() == data once the chain started
    // by the current return value has ended (loops, Sequence) ...
    virtual void PushContinuation(std::int32_t data) = 0;
    // ... or after `seconds` (latent nodes; the node returns kScriptStop now).
    virtual void Suspend(float seconds, std::int32_t data) = 0;
    [[nodiscard]] virtual std::int32_t ResumeData() const = 0;
    // Runs the Custom Event nodes named `name` now; `args` fill their parameter outputs in order
    // (converted where possible).
    virtual void CallEvent(const std::string& name, std::vector<ScriptValue> args) = 0;
    // Interfaces: runs the function `function` of the target's script now (to its end, like a pure
    // function). False: no script or no such function (`results` then stays empty).
    virtual bool CallFunctionOn(Entity target, const std::string& function, std::vector<ScriptValue> args,
                                std::vector<ScriptValue>& results) = 0;
    [[nodiscard]] virtual bool Implements(Entity target, const std::string& interfaceName) = 0;
    // Event dispatchers. CallDispatcher runs every custom event bound to this script's dispatcher;
    // Bind adds (removes) this script's custom event `event` to (from) the target script's
    // dispatcher; UnbindAll clears it. False: the target has no script with that dispatcher.
    virtual void CallDispatcher(const std::string& name, std::vector<ScriptValue> args) = 0;
    virtual bool BindDispatcher(Entity target, const std::string& dispatcher, const std::string& event, bool bind) = 0;
    virtual bool UnbindAll(Entity target, const std::string& dispatcher) = 0;
    virtual void KeepModel(std::uint32_t index, std::uint32_t generation) = 0; // spawned: released when play ends
    // Timers fire a Custom Event of this instance; handles are > 0.
    virtual std::int32_t SetTimer(const std::string& event, float seconds, bool loop) = 0;
    virtual void         ClearTimer(std::int32_t handle) = 0;
    [[nodiscard]] virtual float TimerRemaining(std::int32_t handle) const = 0; // < 0: not active

    // Per-frame work (timelines, tweens): while on, execute(kScriptTick) runs once per update,
    // DeltaTime() long after the last one.
    virtual void                SetTicking(bool on) = 0;
    [[nodiscard]] virtual float DeltaTime() const   = 0;
    [[nodiscard]] virtual const ScriptGraph& Graph() const = 0; // the running graph (timelines, variables)
    [[nodiscard]] virtual const InputMap&    Inputs() const = 0; // project input actions / axes

    // Save games: slots are JSON files in ScriptSystem's save directory, cached in memory (Set /
    // Get work on the cache, Write / Read sync it with the file).
    virtual void SaveSet(const std::string& slot, const std::string& key, const ScriptValue& value, PinType type) = 0;
    [[nodiscard]] virtual std::optional<ScriptValue> SaveGet(const std::string& slot, const std::string& key, PinType type) = 0;
    virtual bool               SaveWrite(const std::string& slot)  = 0;
    virtual bool               SaveRead(const std::string& slot)   = 0;
    [[nodiscard]] virtual bool SaveExists(const std::string& slot) = 0;
    virtual bool               SaveDelete(const std::string& slot) = 0;

    // Levels: the application handles the request after the update (ScriptSystem::TakeLevelRequest).
    virtual void RequestLevel(std::string scene, bool quit) = 0;
    [[nodiscard]] virtual const std::string& CurrentLevel() const = 0;
};

struct NodeDesc {
    std::string          type;     // stable id stored in graphs
    std::string          title;
    std::string          category; // "Flow", "Math|Float", ...
    std::string          tooltip;
    NodeKind             kind = NodeKind::Pure;
    std::vector<PinInfo> pins; // static pins (see resolvePins)
    // Values of unconnected inputs the node does not set itself (else DefaultValue of the type).
    std::vector<std::pair<std::string, ScriptValue>> defaults;

    ParamKind                paramKind = ParamKind::None;
    std::string              paramLabel;
    std::string              paramDefault;
    std::vector<std::string> paramChoices; // ParamKind::Choice
    ParamInference           inference     = ParamInference::None;
    bool                     latent        = false; // suspends (not allowed in functions)
    bool                     arrayVariable = false; // ParamKind::Variable: needs an array variable
    bool                     mapVariable   = false; // ParamKind::Variable: needs a map variable
    bool                     hidden        = false; // not offered in the node palette (function nodes)
    // Inference only from links on these pins (null: any pin).
    std::function<bool(std::string_view pin)> infers;
    // Unconnected input values beyond `defaults` (e.g. struct field defaults); nullopt: the type's.
    std::function<std::optional<ScriptValue>(const ScriptNode&, const PinInfo&)> pinDefault;

    // Pins that depend on the param or the graph (variables, output counts); null: `pins`.
    std::function<std::vector<PinInfo>(const ScriptGraph&, const ScriptNode&)> resolvePins;
    std::function<void(ScriptContext&)>          evaluate; // pure: reads inputs, writes outputs
    std::function<int(ScriptContext&, int entry)> execute; // impure: entry = exec input pin index / kScriptResume
};

[[nodiscard]] std::span<const NodeDesc> ScriptNodeTypes();
[[nodiscard]] const NodeDesc*           FindScriptNodeType(std::string_view type);
// Element type names for ParamKind::ElementType / TypeAndCount: built-ins ("bool", "int", ...) and
// the registered enums / structs ("enum:Color", "struct:Item").
[[nodiscard]] std::vector<std::string> ElementTypeNames();
// "<type>:<count>" (TypeAndCount params): the type and the count (`fallback` without one).
[[nodiscard]] std::pair<std::string, int> SplitTypeAndCount(std::string_view param, int fallback);
// Value of an unconnected input: the node's own default, else the type's, else DefaultValue.
[[nodiscard]] ScriptValue PinDefault(const ScriptNode& node, const NodeDesc* desc, const PinInfo& pin);

// Easing curves for tweens / Ease ("Linear", "Sine In", ..., "Bounce Out"); unknown names: linear.
[[nodiscard]] float                        ScriptEase(std::string_view name, float t);
[[nodiscard]] std::span<const std::string> ScriptEaseNames();

// A keyboard key (KeyNames) or "MouseLeft" / "MouseRight" / "MouseMiddle": held, pressed or
// released this frame. Unknown names: false.
enum class KeyQuery : std::uint8_t { Down, Pressed, Released };
[[nodiscard]] bool QueryKey(const Input& input, std::string_view key, KeyQuery query);

// Key names for key events / Is Key Down ("A".."Z", "0".."9", "Space", "Enter", "Escape", "Tab",
// "Left", "Right", "Up", "Down", "LeftShift", "LeftControl", "F1".."F12"). -1: unknown.
[[nodiscard]] int                           KeyFromName(std::string_view name);
[[nodiscard]] std::span<const std::string> KeyNames();

} // namespace Engine
