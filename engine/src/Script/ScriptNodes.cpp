#include "Engine/Script/ScriptNodes.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Primitives.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Core/Input.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Script/ScriptRegistry.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <random>
#include <unordered_map>

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
    const ScriptVariable* v = graph.FindVariableInScope(node.function, node.param);
    return v ? v->type : PinType::Float;
}

// Element type of an array variable node (float if it is none).
PinType VariableElement(const ScriptGraph& graph, const ScriptNode& node)
{
    const PinType t = VariableType(graph, node);
    return IsArray(t) ? ElementType(t) : PinType::Float;
}

// Element type named by a param ("int", "struct:Item", "vec3:3" -> Vec3); float if it names none.
PinType ElementParam(const std::string& param)
{
    const auto t = PinTypeFromString(SplitTypeAndCount(param, 0).first);
    return t && t->kind != PinKind::Exec && !IsContainer(*t) ? *t : PinType::Float;
}

// Any value type named by a param (scalars and containers).
PinType ValueParam(const std::string& param)
{
    const auto t = PinTypeFromString(param);
    return t && t->kind != PinKind::Exec ? *t : PinType::Float;
}

std::vector<ScriptParam> FunctionParams(const ScriptGraph& graph, const std::string& name, bool inputs)
{
    const ScriptFunction* f = graph.FindFunction(name);
    return f ? (inputs ? f->inputs : f->outputs) : std::vector<ScriptParam>{};
}

int MouseButtonFromName(const std::string& name) { return name == "Right" ? 1 : name == "Middle" ? 2 : 0; }

// The camera of a pin: unconnected = the scene's primary camera (not self).
Entity CameraOf(ScriptContext& c, int pin)
{
    return c.Connected(pin) ? c.InEntity(pin) : c.GetScene().FindPrimaryCamera();
}

// "Item.count" -> {"Item", "count"} (the part after the first dot may be empty).
std::pair<std::string, std::string> SplitDotted(const std::string& param)
{
    const std::size_t dot = param.find('.');
    return dot == std::string::npos ? std::pair{param, std::string()} : std::pair{param.substr(0, dot), param.substr(dot + 1)};
}

// Comma separated case values, trimmed.
std::vector<std::string> SplitCases(const std::string& param)
{
    std::vector<std::string> cases;
    std::size_t              start = 0;
    while (start <= param.size()) {
        const std::size_t comma = std::min(param.find(',', start), param.size());
        std::string       c     = param.substr(start, comma - start);
        c.erase(0, c.find_first_not_of(' '));
        c.erase(c.find_last_not_of(' ') + 1);
        if (!c.empty())
            cases.push_back(std::move(c));
        start = comma + 1;
    }
    return cases;
}

