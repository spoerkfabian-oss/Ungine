#include "Engine/Script/ScriptNodes.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Primitives.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Core/Input.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <random>

namespace Engine {

namespace {

// --- Registry construction helpers --------------------------------------------------------------

PinInfo In(std::string name, PinType type) { return {std::move(name), type, false}; }
PinInfo Out(std::string name, PinType type) { return {std::move(name), type, true}; }
PinInfo ExecIn(std::string name = "In") { return In(std::move(name), PinType::Exec); }
PinInfo ExecOut(std::string name = "Then") { return Out(std::move(name), PinType::Exec); }

using P = PinType;

NodeDesc Pure(std::string type, std::string title, std::string category, std::vector<PinInfo> pins,
              std::function<void(ScriptContext&)> evaluate, std::string tooltip = {})
{
    NodeDesc d;
    d.type     = std::move(type);
    d.title    = std::move(title);
    d.category = std::move(category);
    d.tooltip  = std::move(tooltip);
    d.kind     = NodeKind::Pure;
    d.pins     = std::move(pins);
    d.evaluate = std::move(evaluate);
    return d;
}

// Impure node with "In" and "Then": `run` does the work, then the chain continues at "Then".
NodeDesc Action(std::string type, std::string title, std::string category, std::vector<PinInfo> data,
                std::function<void(ScriptContext&)> run, std::string tooltip = {})
{
    NodeDesc d;
    d.type     = std::move(type);
    d.title    = std::move(title);
    d.category = std::move(category);
    d.tooltip  = std::move(tooltip);
    d.kind     = NodeKind::Impure;
    d.pins     = {ExecIn(), ExecOut()};
    d.pins.insert(d.pins.end(), data.begin(), data.end());
    d.execute = [run = std::move(run)](ScriptContext& c, int) {
        run(c);
        return 1; // "Then"
    };
    return d;
}

NodeDesc Flow(std::string type, std::string title, std::vector<PinInfo> pins, std::function<int(ScriptContext&, int)> execute,
              std::string tooltip = {})
{
    NodeDesc d;
    d.type     = std::move(type);
    d.title    = std::move(title);
    d.category = "Flow";
    d.tooltip  = std::move(tooltip);
    d.kind     = NodeKind::Impure;
    d.pins     = std::move(pins);
    d.execute  = std::move(execute);
    return d;
}

NodeDesc Event(std::string type, std::string title, std::vector<PinInfo> outputs, std::string tooltip = {})
{
    NodeDesc d;
    d.type     = std::move(type);
    d.title    = std::move(title);
    d.category = "Events";
    d.tooltip  = std::move(tooltip);
    d.kind     = NodeKind::Event;
    d.pins     = {ExecOut("Out")};
    d.pins.insert(d.pins.end(), outputs.begin(), outputs.end());
    return d;
}

NodeDesc WithDefaults(NodeDesc d, std::vector<std::pair<std::string, ScriptValue>> defaults)
{
    d.defaults = std::move(defaults);
    return d;
}

NodeDesc WithParam(NodeDesc d, ParamKind kind, std::string label, std::string def, std::vector<std::string> choices = {})
{
    d.paramKind    = kind;
    d.paramLabel   = std::move(label);
    d.paramDefault = std::move(def);
    d.paramChoices = std::move(choices);
    return d;
}

// --- Scene helpers ------------------------------------------------------------------------------

bool Alive(ScriptContext& c, Entity e)
{
    const Registry& r = c.GetScene().GetRegistry();
    return e != NullEntity && r.Valid(e) && r.Has<Transform>(e);
}

// Target entity of a pin; reports invalid ones.
std::optional<Entity> Target(ScriptContext& c, int pin)
{
    const Entity e = c.InEntity(pin);
    if (!Alive(c, e)) {
        c.Error("Target is not a valid entity");
        return std::nullopt;
    }
    return e;
}

glm::mat4 World(ScriptContext& c, Entity e)
{
    c.GetScene().UpdateTransforms(); // cheap when nothing is dirty
    return c.GetScene().GetRegistry().Get<WorldTransform>(e).matrix;
}

glm::mat4 ParentWorld(ScriptContext& c, Entity e)
{
    const Registry& r      = c.GetScene().GetRegistry();
    const Entity    parent = r.Get<Hierarchy>(e).parent;
    return parent != NullEntity && r.Valid(parent) ? World(c, parent) : glm::mat4(1.0f);
}

glm::vec3 ScaleOf(const glm::mat4& m)
{
    return {glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))};
}

glm::quat RotationOf(const glm::mat4& m)
{
    const glm::vec3 s = glm::max(ScaleOf(m), glm::vec3(1e-6f));
    return glm::normalize(glm::quat_cast(glm::mat3(glm::vec3(m[0]) / s.x, glm::vec3(m[1]) / s.y, glm::vec3(m[2]) / s.z)));
}

void SetWorldPosition(ScriptContext& c, Entity e, const glm::vec3& p)
{
    const glm::mat4 parent = ParentWorld(c, e);
    c.GetScene().EditTransform(e).position = glm::vec3(glm::inverse(parent) * glm::vec4(p, 1.0f));
}

void SetWorldRotation(ScriptContext& c, Entity e, const glm::quat& q)
{
    const glm::quat parent = RotationOf(ParentWorld(c, e));
    c.GetScene().EditTransform(e).rotation = glm::normalize(glm::conjugate(parent) * q);
}

glm::quat FromEulerDegrees(const glm::vec3& degrees) { return glm::quat(glm::radians(degrees)); }
glm::vec3 ToEulerDegrees(const glm::quat& q) { return glm::degrees(glm::eulerAngles(q)); }

