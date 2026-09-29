#pragma once
#include "Engine/Script/ScriptGraph.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Engine {

class AssetManager;
class Input;
class PhysicsWorld;
class Scene;

// Pure: no exec pins, evaluated whenever one of its outputs is read. Impure: runs when its exec
// input fires and continues along one exec output. Event: entry point fired by ScriptSystem.
enum class NodeKind : std::uint8_t { Event, Impure, Pure };

// What the node's param means (the editor shows a matching widget).
enum class ParamKind : std::uint8_t { None, Text, Variable, Key, Count, Choice };

// NodeDesc::execute results besides an exec output pin index.
inline constexpr int kScriptStop = -1; // the chain ends here (a pushed continuation resumes)
// NodeDesc::execute entry for a continuation pushed by the node (PushContinuation / Suspend).
inline constexpr int kScriptResume = -2;

// Runtime services for node implementations (implemented by ScriptSystem). Pins are indices into
// the node's resolved pin list (NodePins order).
class ScriptContext {
public:
    virtual ~ScriptContext() = default;

    [[nodiscard]] virtual ScriptValue In(int pin) = 0; // connected: the source's value, else the default
    virtual void Out(int pin, ScriptValue value) = 0;
    [[nodiscard]] virtual bool Connected(int pin) const = 0;
    [[nodiscard]] virtual const std::string& Param() const = 0;

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

    // Per script instance and node (DoOnce, FlipFlop, Gate, Delay).
    struct NodeState {
        std::int32_t counter = 0;
        bool         flag    = false;
        bool         flag2   = false;
    };
    [[nodiscard]] virtual NodeState& State() = 0;

    [[nodiscard]] virtual Entity        Self() const = 0;
    [[nodiscard]] virtual Scene&        GetScene() = 0;
    [[nodiscard]] virtual double        Time() const = 0; // seconds since the script system began
    [[nodiscard]] virtual PhysicsWorld* Physics() = 0;    // may be null
    [[nodiscard]] virtual AssetManager* Assets() = 0;     // may be null
    [[nodiscard]] virtual const Input*  GetInput() = 0;   // null while the game does not have the input
    [[nodiscard]] virtual ScriptValue*  Variable(const std::string& name) = 0; // null: no such variable

    virtual void Print(std::string text, float seconds) = 0;
    virtual void Error(std::string message) = 0; // logged once per node, shown in the editor
    // Flow control: execute(kScriptResume) runs with ResumeData() == data once the chain started
    // by the current return value has ended (loops, Sequence) ...
    virtual void PushContinuation(std::int32_t data) = 0;
    // ... or after `seconds` (latent nodes; the node returns kScriptStop now).
    virtual void Suspend(float seconds, std::int32_t data) = 0;
    [[nodiscard]] virtual std::int32_t ResumeData() const = 0;
    virtual void CallEvent(const std::string& name) = 0; // runs the Custom Event nodes named `name` now
    virtual void KeepModel(std::uint32_t index, std::uint32_t generation) = 0; // spawned: released when play ends
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

    // Pins that depend on the param or the graph (variables, output counts); null: `pins`.
    std::function<std::vector<PinInfo>(const ScriptGraph&, const ScriptNode&)> resolvePins;
    std::function<void(ScriptContext&)>          evaluate; // pure: reads inputs, writes outputs
    std::function<int(ScriptContext&, int entry)> execute; // impure: entry = exec input pin index / kScriptResume
};

[[nodiscard]] std::span<const NodeDesc> ScriptNodeTypes();
[[nodiscard]] const NodeDesc*           FindScriptNodeType(std::string_view type);
// Value of an unconnected input: the node's own default, else the type's, else DefaultValue.
[[nodiscard]] ScriptValue PinDefault(const ScriptNode& node, const NodeDesc* desc, const PinInfo& pin);

// Key names for key events / Is Key Down ("A".."Z", "0".."9", "Space", "Enter", "Escape", "Tab",
// "Left", "Right", "Up", "Down", "LeftShift", "LeftControl", "F1".."F12"). -1: unknown.
[[nodiscard]] int                           KeyFromName(std::string_view name);
[[nodiscard]] std::span<const std::string> KeyNames();

} // namespace Engine