// Map type of a param / map variable (a string -> int map if it names none).
PinType MapParam(const std::string& param)
{
    const PinType t = ValueParam(param);
    return IsMap(t) ? t : PinType::Map(PinType::String, PinType::Int);
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
    r.back().latent = true;
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
    add(Pure("Entity.FindByName", "Find Entity by Name", "Entity",
             {In("Name", P::String), Out("Entity", P::Entity), Out("Found", P::Bool)}, [](ScriptContext& c) {
                 const std::string name  = c.InString(0);
                 Entity            found = NullEntity;
                 c.GetScene().GetRegistry().ViewOf<Name>().Each([&](Entity e, Name& n) {
                     if (found == NullEntity && n.value == name)
                         found = e;
                 });
                 c.Out(1, found);
                 c.Out(2, found != NullEntity);
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
    add(Action("Entity.SpawnPrefab", "Spawn Prefab", "Entity",
               {In("Prefab", P::String), In("Location", P::Vec3), In("Rotation", P::Vec3), Out("Spawned", P::Entity)},
               [](ScriptContext& c) {
                   const std::string file = c.InString(2);
                   if (file.empty()) {
                       c.Error("No prefab file");
                       return;
                   }
                   Transform t;
                   t.position = c.InVec3(3);
                   t.rotation = FromEulerDegrees(c.InVec3(4));
                   std::vector<ModelHandle> models;
                   try {
                       const Entity e = InstantiatePrefab(c.GetScene(), c.Assets(), file, NullEntity, t, models);
                       c.Out(5, e);
                   } catch (const std::exception& ex) {
                       c.Error(ex.what());
                   }
                   for (ModelHandle h : models)
                       c.KeepModel(h.index, h.generation);
               }, "Instantiates a .uprefab (path relative to the project) at a world location (Euler degrees); "
                  "its scripts start on the next frame"));

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

    // Functions (created by the editor's function list; their nodes are not in the palette).
    {
        NodeDesc entry;
        entry.type     = "Function.Entry";
        entry.title    = "Function Entry";
        entry.category = "Functions";
        entry.kind     = NodeKind::Event;
        entry.hidden   = true;
        entry.resolvePins = [](const ScriptGraph& g, const ScriptNode& n) {
            std::vector<PinInfo> pins{ExecOut()};
            for (const ScriptParam& p : FunctionParams(g, n.param, true))
                pins.push_back(Out(p.name, p.type));
            return pins;
        };
        add(WithParam(std::move(entry), ParamKind::Function, "Function", ""));

        NodeDesc ret = Flow("Function.Return", "Return", {}, [](ScriptContext&, int) { return kScriptReturn; },
                            "Ends the function and hands its outputs to the caller");
        ret.category    = "Functions";
        ret.hidden      = true;
        ret.resolvePins = [](const ScriptGraph& g, const ScriptNode& n) {
            std::vector<PinInfo> pins{ExecIn()};
            for (const ScriptParam& p : FunctionParams(g, n.param, false))
                pins.push_back(In(p.name, p.type));
            return pins;
        };
        add(WithParam(std::move(ret), ParamKind::Function, "Function", ""));

        // Calls are run by ScriptSystem itself (execute / evaluate are never reached).
        NodeDesc call = Flow("Function.Call", "Call Function", {}, [](ScriptContext&, int) { return 1; });
        call.category    = "Functions";
        call.hidden      = true;
        call.resolvePins = [](const ScriptGraph& g, const ScriptNode& n) {
            std::vector<PinInfo> pins{ExecIn(), ExecOut()};
            for (const ScriptParam& p : FunctionParams(g, n.param, true))
                pins.push_back(In(p.name, p.type));
            for (const ScriptParam& p : FunctionParams(g, n.param, false))
                pins.push_back(Out(p.name, p.type));
            return pins;
        };
        add(WithParam(std::move(call), ParamKind::Function, "Function", ""));

        NodeDesc pure = Pure("Function.CallPure", "Call Pure Function", "Functions", {}, [](ScriptContext&) {});
        pure.hidden      = true;
        pure.resolvePins = [](const ScriptGraph& g, const ScriptNode& n) {
            std::vector<PinInfo> pins;
            for (const ScriptParam& p : FunctionParams(g, n.param, true))
                pins.push_back(In(p.name, p.type));
            for (const ScriptParam& p : FunctionParams(g, n.param, false))
                pins.push_back(Out(p.name, p.type));
            return pins;
        };
        add(WithParam(std::move(pure), ParamKind::Function, "Function", ""));
    }

    // Arrays. Read nodes take the element type as param (adapts when an array is connected);
    // nodes that change an array work on an array variable.
    const auto arrayRead = [&](const char* type, const char* title, std::function<std::vector<PinInfo>(PinType)> pins,
                               std::function<void(ScriptContext&, PinType)> eval, const char* tooltip = "") {
        NodeDesc d = Pure(type, title, "Array", {}, [eval](ScriptContext& c) { eval(c, ElementParam(c.Param())); }, tooltip);
        d.resolvePins = [pins](const ScriptGraph&, const ScriptNode& n) { return pins(ElementParam(n.param)); };
        d.inference   = ParamInference::ElementType;
        add(WithParam(std::move(d), ParamKind::ElementType, "Element", "float"));
    };
    arrayRead("Array.Length", "Length", [](PinType e) { return std::vector<PinInfo>{In("Array", ArrayOf(e)), Out("Length", P::Int)}; },
              [](ScriptContext& c, PinType) { c.Out(1, static_cast<std::int32_t>(ArrayItems(c.In(0)).items.size())); });
    arrayRead("Array.IsEmpty", "Is Empty", [](PinType e) { return std::vector<PinInfo>{In("Array", ArrayOf(e)), Out("Empty", P::Bool)}; },
              [](ScriptContext& c, PinType) { c.Out(1, ArrayItems(c.In(0)).items.empty()); });
    arrayRead("Array.Get", "Get (a copy)",
              [](PinType e) {
                  return std::vector<PinInfo>{In("Array", ArrayOf(e)), In("Index", P::Int), Out("Item", e), Out("Valid", P::Bool)};
              },
              [](ScriptContext& c, PinType e) {
                  const ScriptValue  value = c.In(0);
                  const ScriptArray& a     = ArrayItems(value);
                  const std::int32_t i     = c.InInt(1);
                  const bool         valid = i >= 0 && static_cast<std::size_t>(i) < a.items.size();
                  c.Out(2, valid ? a.items[static_cast<std::size_t>(i)] : DefaultValue(e));
                  c.Out(3, valid);
              }, "Item at Index (0-based); Valid is false outside the array");
    arrayRead("Array.Last", "Last",
              [](PinType e) { return std::vector<PinInfo>{In("Array", ArrayOf(e)), Out("Item", e), Out("Valid", P::Bool)}; },
              [](ScriptContext& c, PinType e) {
                  const ScriptValue  value = c.In(0);
                  const ScriptArray& a     = ArrayItems(value);
                  c.Out(1, a.items.empty() ? DefaultValue(e) : a.items.back());
                  c.Out(2, !a.items.empty());
              });
    arrayRead("Array.Contains", "Contains",
              [](PinType e) { return std::vector<PinInfo>{In("Array", ArrayOf(e)), In("Item", e), Out("Result", P::Bool)}; },
              [](ScriptContext& c, PinType) {
                  const ScriptValue value = c.In(0), item = c.In(1);
                  c.Out(2, std::ranges::any_of(ArrayItems(value).items, [&](const ScriptValue& v) { return ValuesEqual(v, item); }));
              });
    arrayRead("Array.Find", "Find",
              [](PinType e) { return std::vector<PinInfo>{In("Array", ArrayOf(e)), In("Item", e), Out("Index", P::Int)}; },
              [](ScriptContext& c, PinType) {
                  const ScriptValue  value = c.In(0), item = c.In(1);
                  const ScriptArray& a     = ArrayItems(value);
                  std::int32_t       index = -1;
                  for (std::size_t i = 0; i < a.items.size() && index < 0; ++i)
                      if (ValuesEqual(a.items[i], item))
                          index = static_cast<std::int32_t>(i);
                  c.Out(2, index);
              }, "First index of Item, -1 if missing");
    {
        const auto count = [](const std::string& param) {
            return static_cast<std::uint32_t>(std::clamp(SplitTypeAndCount(param, 2).second, 1, 16));
        };
        NodeDesc d = Pure("Array.Make", "Make Array", "Array", {}, [count](ScriptContext& c) {
            const PinType            e = ElementParam(c.Param());
            const std::uint32_t      n = count(c.Param());
            std::vector<ScriptValue> items;
            for (std::uint32_t i = 0; i < n; ++i)
                items.push_back(Convert(c.In(static_cast<int>(i)), e));
            c.Out(static_cast<int>(n), MakeArray(e, std::move(items)));
        });
        d.resolvePins = [count](const ScriptGraph&, const ScriptNode& n) {
            const PinType        e = ElementParam(n.param);
            std::vector<PinInfo> pins;
            for (std::uint32_t i = 0, c = count(n.param); i < c; ++i)
                pins.push_back(In(std::to_string(i), e));
            pins.push_back(Out("Array", ArrayOf(e)));
            return pins;
        };
        d.inference = ParamInference::ElementType;
        add(WithParam(std::move(d), ParamKind::TypeAndCount, "Element : count", "float:2"));
    }
    const auto arrayWrite = [&](const char* type, const char* title, std::function<std::vector<PinInfo>(PinType)> data,
                                std::function<void(ScriptContext&, ScriptValue&, PinType)> run, const char* tooltip = "") {
        NodeDesc d = Flow(type, title, {}, [run](ScriptContext& c, int) {
            ScriptValue* v = c.Variable(c.Param());
            if (!v || !IsArray(TypeOf(*v))) {
                c.Error("'" + c.Param() + "' is not an array variable");
                return 1;
            }
            run(c, *v, ElementType(TypeOf(*v))); // reads its inputs before changing the array
            return 1;
        }, tooltip);
        d.category      = "Array";
        d.arrayVariable = true;
        d.resolvePins   = [data](const ScriptGraph& g, const ScriptNode& n) {
            std::vector<PinInfo> pins{ExecIn(), ExecOut()};
            const std::vector<PinInfo> extra = data(VariableElement(g, n));
            pins.insert(pins.end(), extra.begin(), extra.end());
            return pins;
        };
        add(WithParam(std::move(d), ParamKind::Variable, "Array", ""));
    };
    arrayWrite("Array.Add", "Add", [](PinType e) { return std::vector<PinInfo>{In("Item", e), Out("Index", P::Int)}; },
               [](ScriptContext& c, ScriptValue& v, PinType e) {
                   ScriptValue  item = c.In(2);
                   ScriptArray& a    = MutableArray(v, e);
                   a.items.push_back(std::move(item));
                   c.Out(3, static_cast<std::int32_t>(a.items.size() - 1));
               }, "Appends Item to the array variable");
    arrayWrite("Array.AddUnique", "Add Unique", [](PinType e) { return std::vector<PinInfo>{In("Item", e), Out("Index", P::Int)}; },
               [](ScriptContext& c, ScriptValue& v, PinType e) {
                   ScriptValue  item = c.In(2);
                   ScriptArray& a    = MutableArray(v, e);
                   const auto   it   = std::ranges::find_if(a.items, [&](const ScriptValue& x) { return ValuesEqual(x, item); });
                   if (it == a.items.end()) {
                       a.items.push_back(std::move(item));
                       c.Out(3, static_cast<std::int32_t>(a.items.size() - 1));
                   } else {
                       c.Out(3, static_cast<std::int32_t>(it - a.items.begin()));
                   }
               }, "Appends Item unless it is already in the array");
    arrayWrite("Array.Insert", "Insert", [](PinType e) { return std::vector<PinInfo>{In("Item", e), In("Index", P::Int)}; },
               [](ScriptContext& c, ScriptValue& v, PinType e) {
                   ScriptValue        item  = c.In(2);
                   const std::int32_t index = c.InInt(3);
                   ScriptArray&       a     = MutableArray(v, e);
                   const auto         at    = std::clamp<std::int64_t>(index, 0, static_cast<std::int64_t>(a.items.size()));
                   a.items.insert(a.items.begin() + at, std::move(item));
               }, "Inserts Item before Index (clamped)");
    arrayWrite("Array.SetAt", "Set Array Element", [](PinType e) { return std::vector<PinInfo>{In("Index", P::Int), In("Item", e)}; },
               [](ScriptContext& c, ScriptValue& v, PinType e) {
                   const std::int32_t index = c.InInt(2);
                   ScriptValue        item  = c.In(3);
                   ScriptArray&       a     = MutableArray(v, e);
                   if (index >= 0 && static_cast<std::size_t>(index) < a.items.size())
                       a.items[static_cast<std::size_t>(index)] = std::move(item);
                   else
                       c.Error("Index " + std::to_string(index) + " is outside the array");
               });
    arrayWrite("Array.RemoveAt", "Remove Index", [](PinType) { return std::vector<PinInfo>{In("Index", P::Int), Out("Removed", P::Bool)}; },
               [](ScriptContext& c, ScriptValue& v, PinType e) {
                   const std::int32_t index = c.InInt(2);
                   ScriptArray&       a     = MutableArray(v, e);
                   const bool         valid = index >= 0 && static_cast<std::size_t>(index) < a.items.size();
                   if (valid)
                       a.items.erase(a.items.begin() + index);
                   c.Out(3, valid);
               });
    arrayWrite("Array.Remove", "Remove Item", [](PinType e) { return std::vector<PinInfo>{In("Item", e), Out("Removed", P::Bool)}; },
               [](ScriptContext& c, ScriptValue& v, PinType e) {
                   const ScriptValue item = c.In(2);
                   ScriptArray&      a    = MutableArray(v, e);
                   c.Out(3, std::erase_if(a.items, [&](const ScriptValue& x) { return ValuesEqual(x, item); }) > 0);
               }, "Removes every element equal to Item");
    arrayWrite("Array.Clear", "Clear", [](PinType) { return std::vector<PinInfo>{}; },
               [](ScriptContext&, ScriptValue& v, PinType e) { MutableArray(v, e).items.clear(); });
    {
        NodeDesc d = Flow("Flow.ForEach", "For Each Loop", {}, [](ScriptContext& c, int entry) {
            ScriptContext::NodeState& s     = c.State();
            std::int32_t              index = 0;
            if (entry == kScriptResume)
                index = c.ResumeData();
            else
                s.value = c.In(1); // iterates the array as it was when the loop started
            const ScriptArray& a = ArrayItems(s.value);
            if (index < 0 || static_cast<std::size_t>(index) >= a.items.size()) {
                s.value = false;
                return 5;
            }
            c.Out(3, a.items[static_cast<std::size_t>(index)]);
            c.Out(4, index);
            c.PushContinuation(index + 1);
            return 2;
        }, "Runs Loop Body for every element, then Completed");
        d.resolvePins = [](const ScriptGraph&, const ScriptNode& n) {
            const PinType e = ElementParam(n.param);
            return std::vector<PinInfo>{ExecIn(), In("Array", ArrayOf(e)), ExecOut("Loop Body"), Out("Element", e),
                                        Out("Index", P::Int), ExecOut("Completed")};
        };
        d.inference = ParamInference::ElementType;
        add(WithParam(std::move(d), ParamKind::ElementType, "Element", "float"));
    }

    // Layout helpers.
    {
        NodeDesc d = Pure("Utility.Reroute", "Reroute", "Utility", {}, [](ScriptContext& c) { c.Out(1, c.In(0)); },
                          "Passes a value through (to route links)");
        d.resolvePins = [](const ScriptGraph&, const ScriptNode& n) {
            const PinType t = ValueParam(n.param);
            return std::vector<PinInfo>{In("In", t), Out("Out", t)};
        };
        d.inference = ParamInference::PinType;
        add(WithParam(std::move(d), ParamKind::PinType, "Type", "float"));
        NodeDesc e = Flow("Utility.RerouteExec", "Reroute (exec)", {ExecIn(), ExecOut("Out")},
                          [](ScriptContext&, int) { return 1; }, "Passes the execution through (to route links)");
        e.category = "Utility";
        add(std::move(e));
    }

    // Timers: fire a Custom Event of this script.
    add(WithDefaults(Action("Timer.Set", "Set Timer by Event", "Utilities",
                            {In("Event", P::String), In("Time", P::Float), In("Looping", P::Bool), Out("Handle", P::Int)},
                            [](ScriptContext& c) { c.Out(5, c.SetTimer(c.InString(2), c.InFloat(3), c.InBool(4))); },
                            "Runs the Custom Event 'Event' after Time seconds (repeatedly when Looping)"),
                     {{"Event", std::string("MyEvent")}, {"Time", 1.0f}}));
    add(Action("Timer.Clear", "Clear Timer", "Utilities", {In("Handle", P::Int)},
               [](ScriptContext& c) { c.ClearTimer(c.InInt(2)); }));
    add(Pure("Timer.Remaining", "Get Timer Remaining", "Utilities",
             {In("Handle", P::Int), Out("Seconds", P::Float), Out("Active", P::Bool)}, [](ScriptContext& c) {
                 const float remaining = c.TimerRemaining(c.InInt(0));
                 c.Out(1, std::max(remaining, 0.0f));
                 c.Out(2, remaining >= 0.0f);
             }));

    // Mouse (positions in game-view pixels, origin top left).
    const std::vector<std::string> buttons{"Left", "Right", "Middle"};
    add(WithParam(Event("Event.MouseButtonPressed", "On Mouse Button Pressed", {Out("Position", P::Vec3)},
                        "The button went down over the game view"),
                  ParamKind::Choice, "Button", "Left", buttons));
    add(WithParam(Event("Event.MouseButtonReleased", "On Mouse Button Released", {Out("Position", P::Vec3)},
                        "The button went up"),
                  ParamKind::Choice, "Button", "Left", buttons));
    add(Pure("Input.MousePosition", "Get Mouse Position", "Input", {Out("Position", P::Vec3), Out("In View", P::Bool)},
             [](ScriptContext& c) {
                 const Input*         in = c.GetInput();
                 const ScriptViewport vp = c.Viewport();
                 const glm::vec2      p  = in ? in->MousePosition() - vp.origin : glm::vec2(-1.0f);
                 c.Out(0, glm::vec3(p, 0.0f));
                 c.Out(1, in && p.x >= 0.0f && p.y >= 0.0f && p.x < vp.size.x && p.y < vp.size.y);
             }, "x, y in pixels (z = 0)"));
    add(Pure("Input.MouseDelta", "Get Mouse Delta", "Input", {Out("Delta", P::Vec3)}, [](ScriptContext& c) {
        const Input* in = c.GetInput();
        c.Out(0, in ? glm::vec3(in->MouseDelta(), 0.0f) : glm::vec3(0.0f));
    }, "Movement since the last frame in pixels"));
    add(WithParam(Pure("Input.IsMouseButtonDown", "Is Mouse Button Down", "Input", {Out("Down", P::Bool)},
                       [](ScriptContext& c) {
                           const Input* in = c.GetInput();
                           c.Out(0, in && in->IsMouseDown(MouseButtonFromName(c.Param())));
                       }),
                  ParamKind::Choice, "Button", "Left", buttons));

    // Cameras (Camera pins: unconnected = the scene's primary camera).
    add(Pure("Camera.GetPrimary", "Get Active Camera", "Camera", {Out("Camera", P::Entity), Out("Found", P::Bool)},
             [](ScriptContext& c) {
                 const Entity e = c.GetScene().FindPrimaryCamera();
                 c.Out(0, e);
                 c.Out(1, e != NullEntity);
             }, "The camera the game renders through"));
    add(Action("Camera.SetPrimary", "Set Active Camera", "Camera", {In("Camera", P::Entity)}, [](ScriptContext& c) {
        const auto e = Target(c, 2);
        Registry&  r = c.GetScene().GetRegistry();
        if (!e || !r.Has<CameraComponent>(*e)) {
            if (e)
                c.Error("Target has no Camera");
            return;
        }
        r.ViewOf<CameraComponent>().Each([&](Entity other, CameraComponent& cam) { cam.primary = other == *e; });
    }, "Makes the camera primary (the others not)"));
    add(WithDefaults(Action("Camera.SetFov", "Set Field of View", "Camera", {In("Camera", P::Entity), In("Degrees", P::Float)},
                            [](ScriptContext& c) {
                                const Entity e = CameraOf(c, 2);
                                if (CameraComponent* cam = Alive(c, e) ? c.GetScene().GetRegistry().TryGet<CameraComponent>(e) : nullptr)
                                    cam->fovY = glm::radians(std::clamp(c.InFloat(3), 1.0f, 170.0f));
                                else
                                    c.Error("No camera");
                            }, "Vertical field of view"),
                     {{"Degrees", 60.0f}}));
    const auto cameraRay = [](ScriptContext& c, Entity camera, glm::vec2 pixel, glm::vec3& origin, glm::vec3& dir) {
        const CameraComponent* cam = Alive(c, camera) ? c.GetScene().GetRegistry().TryGet<CameraComponent>(camera) : nullptr;
        if (!cam)
            return false;
        const glm::mat4      world  = World(c, camera);
        const ScriptViewport vp     = c.Viewport();
        const float          aspect = vp.size.x / std::max(vp.size.y, 1.0f);
        const float          t      = std::tan(cam->fovY * 0.5f);
        const glm::vec2      ndc(pixel.x / std::max(vp.size.x, 1.0f) * 2.0f - 1.0f, 1.0f - pixel.y / std::max(vp.size.y, 1.0f) * 2.0f);
        const glm::vec3      local(ndc.x * t * aspect, ndc.y * t, -1.0f);
        origin = glm::vec3(world[3]);
        dir    = glm::normalize(glm::mat3(RotationOf(world)) * local);
        return true;
    };
    add(Pure("Camera.ScreenToWorld", "Screen to World Ray", "Camera",
             {In("Screen Position", P::Vec3), In("Camera", P::Entity), Out("Origin", P::Vec3), Out("Direction", P::Vec3),
              Out("Valid", P::Bool)},
             [cameraRay](ScriptContext& c) {
                 glm::vec3  origin(0.0f), dir(0.0f, 0.0f, -1.0f);
                 const bool ok = cameraRay(c, CameraOf(c, 1), glm::vec2(c.InVec3(0)), origin, dir);
                 c.Out(2, origin);
                 c.Out(3, dir);
                 c.Out(4, ok);
             }, "Ray through a game-view pixel (e.g. the mouse) for a Raycast"));
    add(Pure("Camera.WorldToScreen", "World to Screen", "Camera",
             {In("Location", P::Vec3), In("Camera", P::Entity), Out("Screen Position", P::Vec3), Out("On Screen", P::Bool)},
             [](ScriptContext& c) {
                 const Entity           camera = CameraOf(c, 1);
                 const CameraComponent* cam    = Alive(c, camera) ? c.GetScene().GetRegistry().TryGet<CameraComponent>(camera) : nullptr;
                 if (!cam) {
                     c.Out(2, glm::vec3(0.0f));
                     c.Out(3, false);
                     return;
                 }
                 const glm::mat4      world = World(c, camera);
                 const ScriptViewport vp    = c.Viewport();
                 const glm::vec3      local = glm::inverse(glm::mat3(RotationOf(world))) * (c.InVec3(0) - glm::vec3(world[3]));
                 const float          t     = std::tan(cam->fovY * 0.5f);
                 const float          aspect = vp.size.x / std::max(vp.size.y, 1.0f);
                 if (local.z >= -1e-4f) { // behind the camera
                     c.Out(2, glm::vec3(-1.0f));
                     c.Out(3, false);
                     return;
                 }
                 const glm::vec2 ndc(local.x / (-local.z * t * aspect), local.y / (-local.z * t));
                 const glm::vec2 px((ndc.x * 0.5f + 0.5f) * vp.size.x, (0.5f - ndc.y * 0.5f) * vp.size.y);
                 c.Out(2, glm::vec3(px, 0.0f));
                 c.Out(3, std::abs(ndc.x) <= 1.0f && std::abs(ndc.y) <= 1.0f);
             }, "Game-view pixel of a world location"));

    // Tags, names, hierarchy, components.
    add(Pure("Entity.HasTag", "Has Tag", "Entity", {In("Target", P::Entity), In("Tag", P::String), Out("Result", P::Bool)},
             [](ScriptContext& c) {
                 const Entity e    = c.InEntity(0);
                 const Tags*  tags = Alive(c, e) ? c.GetScene().GetRegistry().TryGet<Tags>(e) : nullptr;
                 c.Out(2, tags && tags->Has(c.InString(1)));
             }));
    add(Action("Entity.AddTag", "Add Tag", "Entity", {In("Target", P::Entity), In("Tag", P::String)}, [](ScriptContext& c) {
        if (const auto e = Target(c, 2)) {
            const std::string tag  = c.InString(3);
            Registry&         r    = c.GetScene().GetRegistry();
            Tags&             tags = r.Has<Tags>(*e) ? r.Get<Tags>(*e) : r.Emplace<Tags>(*e);
            if (!tag.empty() && !tags.Has(tag))
                tags.values.push_back(tag);
        }
    }));
    add(Action("Entity.RemoveTag", "Remove Tag", "Entity", {In("Target", P::Entity), In("Tag", P::String)}, [](ScriptContext& c) {
        if (const auto e = Target(c, 2))
            if (Tags* tags = c.GetScene().GetRegistry().TryGet<Tags>(*e))
                std::erase(tags->values, c.InString(3));
    }));
    add(Pure("Entity.GetAllWithTag", "Get All Entities with Tag", "Entity", {In("Tag", P::String), Out("Entities", P::EntityArray)},
             [](ScriptContext& c) {
                 const std::string        tag = c.InString(0);
                 std::vector<ScriptValue> found;
                 c.GetScene().GetRegistry().ViewOf<Tags>().Each([&](Entity e, Tags& tags) {
                     if (tags.Has(tag))
                         found.emplace_back(e);
                 });
                 std::ranges::sort(found, {}, [](const ScriptValue& v) { return static_cast<std::uint64_t>(std::get<Entity>(v)); });
                 c.Out(1, MakeArray(P::Entity, std::move(found)));
             }));
    add(Pure("Entity.FindWithTag", "Find Entity with Tag", "Entity",
             {In("Tag", P::String), Out("Entity", P::Entity), Out("Found", P::Bool)}, [](ScriptContext& c) {
                 const std::string tag   = c.InString(0);
                 Entity            found = NullEntity;
                 c.GetScene().GetRegistry().ViewOf<Tags>().Each([&](Entity e, Tags& tags) {
                     if (tags.Has(tag) && (found == NullEntity || static_cast<std::uint64_t>(e) < static_cast<std::uint64_t>(found)))
                         found = e;
                 });
                 c.Out(1, found);
                 c.Out(2, found != NullEntity);
             }));
    add(Action("Entity.SetName", "Set Name", "Entity", {In("Target", P::Entity), In("Name", P::String)}, [](ScriptContext& c) {
        if (const auto e = Target(c, 2))
            c.GetScene().GetRegistry().Get<Name>(*e).value = c.InString(3);
    }));
    add(Pure("Entity.GetParent", "Get Parent", "Entity", {In("Target", P::Entity), Out("Parent", P::Entity), Out("Has Parent", P::Bool)},
             [](ScriptContext& c) {
                 const Entity e      = c.InEntity(0);
                 const Entity parent = Alive(c, e) ? c.GetScene().GetRegistry().Get<Hierarchy>(e).parent : NullEntity;
                 c.Out(1, parent);
                 c.Out(2, parent != NullEntity);
             }));
    add(Pure("Entity.GetChildren", "Get Children", "Entity", {In("Target", P::Entity), Out("Children", P::EntityArray)},
             [](ScriptContext& c) {
                 const Entity             e = c.InEntity(0);
                 std::vector<ScriptValue> children;
                 if (Alive(c, e))
                     for (Entity child : c.GetScene().GetRegistry().Get<Hierarchy>(e).children)
                         children.emplace_back(child);
                 c.Out(1, MakeArray(P::Entity, std::move(children)));
             }));
    add(WithParam(Pure("Entity.HasComponent", "Has Component", "Entity", {In("Target", P::Entity), Out("Result", P::Bool)},
                       [](ScriptContext& c) {
                           const Entity    e = c.InEntity(0);
                           const Registry& r = c.GetScene().GetRegistry();
                           bool            has = false;
                           if (Alive(c, e)) {
                               const std::string& kind = c.Param();
                               has = kind == "Mesh"        ? r.Has<MeshRenderer>(e)
                                     : kind == "Light"     ? r.Has<Light>(e)
                                     : kind == "RigidBody" ? r.Has<RigidBody>(e)
                                     : kind == "Collider"  ? r.Has<Collider>(e)
                                     : kind == "Character" ? r.Has<CharacterController>(e)
                                     : kind == "Camera"    ? r.Has<CameraComponent>(e)
                                     : kind == "AudioSource" ? r.Has<AudioSource>(e)
                                     : kind == "Script"    ? r.Has<ScriptComponent>(e)
                                     : kind == "Tags"      ? r.Has<Tags>(e)
                                                           : false;
                           }
                           c.Out(1, has);
                       }),
                  ParamKind::Choice, "Component", "Mesh",
                  {"Mesh", "Light", "RigidBody", "Collider", "Character", "Camera", "AudioSource", "Script", "Tags"}));
    add(Pure("Entity.GetRight", "Get Right Vector", "Transform", {In("Target", P::Entity), Out("Right", P::Vec3)},
             [](ScriptContext& c) {
                 if (const auto e = Target(c, 0))
                     c.Out(1, glm::normalize(glm::vec3(World(c, *e)[0])));
             }, "World +X of the entity"));
    add(Pure("Entity.GetUp", "Get Up Vector", "Transform", {In("Target", P::Entity), Out("Up", P::Vec3)}, [](ScriptContext& c) {
        if (const auto e = Target(c, 0))
            c.Out(1, glm::normalize(glm::vec3(World(c, *e)[1])));
    }, "World +Y of the entity"));

    // Other entities' scripts (variables by name, custom events).
    {
        NodeDesc get = Pure("Script.GetVariable", "Get Script Variable", "Script",  {}, [](ScriptContext& c) {
            const PinType      t = ValueParam(c.Param());
            const ScriptValue* v = c.InstanceVariable(c.InEntity(0), c.InString(1));
            c.Out(2, v && CanConvert(TypeOf(*v), t) ? Convert(*v, t) : DefaultValue(t));
            c.Out(3, v != nullptr);
        }, "A variable of another entity's script (by name)");
        get.resolvePins = [](const ScriptGraph&, const ScriptNode& n) {
            return std::vector<PinInfo>{In("Target", P::Entity), In("Name", P::String), Out("Value", ValueParam(n.param)),
                                        Out("Found", P::Bool)};
        };
        add(WithParam(std::move(get), ParamKind::PinType, "Type", "float"));
        NodeDesc set = Flow("Script.SetVariable", "Set Script Variable", {}, [](ScriptContext& c, int) {
            ScriptValue* v = c.InstanceVariable(c.InEntity(2), c.InString(3));
            const bool   ok = v && CanConvert(ValueParam(c.Param()), TypeOf(*v));
            if (ok)
                *v = Convert(c.In(4), TypeOf(*v));
            c.Out(5, ok);
            return 1;
        }, "Sets a variable of another entity's script (by name)");
        set.category    = "Script";
        set.resolvePins = [](const ScriptGraph&, const ScriptNode& n) {
            return std::vector<PinInfo>{ExecIn(), ExecOut(), In("Target", P::Entity), In("Name", P::String),
                                        In("Value", ValueParam(n.param)), Out("Success", P::Bool)};
        };
        add(WithParam(std::move(set), ParamKind::PinType, "Type", "float"));
    }
    add(WithDefaults(Action("Script.CallEvent", "Call Event on Entity", "Script",
                            {In("Target", P::Entity), In("Event", P::String), Out("Called", P::Bool)},
                            [](ScriptContext& c) { c.Out(4, c.CallEventOn(c.InEntity(2), c.InString(3))); },
                            "Runs the Custom Event of another entity's script now"),
                     {{"Event", std::string("MyEvent")}}));

    // Structs (ScriptRegistry definitions).
    {
        NodeDesc d = Pure("Struct.Make", "Make Struct", "Struct", {}, [](ScriptContext& c) {
            const std::string&     name  = c.Param();
            ScriptValue            value = MakeStruct(name);
            const ScriptStructDef* def   = ScriptRegistry::FindStruct(name);
            const std::size_t      count = def ? def->fields.size() : 0;
            for (std::size_t i = 0; i < count; ++i)
                SetStructField(value, name, def->fields[i].name, c.In(static_cast<int>(i)));
            c.Out(static_cast<int>(count), std::move(value));
        }, "A struct from its field values");
        d.resolvePins = [](const ScriptGraph&, const ScriptNode& n) {
            std::vector<PinInfo> pins;
            if (const ScriptStructDef* def = ScriptRegistry::FindStruct(n.param))
                for (const ScriptStructField& f : def->fields)
                    pins.push_back(In(f.name, f.type));
            pins.push_back(Out("Struct", PinType::Struct(n.param)));
            return pins;
        };
        d.pinDefault = [](const ScriptNode& n, const PinInfo& pin) -> std::optional<ScriptValue> {
            if (const ScriptStructDef* def = ScriptRegistry::FindStruct(n.param))
                for (const ScriptStructField& f : def->fields)
                    if (f.name == pin.name)
                        return f.value;
            return std::nullopt;
        };
        d.inference = ParamInference::UserType;
        add(WithParam(std::move(d), ParamKind::StructType, "Struct", ""));

        NodeDesc b = Pure("Struct.Break", "Break Struct", "Struct", {}, [](ScriptContext& c) {
            const ScriptValue      value = c.In(0);
            const ScriptStructDef* def   = ScriptRegistry::FindStruct(c.Param());
            for (std::size_t i = 0; def && i < def->fields.size(); ++i) {
                const ScriptValue* field = StructField(value, def->fields[i].name);
                c.Out(static_cast<int>(i) + 1, field ? Convert(*field, def->fields[i].type) : def->fields[i].value);
            }
        }, "The field values of a struct");
        b.resolvePins = [](const ScriptGraph&, const ScriptNode& n) {
            std::vector<PinInfo> pins{In("Struct", PinType::Struct(n.param))};
            if (const ScriptStructDef* def = ScriptRegistry::FindStruct(n.param))
                for (const ScriptStructField& f : def->fields)
                    pins.push_back(Out(f.name, f.type));
            return pins;
        };
        b.inference = ParamInference::UserType;
        add(WithParam(std::move(b), ParamKind::StructType, "Struct", ""));

        NodeDesc f = Pure("Struct.SetField", "Set Struct Field", "Struct", {}, [](ScriptContext& c) {
            const auto [name, field] = SplitDotted(c.Param());
            ScriptValue value        = c.In(0);
            SetStructField(value, name, field, c.In(1));
            c.Out(2, std::move(value));
        }, "A copy of the struct with one field changed");
        f.resolvePins = [](const ScriptGraph&, const ScriptNode& n) {
            const auto [name, field] = SplitDotted(n.param);
            PinType type             = PinType::Float;
            if (const ScriptStructDef* def = ScriptRegistry::FindStruct(name))
                for (const ScriptStructField& x : def->fields)
                    if (x.name == field)
                        type = x.type;
            return std::vector<PinInfo>{In("Struct", PinType::Struct(name)), In("Value", type), Out("Struct", PinType::Struct(name))};
        };
        add(WithParam(std::move(f), ParamKind::StructField, "Field", ""));
    }

    // Enums.
    {
        NodeDesc lit = Pure("Enum.Literal", "Enum Value", "Enum", {}, [](ScriptContext& c) {
            const auto [name, value] = SplitDotted(c.Param());
            c.Out(0, std::max(ScriptRegistry::EnumValueIndex(name, value), 0));
        }, "A constant enum value");
        lit.resolvePins = [](const ScriptGraph&, const ScriptNode& n) {
            return std::vector<PinInfo>{Out("Value", PinType::Enum(SplitDotted(n.param).first))};
        };
        add(WithParam(std::move(lit), ParamKind::EnumValue, "Value", ""));

        NodeDesc sw = Flow("Enum.Switch", "Switch on Enum", {}, [](ScriptContext& c, int) {
            const ScriptEnum*  e     = ScriptRegistry::FindEnum(c.Param());
            const std::int32_t value = c.InInt(1);
            const std::int32_t count = e ? static_cast<std::int32_t>(e->values.size()) : 0;
            return 2 + (value >= 0 && value < count ? value : count);
        }, "Continues at the output of the value (Default for unknown values)");
        sw.category    = "Enum";
        sw.resolvePins = [](const ScriptGraph&, const ScriptNode& n) {
            std::vector<PinInfo> pins{ExecIn(), In("Selection", PinType::Enum(n.param))};
            if (const ScriptEnum* e = ScriptRegistry::FindEnum(n.param))
                for (const std::string& v : e->values)
                    pins.push_back(ExecOut(v));
            pins.push_back(ExecOut("Default"));
            return pins;
        };
        sw.inference = ParamInference::UserType;
        add(WithParam(std::move(sw), ParamKind::EnumType, "Enum", ""));

        NodeDesc name = Pure("Enum.ToName", "Enum to Name", "Enum", {}, [](ScriptContext& c) {
            c.Out(1, ScriptRegistry::EnumValueName(c.Param(), c.InInt(0)));
        });
        name.resolvePins = [](const ScriptGraph&, const ScriptNode& n) {
            return std::vector<PinInfo>{In("Value", PinType::Enum(n.param)), Out("Name", P::String)};
        };
        name.inference = ParamInference::UserType;
        add(WithParam(std::move(name), ParamKind::EnumType, "Enum", ""));

        NodeDesc from = Pure("Enum.FromName", "Name to Enum", "Enum", {}, [](ScriptContext& c) {
            const std::int32_t index = ScriptRegistry::EnumValueIndex(c.Param(), c.InString(0));
            c.Out(1, std::max(index, 0));
            c.Out(2, index >= 0);
        });
        from.resolvePins = [](const ScriptGraph&, const ScriptNode& n) {
            return std::vector<PinInfo>{In("Name", P::String), Out("Value", PinType::Enum(n.param)), Out("Found", P::Bool)};
        };
        add(WithParam(std::move(from), ParamKind::EnumType, "Enum", ""));

        NodeDesc count = Pure("Enum.Count", "Enum Value Count", "Enum", {Out("Count", P::Int)}, [](ScriptContext& c) {
            const ScriptEnum* e = ScriptRegistry::FindEnum(c.Param());
            c.Out(0, e ? static_cast<std::int32_t>(e->values.size()) : 0);
        });
        add(WithParam(std::move(count), ParamKind::EnumType, "Enum", ""));
    }

    // Maps.
    const auto mapRead = [&](const char* type, const char* title, auto pins, auto eval, const char* tooltip = "") {
        NodeDesc d = Pure(type, title, "Map", {}, [eval](ScriptContext& c) {
            const PinType     t     = MapParam(c.Param());
            const ScriptValue value = c.In(0);
            eval(c, MapOf(value), t);
        }, tooltip);
        d.resolvePins = [pins](const ScriptGraph&, const ScriptNode& n) { return pins(MapParam(n.param)); };
        d.inference   = ParamInference::MapType;
        add(WithParam(std::move(d), ParamKind::PinType, "Map type", "map:string:int"));
    };
    mapRead("Map.Find", "Find",
            [](PinType t) {
                return std::vector<PinInfo>{In("Map", t), In("Key", KeyType(t)), Out("Value", ElementType(t)), Out("Found", P::Bool)};
            },
            [](ScriptContext& c, const ScriptMap& m, PinType t) {
                const auto it = m.items.find(c.In(1));
                c.Out(2, it != m.items.end() ? it->second : DefaultValue(ElementType(t)));
                c.Out(3, it != m.items.end());
            }, "The value stored for Key");
    mapRead("Map.Contains", "Contains Key",
            [](PinType t) { return std::vector<PinInfo>{In("Map", t), In("Key", KeyType(t)), Out("Result", P::Bool)}; },
            [](ScriptContext& c, const ScriptMap& m, PinType) { c.Out(2, m.items.contains(c.In(1))); });
    mapRead("Map.Length", "Length (map)", [](PinType t) { return std::vector<PinInfo>{In("Map", t), Out("Length", P::Int)}; },
            [](ScriptContext& c, const ScriptMap& m, PinType) { c.Out(1, static_cast<std::int32_t>(m.items.size())); });
    mapRead("Map.Keys", "Keys", [](PinType t) { return std::vector<PinInfo>{In("Map", t), Out("Keys", ArrayOf(KeyType(t)))}; },
            [](ScriptContext& c, const ScriptMap& m, PinType t) {
                std::vector<ScriptValue> keys;
                for (const auto& [k, v] : m.items)
                    keys.push_back(k);
                c.Out(1, MakeArray(KeyType(t), std::move(keys)));
            }, "All keys (sorted)");
    mapRead("Map.Values", "Values", [](PinType t) { return std::vector<PinInfo>{In("Map", t), Out("Values", ArrayOf(ElementType(t)))}; },
            [](ScriptContext& c, const ScriptMap& m, PinType t) {
                std::vector<ScriptValue> values;
                for (const auto& [k, v] : m.items)
                    values.push_back(v);
                c.Out(1, MakeArray(ElementType(t), std::move(values)));
            }, "All values (in key order)");
    const auto mapWrite = [&](const char* type, const char* title, auto data, auto run, const char* tooltip = "") {
        NodeDesc d = Flow(type, title, {}, [run](ScriptContext& c, int) {
            ScriptValue* v = c.Variable(c.Param());
            if (!v || !IsMap(TypeOf(*v))) {
                c.Error("'" + c.Param() + "' is not a map variable");
                return 1;
            }
            run(c, *v, TypeOf(*v)); // reads its inputs before changing the map
            return 1;
        }, tooltip);
        d.category    = "Map";
        d.mapVariable = true;
        d.resolvePins = [data](const ScriptGraph& g, const ScriptNode& n) {
            const PinType        t = VariableType(g, n);
            std::vector<PinInfo> pins{ExecIn(), ExecOut()};
            const std::vector<PinInfo> extra = data(IsMap(t) ? t : PinType::Map(PinType::String, PinType::Int));
            pins.insert(pins.end(), extra.begin(), extra.end());
            return pins;
        };
        add(WithParam(std::move(d), ParamKind::Variable, "Map", ""));
    };
    mapWrite("Map.Add", "Add (map)", [](PinType t) { return std::vector<PinInfo>{In("Key", KeyType(t)), In("Value", ElementType(t))}; },
             [](ScriptContext& c, ScriptValue& v, PinType t) {
                 ScriptValue key = c.In(2), value = c.In(3);
                 MutableMap(v, KeyType(t), ElementType(t)).items[std::move(key)] = std::move(value);
             }, "Stores Value for Key (replaces an existing one)");
    mapWrite("Map.Remove", "Remove (map)", [](PinType t) { return std::vector<PinInfo>{In("Key", KeyType(t)), Out("Removed", P::Bool)}; },
             [](ScriptContext& c, ScriptValue& v, PinType t) {
                 const ScriptValue key = c.In(2);
                 c.Out(3, MutableMap(v, KeyType(t), ElementType(t)).items.erase(key) > 0);
             });
    mapWrite("Map.Clear", "Clear (map)", [](PinType) { return std::vector<PinInfo>{}; },
             [](ScriptContext&, ScriptValue& v, PinType t) { MutableMap(v, KeyType(t), ElementType(t)).items.clear(); });

    // Switches, select, multi gate, retriggerable delay.
    for (const bool strings : {false, true}) {
        NodeDesc d = Flow(strings ? "Flow.SwitchString" : "Flow.SwitchInt", strings ? "Switch on String" : "Switch on Int", {},
                          [strings](ScriptContext& c, int) {
                              const std::vector<std::string> cases = SplitCases(c.Param());
                              for (std::size_t i = 0; i < cases.size(); ++i)
                                  if (strings ? c.InString(1) == cases[i] : c.InInt(1) == std::atoi(cases[i].c_str()))
                                      return 2 + static_cast<int>(i);
                              return 2 + static_cast<int>(cases.size()); // Default
                          },
                          "Continues at the matching case, else at Default");
        d.resolvePins = [strings](const ScriptGraph&, const ScriptNode& n) {
            std::vector<PinInfo> pins{ExecIn(), In("Selection", strings ? P::String : P::Int)};
            for (const std::string& c : SplitCases(n.param))
                pins.push_back(ExecOut(c));
            pins.push_back(ExecOut("Default"));
            return pins;
        };
        add(WithParam(std::move(d), ParamKind::Cases, "Cases", strings ? "A, B, C" : "0, 1, 2"));
    }
    {
        const auto count = [](const std::string& param) { return std::clamp(SplitTypeAndCount(param, 2).second, 2, 16); };
        NodeDesc   d     = Pure("Utility.Select", "Select", "Utility", {}, [count](ScriptContext& c) {
            const int n     = count(c.Param());
            const int index = c.InInt(0);
            c.Out(n + 1, index >= 0 && index < n ? c.In(1 + index) : DefaultValue(ElementParam(c.Param())));
        }, "The option at Index (only that one is evaluated; bools pick 0 / 1)");
        d.resolvePins = [count](const ScriptGraph&, const ScriptNode& n) {
            const PinType        t = ElementParam(n.param);
            std::vector<PinInfo> pins{In("Index", P::Int)};
            for (int i = 0, c = count(n.param); i < c; ++i)
                pins.push_back(In("Option " + std::to_string(i), t));
            pins.push_back(Out("Result", t));
            return pins;
        };
        d.inference = ParamInference::PinType;
        d.infers    = [](std::string_view pin) { return pin != "Index"; };
        add(WithParam(std::move(d), ParamKind::TypeAndCount, "Type : options", "float:2"));
    }
    {
        NodeDesc d = Flow("Flow.MultiGate", "Multi Gate", {}, [](ScriptContext& c, int entry) {
            ScriptContext::NodeState& s = c.State();
            const int                 n = static_cast<int>(CountParam(c.Param(), 2, 16));
            if (entry == 1) { // Reset
                s.counter = 0;
                s.flag2   = false;
                return kScriptStop;
            }
            const std::int32_t all = (1 << n) - 1;
            if ((s.counter & all) == all) {
                if (!c.InBool(3)) // done until Reset
                    return kScriptStop;
                s.counter = 0;
            }
            if (!s.flag2) { // first run: the start index
                s.flag2 = true;
                s.value = std::clamp(c.InInt(4), 0, n - 1);
            }
            int pick = -1;
            if (c.InBool(2)) { // random among the unused outputs
                std::vector<int> unused;
                for (int i = 0; i < n; ++i)
                    if (!(s.counter & (1 << i)))
                        unused.push_back(i);
                pick = unused[std::uniform_int_distribution<std::size_t>(0, unused.size() - 1)(Rng())];
            } else {
                const int start = std::get<std::int32_t>(Convert(s.value, P::Int));
                for (int k = 0; k < n && pick < 0; ++k)
                    if (!(s.counter & (1 << ((start + k) % n))))
                        pick = (start + k) % n;
                s.value = (pick + 1) % n;
            }
            s.counter |= 1 << pick;
            return 5 + pick;
        }, "Each run fires the next unused output (or a random one); Loop starts over when all were used");
        d.resolvePins = [](const ScriptGraph&, const ScriptNode& node) {
            std::vector<PinInfo> pins{ExecIn(), ExecIn("Reset"), In("Is Random", P::Bool), In("Loop", P::Bool), In("Start Index", P::Int)};
            for (std::uint32_t i = 0, n = CountParam(node.param, 2, 16); i < n; ++i)
                pins.push_back(ExecOut("Out " + std::to_string(i)));
            return pins;
        };
        add(WithParam(std::move(d), ParamKind::Count, "Outputs", "3"));
    }
    add(WithDefaults(Flow("Flow.RetriggerableDelay", "Retriggerable Delay",
                          {ExecIn(), In("Duration", P::Float), ExecOut("Completed")},
                          [](ScriptContext& c, int entry) {
                              ScriptContext::NodeState& s = c.State();
                              if (entry == kScriptResume) // only the latest trigger completes
                                  return c.ResumeData() == s.counter ? 2 : kScriptStop;
                              ++s.counter;
                              c.Suspend(std::max(c.InFloat(1), 0.0f), s.counter);
                              return kScriptStop;
                          },
                          "Continues Duration seconds after the last trigger (each trigger restarts the countdown)"),
                     {{"Duration", 1.0f}}));
    r.back().latent = true;
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
    if (desc) {
        for (const auto& [name, value] : desc->defaults)
            if (name == pin.name)
                return Convert(value, pin.type);
        if (desc->pinDefault)
            if (auto value = desc->pinDefault(node, pin))
                return Convert(*value, pin.type);
    }
    return DefaultValue(pin.type);
}

const NodeDesc* FindScriptNodeType(std::string_view type)
{
    static const std::unordered_map<std::string_view, const NodeDesc*> index = [] {
        std::unordered_map<std::string_view, const NodeDesc*> map;
        for (const NodeDesc& d : ScriptNodeTypes())
            map.emplace(d.type, &d);
        return map;
    }();
    const auto it = index.find(type);
    return it != index.end() ? it->second : nullptr;
}

std::vector<std::string> ElementTypeNames()
{
    std::vector<std::string> names{"bool", "int", "float", "vec3", "string", "entity"};
    for (const std::string& e : ScriptRegistry::EnumNames())
        names.push_back("enum:" + e);
    for (const std::string& s : ScriptRegistry::StructNames())
        names.push_back("struct:" + s);
    return names;
}

std::pair<std::string, int> SplitTypeAndCount(std::string_view param, int fallback)
{
    const std::size_t colon = param.rfind(':');
    if (colon != std::string_view::npos && colon + 1 < param.size() &&
        std::all_of(param.begin() + static_cast<std::ptrdiff_t>(colon + 1), param.end(),
                    [](char ch) { return ch >= '0' && ch <= '9'; }))
        return {std::string(param.substr(0, colon)), std::atoi(std::string(param.substr(colon + 1)).c_str())};
    return {std::string(param), fallback};
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