std::mt19937& Rng()
{
    static thread_local std::mt19937 rng{std::random_device{}()};
    return rng;
}

// Variable nodes: pins typed like the variable (float if it does not exist).
PinType VariableType(const ScriptGraph& graph, const ScriptNode& node)
{
    const ScriptVariable* v = graph.FindVariable(node.param);
    return v ? v->type : PinType::Float;
}

std::uint32_t CountParam(const std::string& param, std::uint32_t min, std::uint32_t max)
{
    std::uint32_t n = min;
    std::from_chars(param.data(), param.data() + param.size(), n);
    return std::clamp(n, min, max);
}

// --- Keys ---------------------------------------------------------------------------------------

struct KeyEntry {
    std::string name;
    int         code;
};

const std::vector<KeyEntry>& KeyTable()
{
    static const std::vector<KeyEntry> table = [] {
        std::vector<KeyEntry> t;
        for (char c = 'A'; c <= 'Z'; ++c)
            t.push_back({std::string(1, c), c});
        for (char c = '0'; c <= '9'; ++c)
            t.push_back({std::string(1, c), c});
        const std::pair<const char*, int> named[] = {
            {"Space", 32},        {"Enter", 257},       {"Escape", 256},    {"Tab", 258},        {"Backspace", 259},
            {"Right", 262},       {"Left", 263},        {"Down", 264},      {"Up", 265},         {"LeftShift", 340},
            {"LeftControl", 341}, {"LeftAlt", 342},     {"RightShift", 344}, {"RightControl", 345}};
        for (const auto& [name, code] : named)
            t.push_back({name, code});
        for (int i = 1; i <= 12; ++i)
            t.push_back({"F" + std::to_string(i), 289 + i});
        return t;
    }();
    return table;
}

// --- The catalog --------------------------------------------------------------------------------

std::vector<NodeDesc> BuildRegistry()
{
    std::vector<NodeDesc> r;
    const auto add = [&](NodeDesc d) { r.push_back(std::move(d)); };

    // Events (outputs filled by ScriptSystem when they fire).
    add(Event("Event.BeginPlay", "Event BeginPlay", {}, "When play starts (or the script is added while playing)"));
    add(Event("Event.Tick", "Event Tick", {Out("Delta Seconds", P::Float)}, "Every frame while playing"));
    add(Event("Event.EndPlay", "Event EndPlay", {}, "When play stops or the script / entity is removed"));
    add(Event("Event.CollisionBegin", "On Collision Begin", {Out("Other", P::Entity), Out("Is Trigger", P::Bool)},
              "This entity starts touching another body (physics)"));
    add(Event("Event.CollisionEnd", "On Collision End", {Out("Other", P::Entity), Out("Is Trigger", P::Bool)},
              "This entity stops touching another body"));
    add(WithParam(Event("Event.KeyPressed", "On Key Pressed", {}, "The key went down this frame"), ParamKind::Key,
                  "Key", "Space"));
    add(WithParam(Event("Event.KeyReleased", "On Key Released", {}, "The key went up this frame"), ParamKind::Key,
                  "Key", "Space"));
    add(WithParam(Event("Event.Custom", "Custom Event", {}, "Runs when a Call Custom Event node with this name runs"),
                  ParamKind::Text, "Name", "MyEvent"));

    // Flow control.
    add(Flow("Flow.Branch", "Branch", {ExecIn(), In("Condition", P::Bool), ExecOut("True"), ExecOut("False")},
             [](ScriptContext& c, int) { return c.InBool(1) ? 2 : 3; }, "If / else"));
    {
        NodeDesc d = Flow("Flow.Sequence", "Sequence", {}, [](ScriptContext& c, int entry) {
            const std::int32_t outputs = static_cast<std::int32_t>(CountParam(c.Param(), 2, 16));
            const std::int32_t next    = entry == kScriptResume ? c.ResumeData() : 0;
            if (next + 1 < outputs)
                c.PushContinuation(next + 1);
            return 1 + next;
        }, "Runs the outputs one after another");
        d.resolvePins = [](const ScriptGraph&, const ScriptNode& node) {
            std::vector<PinInfo> pins{ExecIn()};
            for (std::uint32_t i = 0, n = CountParam(node.param, 2, 16); i < n; ++i)
                pins.push_back(ExecOut("Then " + std::to_string(i)));
            return pins;
        };
        add(WithParam(std::move(d), ParamKind::Count, "Outputs", "2"));
    }
    add(WithDefaults(Flow("Flow.ForLoop", "For Loop",
             {ExecIn(), In("First Index", P::Int), In("Last Index", P::Int), ExecOut("Loop Body"), Out("Index", P::Int),
              ExecOut("Completed")},
             [](ScriptContext& c, int entry) {
                 const std::int32_t index = entry == kScriptResume ? c.ResumeData() : c.InInt(1);
                 if (index > c.InInt(2))
                     return 5;
                 c.Out(4, index);
                 c.PushContinuation(index + 1);
                 return 3;
             },
             "Runs Loop Body for First..Last (inclusive), then Completed"),
                     {{"Last Index", std::int32_t{9}}}));
    add(Flow("Flow.WhileLoop", "While Loop", {ExecIn(), In("Condition", P::Bool), ExecOut("Loop Body"), ExecOut("Completed")},
             [](ScriptContext& c, int) {
                 if (!c.InBool(1))
                     return 3;
                 c.PushContinuation(0);
                 return 2;
             },
             "Runs Loop Body while Condition is true"));
    add(Flow("Flow.DoOnce", "Do Once", {ExecIn(), ExecIn("Reset"), ExecOut("Completed")},
             [](ScriptContext& c, int entry) {
                 ScriptContext::NodeState& s = c.State();
                 if (entry == 1) {
                     s.flag = false;
                     return kScriptStop;
                 }
                 if (s.flag)
                     return kScriptStop;
                 s.flag = true;
                 return 2;
             },
             "Passes only the first time (until Reset)"));
    add(Flow("Flow.FlipFlop", "Flip Flop", {ExecIn(), ExecOut("A"), ExecOut("B"), Out("Is A", P::Bool)},
             [](ScriptContext& c, int) {
                 ScriptContext::NodeState& s = c.State();
                 const bool                a = !s.flag;
                 s.flag                     = a;
                 c.Out(3, a);
                 return a ? 1 : 2;
             },
             "Alternates between A and B"));
    add(Flow("Flow.Gate", "Gate",
             {ExecIn("Enter"), ExecIn("Open"), ExecIn("Close"), ExecIn("Toggle"), In("Start Closed", P::Bool), ExecOut("Exit")},
             [](ScriptContext& c, int entry) {
                 ScriptContext::NodeState& s = c.State();
                 if (!s.flag2) { // first use: initial state
                     s.flag2 = true;
                     s.flag  = !c.InBool(4);
                 }
                 switch (entry) {
                 case 0: return s.flag ? 5 : kScriptStop;
                 case 1: s.flag = true; break;
                 case 2: s.flag = false; break;
                 case 3: s.flag = !s.flag; break;
                 default: break;
                 }
                 return kScriptStop;
             },
             "Enter passes while the gate is open"));
    add(WithDefaults(Flow("Flow.Delay", "Delay", {ExecIn(), In("Duration", P::Float), ExecOut("Completed")},
             [](ScriptContext& c, int entry) {
                 ScriptContext::NodeState& s = c.State();
                 if (entry == kScriptResume) {
                     s.flag = false;
                     return 2;
                 }
                 if (s.flag) // already waiting: ignored (like UE)
                     return kScriptStop;
                 s.flag = true;
                 c.Suspend(std::max(c.InFloat(1), 0.0f), 0);
                 return kScriptStop;
             },
             "Continues after Duration seconds (latent)"),
                     {{"Duration", 1.0f}}));
    add(WithParam(Flow("Flow.CallEvent", "Call Custom Event", {ExecIn(), ExecOut()},
                       [](ScriptContext& c, int) {
                           c.CallEvent(c.Param());
                           return 1;
                       },
                       "Runs the Custom Event nodes with this name"),
                  ParamKind::Text, "Event", "MyEvent"));

    // Variables (pins typed like the variable).
    {
        NodeDesc get = Pure("Variable.Get", "Get", "Variables", {}, [](ScriptContext& c) {
            if (const ScriptValue* v = c.Variable(c.Param()))
                c.Out(0, *v);
            else
                c.Error("Unknown variable '" + c.Param() + "'");
        });
        get.resolvePins = [](const ScriptGraph& g, const ScriptNode& n) {
            return std::vector<PinInfo>{Out("Value", VariableType(g, n))};
        };
        add(WithParam(std::move(get), ParamKind::Variable, "Variable", ""));

        NodeDesc set = Flow("Variable.Set", "Set", {}, [](ScriptContext& c, int) {
            if (ScriptValue* v = c.Variable(c.Param())) {
                *v = Convert(c.In(1), TypeOf(*v));
                c.Out(3, *v);
            } else {
                c.Error("Unknown variable '" + c.Param() + "'");
            }
            return 2;
        });
        set.category    = "Variables";
        set.resolvePins = [](const ScriptGraph& g, const ScriptNode& n) {
            const PinType t = VariableType(g, n);
            return std::vector<PinInfo>{ExecIn(), In("Value", t), ExecOut(), Out("Value", t)};
        };
        add(WithParam(std::move(set), ParamKind::Variable, "Variable", ""));
    }

    // Debug.
    add(WithDefaults(Action("Debug.Print", "Print String", "Debug", {In("Text", P::String), In("Duration", P::Float)},
                            [](ScriptContext& c) { c.Print(c.InString(2), c.InFloat(3)); }, "Log + message on screen"),
                     {{"Text", std::string("Hello")}, {"Duration", 2.0f}}));

    // Math: float.
    const auto binaryF = [&](const char* id, const char* title, auto fn) {
        add(Pure(std::string("Math.") + id + "Float", title, "Math|Float",
                 {In("A", P::Float), In("B", P::Float), Out("Result", P::Float)},
                 [fn](ScriptContext& c) { c.Out(2, fn(c.InFloat(0), c.InFloat(1))); }));
    };
    binaryF("Add", "+ (float)", [](float a, float b) { return a + b; });
    binaryF("Subtract", "- (float)", [](float a, float b) { return a - b; });
    binaryF("Multiply", "* (float)", [](float a, float b) { return a * b; });
    binaryF("Divide", "/ (float)", [](float a, float b) { return b != 0.0f ? a / b : 0.0f; });
    binaryF("Min", "Min (float)", [](float a, float b) { return std::min(a, b); });
    binaryF("Max", "Max (float)", [](float a, float b) { return std::max(a, b); });
    binaryF("Pow", "Power", [](float a, float b) { return std::pow(a, b); });
    binaryF("Mod", "% (float)", [](float a, float b) { return b != 0.0f ? std::fmod(a, b) : 0.0f; });
    const auto unaryF = [&](const char* id, const char* title, auto fn) {
        add(Pure(std::string("Math.") + id, title, "Math|Float", {In("Value", P::Float), Out("Result", P::Float)},
                 [fn](ScriptContext& c) { c.Out(1, fn(c.InFloat(0))); }));
    };
    unaryF("Abs", "Abs", [](float x) { return std::abs(x); });
    unaryF("Negate", "Negate", [](float x) { return -x; });
    unaryF("Sin", "Sin (radians)", [](float x) { return std::sin(x); });
    unaryF("Cos", "Cos (radians)", [](float x) { return std::cos(x); });
    unaryF("Sqrt", "Sqrt", [](float x) { return x > 0.0f ? std::sqrt(x) : 0.0f; });
    unaryF("Floor", "Floor", [](float x) { return std::floor(x); });
    add(Pure("Math.Clamp", "Clamp (float)", "Math|Float",
             {In("Value", P::Float), In("Min", P::Float), In("Max", P::Float), Out("Result", P::Float)},
             [](ScriptContext& c) { c.Out(3, std::clamp(c.InFloat(0), c.InFloat(1), std::max(c.InFloat(1), c.InFloat(2)))); }));
    add(Pure("Math.Lerp", "Lerp", "Math|Float",
             {In("A", P::Float), In("B", P::Float), In("Alpha", P::Float), Out("Result", P::Float)},
             [](ScriptContext& c) { c.Out(3, c.InFloat(0) + (c.InFloat(1) - c.InFloat(0)) * c.InFloat(2)); }));
    add(Pure("Math.RandomFloat", "Random Float in Range", "Math|Float",
             {In("Min", P::Float), In("Max", P::Float), Out("Result", P::Float)}, [](ScriptContext& c) {
                 const float lo = c.InFloat(0), hi = std::max(c.InFloat(0), c.InFloat(1));
                 c.Out(2, std::uniform_real_distribution<float>(lo, hi)(Rng()));
             }));

    // Math: int.
    const auto binaryI = [&](const char* id, const char* title, auto fn) {
        add(Pure(std::string("Math.") + id + "Int", title, "Math|Int", {In("A", P::Int), In("B", P::Int), Out("Result", P::Int)},
                 [fn](ScriptContext& c) { c.Out(2, static_cast<std::int32_t>(fn(c.InInt(0), c.InInt(1)))); }));
    };
    binaryI("Add", "+ (int)", [](std::int64_t a, std::int64_t b) { return a + b; });
    binaryI("Subtract", "- (int)", [](std::int64_t a, std::int64_t b) { return a - b; });
    binaryI("Multiply", "* (int)", [](std::int64_t a, std::int64_t b) { return a * b; });
    binaryI("Divide", "/ (int)", [](std::int64_t a, std::int64_t b) { return b != 0 ? a / b : 0; });
    binaryI("Mod", "% (int)", [](std::int64_t a, std::int64_t b) { return b != 0 ? a % b : 0; });
    add(Pure("Math.RandomInt", "Random Int in Range", "Math|Int", {In("Min", P::Int), In("Max", P::Int), Out("Result", P::Int)},
             [](ScriptContext& c) {
                 const std::int32_t lo = c.InInt(0), hi = std::max(c.InInt(0), c.InInt(1));
                 c.Out(2, std::uniform_int_distribution<std::int32_t>(lo, hi)(Rng()));
             }));

    // Comparison / logic.
    const auto compareF = [&](const char* id, const char* title, auto fn) {
        add(Pure(std::string("Compare.") + id + "Float", title, "Logic", {In("A", P::Float), In("B", P::Float), Out("Result", P::Bool)},
                 [fn](ScriptContext& c) { c.Out(2, static_cast<bool>(fn(c.InFloat(0), c.InFloat(1)))); }));
    };
    compareF("Less", "< (float)", [](float a, float b) { return a < b; });
    compareF("Greater", "> (float)", [](float a, float b) { return a > b; });
    compareF("LessEqual", "<= (float)", [](float a, float b) { return a <= b; });
    compareF("GreaterEqual", ">= (float)", [](float a, float b) { return a >= b; });
    compareF("Equal", "== (float)", [](float a, float b) { return std::abs(a - b) <= 1e-4f; });
    const auto compareI = [&](const char* id, const char* title, auto fn) {
        add(Pure(std::string("Compare.") + id + "Int", title, "Logic", {In("A", P::Int), In("B", P::Int), Out("Result", P::Bool)},
                 [fn](ScriptContext& c) { c.Out(2, static_cast<bool>(fn(c.InInt(0), c.InInt(1)))); }));
    };
    compareI("Less", "< (int)", [](std::int32_t a, std::int32_t b) { return a < b; });
    compareI("Greater", "> (int)", [](std::int32_t a, std::int32_t b) { return a > b; });
    compareI("Equal", "== (int)", [](std::int32_t a, std::int32_t b) { return a == b; });
    add(Pure("Compare.EqualEntity", "== (entity)", "Logic", {In("A", P::Entity), In("B", P::Entity), Out("Result", P::Bool)},
             [](ScriptContext& c) { c.Out(2, c.InEntity(0) == c.InEntity(1)); }));
    const auto logic = [&](const char* id, const char* title, auto fn) {
        add(Pure(std::string("Logic.") + id, title, "Logic", {In("A", P::Bool), In("B", P::Bool), Out("Result", P::Bool)},
                 [fn](ScriptContext& c) { c.Out(2, static_cast<bool>(fn(c.InBool(0), c.InBool(1)))); }));
    };
    logic("And", "AND", [](bool a, bool b) { return a && b; });
    logic("Or", "OR", [](bool a, bool b) { return a || b; });
    logic("Xor", "XOR", [](bool a, bool b) { return a != b; });
    add(Pure("Logic.Not", "NOT", "Logic", {In("Value", P::Bool), Out("Result", P::Bool)},
             [](ScriptContext& c) { c.Out(1, !c.InBool(0)); }));

    // Vectors.
    add(Pure("Vector.Make", "Make Vector", "Math|Vector", {In("X", P::Float), In("Y", P::Float), In("Z", P::Float), Out("Vector", P::Vec3)},
             [](ScriptContext& c) { c.Out(3, glm::vec3(c.InFloat(0), c.InFloat(1), c.InFloat(2))); }));
    add(Pure("Vector.Break", "Break Vector", "Math|Vector",
             {In("Vector", P::Vec3), Out("X", P::Float), Out("Y", P::Float), Out("Z", P::Float)}, [](ScriptContext& c) {
                 const glm::vec3 v = c.InVec3(0);
                 c.Out(1, v.x);
                 c.Out(2, v.y);
                 c.Out(3, v.z);
             }));
    const auto binaryV = [&](const char* id, const char* title, auto fn) {
        add(Pure(std::string("Vector.") + id, title, "Math|Vector", {In("A", P::Vec3), In("B", P::Vec3), Out("Result", P::Vec3)},
                 [fn](ScriptContext& c) { c.Out(2, glm::vec3(fn(c.InVec3(0), c.InVec3(1)))); }));
    };
    binaryV("Add", "+ (vector)", [](glm::vec3 a, glm::vec3 b) { return a + b; });
    binaryV("Subtract", "- (vector)", [](glm::vec3 a, glm::vec3 b) { return a - b; });
    binaryV("Cross", "Cross", [](glm::vec3 a, glm::vec3 b) { return glm::cross(a, b); });
    add(Pure("Vector.Scale", "* (vector, float)", "Math|Vector", {In("Vector", P::Vec3), In("Scale", P::Float), Out("Result", P::Vec3)},
             [](ScriptContext& c) { c.Out(2, c.InVec3(0) * c.InFloat(1)); }));
    add(Pure("Vector.Dot", "Dot", "Math|Vector", {In("A", P::Vec3), In("B", P::Vec3), Out("Result", P::Float)},
             [](ScriptContext& c) { c.Out(2, glm::dot(c.InVec3(0), c.InVec3(1))); }));
    add(Pure("Vector.Length", "Vector Length", "Math|Vector", {In("Vector", P::Vec3), Out("Length", P::Float)},
             [](ScriptContext& c) { c.Out(1, glm::length(c.InVec3(0))); }));
    add(Pure("Vector.Normalize", "Normalize", "Math|Vector", {In("Vector", P::Vec3), Out("Result", P::Vec3)}, [](ScriptContext& c) {
        const glm::vec3 v = c.InVec3(0);
        const float     l = glm::length(v);
        c.Out(1, l > 1e-8f ? v / l : glm::vec3(0.0f));
    }));
    add(Pure("Vector.Distance", "Distance", "Math|Vector", {In("A", P::Vec3), In("B", P::Vec3), Out("Distance", P::Float)},
             [](ScriptContext& c) { c.Out(2, glm::distance(c.InVec3(0), c.InVec3(1))); }));
    add(Pure("Vector.Lerp", "Lerp (vector)", "Math|Vector",
             {In("A", P::Vec3), In("B", P::Vec3), In("Alpha", P::Float), Out("Result", P::Vec3)},
             [](ScriptContext& c) { c.Out(3, glm::mix(c.InVec3(0), c.InVec3(1), c.InFloat(2))); }));

    // Conversion / strings.
    add(Pure("Convert.IntToFloat", "Int to Float", "Convert", {In("Value", P::Int), Out("Result", P::Float)},
             [](ScriptContext& c) { c.Out(1, static_cast<float>(c.InInt(0))); }));
    add(Pure("Convert.FloatToInt", "Float to Int (truncate)", "Convert", {In("Value", P::Float), Out("Result", P::Int)},
             [](ScriptContext& c) { c.Out(1, c.InInt(0)); }));
    add(Pure("Convert.Round", "Round", "Convert", {In("Value", P::Float), Out("Result", P::Int)},
             [](ScriptContext& c) { c.Out(1, std::get<std::int32_t>(Convert(std::round(c.InFloat(0)), P::Int))); }));
    add(Pure("String.Append", "Append", "String", {In("A", P::String), In("B", P::String), Out("Result", P::String)},
             [](ScriptContext& c) { c.Out(2, c.InString(0) + c.InString(1)); }));
    add(Pure("String.FromFloat", "To String (float)", "String", {In("Value", P::Float), Out("Result", P::String)},
             [](ScriptContext& c) { c.Out(1, c.InString(0)); }));
    add(Pure("String.FromVector", "To String (vector)", "String", {In("Value", P::Vec3), Out("Result", P::String)},
             [](ScriptContext& c) { c.Out(1, c.InString(0)); }));
    add(Pure("String.FromEntity", "Get Display Name", "String", {In("Entity", P::Entity), Out("Result", P::String)},
             [](ScriptContext& c) {
                 const Entity e = c.InEntity(0);
                 c.Out(1, Alive(c, e) ? c.GetScene().GetRegistry().Get<Name>(e).value : std::string("none"));
             }));

    // Time / input.
    add(Pure("Time.GameTime", "Get Game Time", "Utilities", {Out("Seconds", P::Float)},
             [](ScriptContext& c) { c.Out(0, static_cast<float>(c.Time())); }, "Seconds since play started"));
    add(WithParam(Pure("Input.IsKeyDown", "Is Key Down", "Input", {Out("Down", P::Bool)},
                       [](ScriptContext& c) {
                           const Input* input = c.GetInput();
                           const int    key   = KeyFromName(c.Param());
                           c.Out(0, input && key >= 0 && input->IsKeyDown(key));
                       }),
                  ParamKind::Key, "Key", "Space"));
    add(Pure("Input.MoveAxis", "Get Move Input (WASD)", "Input", {Out("Axis", P::Vec3)}, [](ScriptContext& c) {
        const Input* in = c.GetInput();
        glm::vec3    v(0.0f);
        if (in) {
            v.x = (in->IsKeyDown('D') ? 1.0f : 0.0f) - (in->IsKeyDown('A') ? 1.0f : 0.0f);
            v.z = (in->IsKeyDown('S') ? 1.0f : 0.0f) - (in->IsKeyDown('W') ? 1.0f : 0.0f);
        }
        c.Out(0, v);
    }, "x = D - A, z = S - W (forward is -Z)"));

    // Entities.
    add(Pure("Entity.Self", "Self", "Entity", {Out("Self", P::Entity)}, [](ScriptContext& c) { c.Out(0, c.Self()); },
             "The entity this script belongs to"));
    add(Pure("Entity.FindByName", "Find Entity by Name", "Entity", {In("Name", P::String), Out("Entity", P::Entity)},
             [](ScriptContext& c) {
                 const std::string name  = c.InString(0);
                 Entity            found = NullEntity;
                 c.GetScene().GetRegistry().ViewOf<Name>().Each([&](Entity e, Name& n) {
                     if (found == NullEntity && n.value == name)
                         found = e;
                 });
                 c.Out(1, found);
             }));
    add(Pure("Entity.IsValid", "Is Valid", "Entity", {In("Entity", P::Entity), Out("Valid", P::Bool)},
             [](ScriptContext& c) { c.Out(1, Alive(c, c.InEntity(0))); }));
    add(Pure("Entity.GetPosition", "Get World Position", "Transform", {In("Target", P::Entity), Out("Position", P::Vec3)},
             [](ScriptContext& c) {
                 if (const auto e = Target(c, 0))
                     c.Out(1, glm::vec3(World(c, *e)[3]));
             }));
    add(Pure("Entity.GetRotation", "Get World Rotation", "Transform", {In("Target", P::Entity), Out("Rotation", P::Vec3)},
             [](ScriptContext& c) {
                 if (const auto e = Target(c, 0))
                     c.Out(1, ToEulerDegrees(RotationOf(World(c, *e))));
             }, "Euler angles in degrees (X, Y, Z)"));
    add(Pure("Entity.GetScale", "Get Scale", "Transform", {In("Target", P::Entity), Out("Scale", P::Vec3)}, [](ScriptContext& c) {
        if (const auto e = Target(c, 0))
            c.Out(1, c.GetScene().GetTransform(*e).scale);
    }, "Local scale"));
    add(Pure("Entity.GetForward", "Get Forward Vector", "Transform", {In("Target", P::Entity), Out("Forward", P::Vec3)},
             [](ScriptContext& c) {
                 if (const auto e = Target(c, 0))
                     c.Out(1, glm::normalize(-glm::vec3(World(c, *e)[2])));
             }, "World -Z of the entity"));
    add(Action("Entity.SetPosition", "Set World Position", "Transform", {In("Target", P::Entity), In("Position", P::Vec3)},
               [](ScriptContext& c) {
                   if (const auto e = Target(c, 2))
                       SetWorldPosition(c, *e, c.InVec3(3));
               }));
    add(Action("Entity.AddOffset", "Add World Offset", "Transform", {In("Target", P::Entity), In("Offset", P::Vec3)},
               [](ScriptContext& c) {
                   if (const auto e = Target(c, 2))
                       SetWorldPosition(c, *e, glm::vec3(World(c, *e)[3]) + c.InVec3(3));
               }));
    add(Action("Entity.SetRotation", "Set World Rotation", "Transform", {In("Target", P::Entity), In("Rotation", P::Vec3)},
               [](ScriptContext& c) {
                   if (const auto e = Target(c, 2))
                       SetWorldRotation(c, *e, FromEulerDegrees(c.InVec3(3)));
               }, "Euler angles in degrees"));
    add(Action("Entity.AddRotation", "Add World Rotation", "Transform", {In("Target", P::Entity), In("Degrees", P::Vec3)},
               [](ScriptContext& c) {
                   if (const auto e = Target(c, 2))
                       SetWorldRotation(c, *e, glm::normalize(FromEulerDegrees(c.InVec3(3)) * RotationOf(World(c, *e))));
               }, "Rotates by Euler angles in degrees (world axes)"));
    add(Action("Entity.SetScale", "Set Scale", "Transform", {In("Target", P::Entity), In("Scale", P::Vec3)}, [](ScriptContext& c) {
        if (const auto e = Target(c, 2))
            c.GetScene().EditTransform(*e).scale = c.InVec3(3);
    }, "Local scale"));
    add(Action("Entity.Destroy", "Destroy Entity", "Entity", {In("Target", P::Entity)}, [](ScriptContext& c) {
        if (const auto e = Target(c, 2))
            c.GetScene().DestroyEntity(*e);
    }, "Destroys the entity and its children"));
    add(WithParam(WithDefaults(Action("Entity.SpawnPrimitive", "Spawn Primitive", "Entity",
                         {In("Location", P::Vec3), In("Size", P::Float), In("Simulate Physics", P::Bool), In("Name", P::String),
                          Out("Spawned", P::Entity)},
                         [](ScriptContext& c) {
                             AssetManager* assets = c.Assets();
                             if (!assets) {
                                 c.Error("No asset manager");
                                 return;
                             }
                             const PrimitiveShape shape = PrimitiveShapeFromString(c.Param()).value_or(PrimitiveShape::Box);
                             const float          size  = std::max(c.InFloat(3), 0.01f);
                             const ModelHandle model = assets->CreatePrimitive({.shape = shape, .size = size});
                             c.KeepModel(model.index, model.generation);
                             Scene&            scene = c.GetScene();
                             const std::string name  = c.InString(5);
                             const Entity e = scene.CreateEntity(name.empty() ? std::string(ToString(shape)) : name);
                             scene.EditTransform(e).position = c.InVec3(2);
                             Registry& r = scene.GetRegistry();
                             r.Emplace<MeshRenderer>(e, MeshRenderer{.model = model, .meshIndex = 0});
                             if (c.InBool(4)) {
                                 const float h = size * 0.5f;
                                 Collider    collider;
                                 switch (shape) {
                                 case PrimitiveShape::Box: collider.halfExtents = glm::vec3(h); break;
                                 case PrimitiveShape::Plane:
                                     collider.halfExtents = {h, 0.01f, h};
                                     break;
                                 case PrimitiveShape::Sphere:
                                     collider.shape  = ColliderShape::Sphere;
                                     collider.radius = h;
                                     break;
                                 case PrimitiveShape::Capsule:
                                     collider.shape      = ColliderShape::Capsule;
                                     collider.radius     = h;
                                     collider.halfHeight = h;
                                     break;
                                 }
                                 r.Emplace<Collider>(e, collider);
                                 r.Emplace<RigidBody>(e, RigidBody{.type = shape == PrimitiveShape::Plane ? BodyType::Static
                                                                                                          : BodyType::Dynamic});
                             }
                             c.Out(6, e);
                         }, "Creates a primitive entity (optionally a dynamic body); removed when play stops"),
                               {{"Size", 0.5f}, {"Simulate Physics", true}}),
                  ParamKind::Choice, "Shape", "box", {"box", "sphere", "capsule", "plane"}));

    // Physics.
    add(Action("Physics.AddImpulse", "Add Impulse", "Physics", {In("Target", P::Entity), In("Impulse", P::Vec3)},
               [](ScriptContext& c) {
                   if (const auto e = Target(c, 2); e && c.Physics())
                       c.Physics()->AddImpulse(*e, c.InVec3(3));
               }, "Dynamic bodies (kg*m/s)"));
    add(Action("Physics.SetVelocity", "Set Linear Velocity", "Physics", {In("Target", P::Entity), In("Velocity", P::Vec3)},
               [](ScriptContext& c) {
                   if (const auto e = Target(c, 2); e && c.Physics())
                       c.Physics()->SetLinearVelocity(*e, c.InVec3(3));
               }));
    add(Pure("Physics.GetVelocity", "Get Linear Velocity", "Physics", {In("Target", P::Entity), Out("Velocity", P::Vec3)},
             [](ScriptContext& c) {
                 if (const auto e = Target(c, 0); e && c.Physics())
                     c.Out(1, c.Physics()->LinearVelocity(*e));
             }));
    add(WithDefaults(Action("Physics.Raycast", "Raycast", "Physics",
               {In("Start", P::Vec3), In("Direction", P::Vec3), In("Distance", P::Float), In("Ignore", P::Entity),
                Out("Hit", P::Bool), Out("Hit Entity", P::Entity), Out("Hit Point", P::Vec3), Out("Hit Normal", P::Vec3)},
               [](ScriptContext& c) {
                   const float distance = c.InFloat(4);
                   std::optional<PhysicsHit> hit;
                   if (c.Physics())
                       hit = c.Physics()->Raycast(c.InVec3(2), c.InVec3(3), distance, c.Connected(5) ? c.InEntity(5) : c.Self());
                   c.Out(6, hit.has_value());
                   c.Out(7, hit ? hit->entity : NullEntity);
                   c.Out(8, hit ? hit->point : glm::vec3(0.0f));
                   c.Out(9, hit ? hit->normal : glm::vec3(0.0f));
               }, "Closest body hit (Ignore: self when unconnected)"),
                     {{"Direction", glm::vec3(0.0f, -1.0f, 0.0f)}, {"Distance", 1000.0f}}));
    add(Action("Physics.CharacterInput", "Set Character Input", "Physics",
               {In("Target", P::Entity), In("Move", P::Vec3), In("Jump", P::Bool)}, [](ScriptContext& c) {
                   if (const auto e = Target(c, 2); e && c.Physics())
                       c.Physics()->SetCharacterInput(*e, c.InVec3(3), c.InBool(4));
               }, "Character Controller: horizontal velocity (m/s) and jump"));

    // Lights.
    add(Action("Light.SetColor", "Set Light Color", "Light", {In("Target", P::Entity), In("Color", P::Vec3)}, [](ScriptContext& c) {
        const auto e = Target(c, 2);
        if (Light* light = e ? c.GetScene().GetRegistry().TryGet<Light>(*e) : nullptr) {
            light->color = glm::max(c.InVec3(3), glm::vec3(0.0f));
            c.GetScene().MarkChanged(*e);
        } else if (e) {
            c.Error("Target has no Light");
        }
    }));
    add(Action("Light.SetIntensity", "Set Light Intensity", "Light", {In("Target", P::Entity), In("Intensity", P::Float)},
               [](ScriptContext& c) {
                   const auto e = Target(c, 2);
                   if (Light* light = e ? c.GetScene().GetRegistry().TryGet<Light>(*e) : nullptr) {
                       light->intensity = std::max(c.InFloat(3), 0.0f);
                       c.GetScene().MarkChanged(*e);
                   } else if (e) {
                       c.Error("Target has no Light");
                   }
               }));
    add(Pure("Light.GetIntensity", "Get Light Intensity", "Light", {In("Target", P::Entity), Out("Intensity", P::Float)},
             [](ScriptContext& c) {
                 const auto e = Target(c, 0);
                 if (const Light* light = e ? c.GetScene().GetRegistry().TryGet<Light>(*e) : nullptr)
                     c.Out(1, light->intensity);
             }));

    // Audio. Sound paths: absolute or relative to the working directory (the project root).
    const auto busParam = [](NodeDesc desc, const char* fallback) {
        return WithParam(std::move(desc), ParamKind::Choice, "Bus", fallback, {"world", "music", "ui", "ambient"});
    };
    const auto bus = [](ScriptContext& c, AudioBus fallback) { return AudioBusFromString(c.Param()).value_or(fallback); };
    add(busParam(WithDefaults(Action("Audio.PlaySound", "Play Sound 2D", "Audio", {In("Sound", P::String), In("Volume", P::Float)},
                                     [bus](ScriptContext& c) {
                                         if (!c.Audio())
                                             return c.Error("No audio system");
                                         if (!c.Audio()->Play2D(c.InString(2), c.InFloat(3), bus(c, AudioBus::Ui)))
                                             c.Error("Cannot play '" + c.InString(2) + "'");
                                     }, "Non-positional one-shot (UI, music stingers)"),
                              {{"Volume", 1.0f}}),
                 "ui"));
    add(busParam(WithDefaults(Action("Audio.PlaySoundAt", "Play Sound at Location", "Audio",
                                     {In("Sound", P::String), In("Location", P::Vec3), In("Volume", P::Float), In("Pitch", P::Float)},
                                     [bus](ScriptContext& c) {
                                         if (!c.Audio())
                                             return c.Error("No audio system");
                                         if (!c.Audio()->PlayAt(c.InString(2), c.InVec3(3), c.InFloat(4), c.InFloat(5), bus(c, AudioBus::World)))
                                             c.Error("Cannot play '" + c.InString(2) + "'");
                                     }, "3D one-shot at a point (impacts, footsteps)"),
                              {{"Volume", 1.0f}, {"Pitch", 1.0f}}),
                 "world"));
    add(Action("Audio.Play", "Play Audio Source", "Audio", {In("Target", P::Entity)}, [](ScriptContext& c) {
        const auto e = Target(c, 2);
        if (!e || !c.Audio())
            return;
        if (!c.GetScene().GetRegistry().Has<AudioSource>(*e))
            return c.Error("Target has no Audio Source");
        c.Audio()->Play(*e);
    }, "(Re)starts the entity's Audio Source"));
    add(Action("Audio.Stop", "Stop Audio Source", "Audio", {In("Target", P::Entity), In("Fade Out", P::Float)},
               [](ScriptContext& c) {
                   if (const auto e = Target(c, 2); e && c.Audio())
                       c.Audio()->Stop(*e, std::max(c.InFloat(3), 0.0f));
               }, "Fade Out in seconds"));
    add(Pure("Audio.IsPlaying", "Is Audio Playing", "Audio", {In("Target", P::Entity), Out("Playing", P::Bool)},
             [](ScriptContext& c) {
                 const auto e = Target(c, 0);
                 c.Out(1, e && c.Audio() && c.Audio()->IsPlaying(*e));
             }));
    add(Action("Audio.SetVolume", "Set Audio Volume", "Audio", {In("Target", P::Entity), In("Volume", P::Float)},
               [](ScriptContext& c) {
                   const auto e = Target(c, 2);
                   if (AudioSource* a = e ? c.GetScene().GetRegistry().TryGet<AudioSource>(*e) : nullptr)
                       a->volume = std::max(c.InFloat(3), 0.0f);
                   else if (e)
                       c.Error("Target has no Audio Source");
               }, "Audio Source volume (applied smoothly while playing)"));
    add(Action("Audio.SetPitch", "Set Audio Pitch", "Audio", {In("Target", P::Entity), In("Pitch", P::Float)},
               [](ScriptContext& c) {
                   const auto e = Target(c, 2);
                   if (AudioSource* a = e ? c.GetScene().GetRegistry().TryGet<AudioSource>(*e) : nullptr)
                       a->pitch = std::clamp(c.InFloat(3), 0.01f, 16.0f);
                   else if (e)
                       c.Error("Target has no Audio Source");
               }, "Playback rate: 2 = one octave up"));
    add(busParam(WithDefaults(Action("Audio.SetBusVolume", "Set Bus Volume", "Audio", {In("Volume", P::Float)},
                                     [bus](ScriptContext& c) {
                                         if (c.Audio())
                                             c.Audio()->Engine().SetBusVolume(bus(c, AudioBus::Music), std::max(c.InFloat(2), 0.0f));
                                     }, "Mixer bus volume until play stops (options menus, ducking)"),
                              {{"Volume", 1.0f}}),
                 "music"));
    return r;
}

} // namespace

std::span<const NodeDesc> ScriptNodeTypes()
{
    static const std::vector<NodeDesc> registry = BuildRegistry();
    return registry;
}

ScriptValue PinDefault(const ScriptNode& node, const NodeDesc* desc, const PinInfo& pin)
{
    if (const auto it = node.defaults.find(pin.name); it != node.defaults.end())
        return Convert(it->second, pin.type);
    if (desc)
        for (const auto& [name, value] : desc->defaults)
            if (name == pin.name)
                return Convert(value, pin.type);
    return DefaultValue(pin.type);
}

const NodeDesc* FindScriptNodeType(std::string_view type)
{
    const auto nodes = ScriptNodeTypes();
    const auto it    = std::ranges::find_if(nodes, [&](const NodeDesc& d) { return d.type == type; });
    return it != nodes.end() ? &*it : nullptr;
}

int KeyFromName(std::string_view name)
{
    for (const KeyEntry& k : KeyTable())
        if (k.name == name)
            return k.code;
    return -1;
}

std::span<const std::string> KeyNames()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const KeyEntry& k : KeyTable())
            n.push_back(k.name);
        return n;
    }();
    return names;
}

} // namespace Engine
