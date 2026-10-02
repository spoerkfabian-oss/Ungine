#include "Engine/Script/ScriptSystem.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Input.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptCondition.h"
#include "Engine/Script/ScriptNodes.h"
#include "Engine/Script/ScriptRegistry.h"
#include "ScriptExpand.h"
#include "ScriptJson.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <deque>
#include <fstream>
#include <functional>
#include <unordered_set>

namespace Engine {

namespace {

std::uint64_t EntityKey(Entity e) { return static_cast<std::uint64_t>(e); }

// Save games: like ScriptValueToJson, but entities (also inside containers / structs) are stored
// as their UUID and found again by it when loaded (in the same level).
nlohmann::json SaveValueToJson(const ScriptValue& value, const Registry& r)
{
    return std::visit(
        [&](const auto& v) -> nlohmann::json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Entity>) {
                const Uuid* uuid = v != NullEntity && r.Valid(v) ? r.TryGet<Uuid>(v) : nullptr;
                return uuid ? nlohmann::json{{"$uuid", uuid->value}} : nlohmann::json();
            } else if constexpr (std::is_same_v<T, ScriptArrayPtr>) {
                nlohmann::json list = nlohmann::json::array();
                for (const ScriptValue& item : ArrayItems(value).items)
                    list.push_back(SaveValueToJson(item, r));
                return list;
            } else if constexpr (std::is_same_v<T, ScriptStructPtr>) {
                nlohmann::json object = nlohmann::json::object();
                for (const auto& [name, field] : StructOf(value).fields)
                    object[name] = SaveValueToJson(field, r);
                return object;
            } else if constexpr (std::is_same_v<T, ScriptMapPtr>) {
                nlohmann::json pairs = nlohmann::json::array();
                for (const auto& [key, item] : MapOf(value).items)
                    pairs.push_back({SaveValueToJson(key, r), SaveValueToJson(item, r)});
                return pairs;
            } else {
                return ScriptValueToJson(value);
            }
        },
        value);
}

ScriptValue SaveValueFromJson(const nlohmann::json& j, PinType type, Scene& scene)
{
    if (IsArray(type)) {
        std::vector<ScriptValue> items;
        if (j.is_array())
            for (const nlohmann::json& item : j)
                items.push_back(SaveValueFromJson(item, ElementType(type), scene));
        return MakeArray(ElementType(type), std::move(items));
    }
    if (IsMap(type)) {
        ScriptValue value = MakeMap(KeyType(type), ElementType(type));
        if (j.is_array())
            for (const nlohmann::json& pair : j)
                if (pair.is_array() && pair.size() == 2)
                    MutableMap(value, KeyType(type), ElementType(type)).items[SaveValueFromJson(pair[0], KeyType(type), scene)] =
                        SaveValueFromJson(pair[1], ElementType(type), scene);
        return value;
    }
    if (type.kind == PinKind::Struct) {
        ScriptValue value = DefaultValue(type);
        if (const ScriptStructDef* def = ScriptRegistry::FindStruct(UserTypeName(type)); def && j.is_object())
            for (const ScriptStructField& f : def->fields)
                if (const auto it = j.find(f.name); it != j.end())
                    SetStructField(value, def->name, f.name, SaveValueFromJson(*it, f.type, scene));
        return value;
    }
    if (type.kind == PinKind::Entity)
        return j.is_object() && j.contains("$uuid") ? scene.FindByUuid(j["$uuid"].get<std::uint64_t>()) : NullEntity;
    return ScriptValueFromJson(j, type);
}

struct PinRef {
    int node = -1; // index into Program::nodes, -1: none
    int pin  = -1;
};

struct CompiledNode {
    const ScriptNode*        node = nullptr; // into Program::graph
    const NodeDesc*          desc = nullptr;
    std::vector<PinInfo>     pins;
    std::vector<PinRef>      source;   // per pin: data input <- output
    std::vector<PinRef>      target;   // per pin: exec output -> input
    std::vector<ScriptValue> defaults; // per pin: unconnected data inputs
    int                      scope        = -1; // function the node belongs to (-1: event graph)
    int                      callFunction = -1; // Call / CallPure: the called function
    std::vector<int>         argPins;    // Call: input pin of each function input
    std::vector<int>         resultPins; // Call: output pin of each function output
    // Where the node comes from (copies of macros / library functions): Program::files index, node
    // id and function there, and the Macro node of the graph itself it was copied for.
    int           file = 0;
    std::uint32_t id   = 0;
    std::string   function;
    std::uint32_t use = 0;
};

struct CompiledFunction {
    std::string                          name;
    int                                  entry = -1; // node index of the Entry node
    bool                                 pure  = false;
    std::vector<ScriptVariable>          locals;
    std::unordered_map<std::string, int> localIndex;
    std::size_t                          outputs = 0;
};

struct Program {
    std::string                             key;
    ScriptGraph                             graph;
    std::vector<CompiledNode>               nodes;
    std::vector<CompiledFunction>           functions;
    std::vector<ScriptDiagnostic>           diagnostics;
    bool                                    ok = false;
    std::unordered_map<std::string, int>    variableIndex;
    std::unordered_map<std::string, int>    functionIndex;
    std::map<std::string, std::vector<int>> events; // node type -> event nodes (event graph only)
    // Graph files of the nodes (debugger keys): 0 = this graph, then the libraries copied in.
    std::vector<std::string> files;
    std::vector<std::string> fileLibraries; // library name per file ("" for this graph)
};

// Debugger key of a library: its file (ScriptSystem::Key), or a name for libraries made in code.
std::string LibraryKey(const std::string& name)
{
    const ScriptLibrary* l = ScriptRegistry::FindLibrary(name);
    return l && !l->file.empty() ? ScriptSystem::Key(l->file) : "library:" + name;
}

int PinIndex(const CompiledNode& c, const std::string& name, bool output)
{
    for (std::size_t i = 0; i < c.pins.size(); ++i)
        if (c.pins[i].output == output && c.pins[i].name == name)
            return static_cast<int>(i);
    return -1;
}

std::shared_ptr<Program> Compile(std::string key, ScriptGraph graph, std::vector<ScriptDiagnostic> diagnostics,
                                 const std::unordered_map<std::uint32_t, ScriptOrigin>& origins)
{
    auto p         = std::make_shared<Program>();
    p->key         = std::move(key);
    p->files       = {p->key};
    p->fileLibraries = {std::string()};
    p->graph       = std::move(graph);
    p->diagnostics = std::move(diagnostics);
    p->ok = std::ranges::none_of(p->diagnostics, [](const ScriptDiagnostic& d) { return d.error; });
    if (!p->ok)
        return p;

    std::unordered_map<std::string, int>& functionIndex = p->functionIndex;
    for (const ScriptFunction& f : p->graph.functions) {
        functionIndex[f.name] = static_cast<int>(p->functions.size());
        CompiledFunction cf;
        cf.name    = f.name;
        cf.pure    = f.pure;
        cf.locals  = f.locals;
        cf.outputs = f.outputs.size();
        for (std::size_t i = 0; i < f.locals.size(); ++i)
            cf.localIndex[f.locals[i].name] = static_cast<int>(i);
        p->functions.push_back(std::move(cf));
    }

    std::unordered_map<std::uint32_t, int> index;
    for (const ScriptNode& n : p->graph.nodes) {
        index[n.id] = static_cast<int>(p->nodes.size());
        CompiledNode c;
        c.node  = &n;
        c.desc  = FindScriptNodeType(n.type);
        c.pins  = NodePins(p->graph, n);
        c.scope = n.function.empty() ? -1 : functionIndex.at(n.function);
        c.id       = n.id;
        c.function = n.function;
        if (const auto o = origins.find(n.id); o != origins.end()) {
            c.id       = o->second.node;
            c.function = o->second.function;
            c.use      = o->second.use;
            if (!o->second.library.empty()) {
                const auto f = std::ranges::find(p->fileLibraries, o->second.library);
                c.file       = static_cast<int>(f - p->fileLibraries.begin());
                if (f == p->fileLibraries.end()) {
                    p->files.push_back(LibraryKey(o->second.library));
                    p->fileLibraries.push_back(o->second.library);
                }
            }
        }
        c.source.resize(c.pins.size());
        c.target.resize(c.pins.size());
        c.defaults.resize(c.pins.size(), false);
        for (std::size_t i = 0; i < c.pins.size(); ++i)
            if (!c.pins[i].output && c.pins[i].type != PinType::Exec)
                c.defaults[i] = PinDefault(n, c.desc, c.pins[i]);
        if (n.type == "Function.Entry")
            p->functions[static_cast<std::size_t>(functionIndex.at(n.param))].entry = index[n.id];
        else if (c.desc->kind == NodeKind::Event && c.scope < 0)
            p->events[n.type].push_back(index[n.id]);
        if (n.type == "Function.Call" || n.type == "Function.CallPure") {
            c.callFunction = functionIndex.at(n.param);
            const ScriptFunction& f = p->graph.functions[static_cast<std::size_t>(c.callFunction)];
            for (const ScriptParam& in : f.inputs)
                c.argPins.push_back(PinIndex(c, in.name, false));
            for (const ScriptParam& out : f.outputs)
                c.resultPins.push_back(PinIndex(c, out.name, true));
        }
        p->nodes.push_back(std::move(c));
    }
    for (const ScriptLink& l : p->graph.links) {
        const int from = index.at(l.fromNode), to = index.at(l.toNode);
        const int out = PinIndex(p->nodes[static_cast<std::size_t>(from)], l.fromPin, true);
        const int in  = PinIndex(p->nodes[static_cast<std::size_t>(to)], l.toPin, false);
        if (out < 0 || in < 0)
            continue; // validated before
        if (p->nodes[static_cast<std::size_t>(from)].pins[static_cast<std::size_t>(out)].type == PinType::Exec)
            p->nodes[static_cast<std::size_t>(from)].target[static_cast<std::size_t>(out)] = {to, in};
        else
            p->nodes[static_cast<std::size_t>(to)].source[static_cast<std::size_t>(in)] = {from, out};
    }
    for (std::size_t i = 0; i < p->graph.variables.size(); ++i)
        p->variableIndex[p->graph.variables[i].name] = static_cast<int>(i);
    return p;
}

} // namespace

struct ScriptSystem::Impl {
    struct Timer {
        std::int32_t handle = 0;
        std::string  event;
        float        interval = 0.0f;
        double       next     = 0.0;
        bool         loop     = false;
    };
    struct Instance {
        Entity                                 entity = NullEntity;
        std::uint64_t                          serial = 0; // distinguishes re-created instances
        std::shared_ptr<const Program>         program;
        std::vector<ScriptValue>               variables;
        std::vector<std::vector<ScriptValue>>  locals;  // per function
        std::vector<char>                      active;  // per function: on the call stack
        std::vector<std::vector<ScriptValue>>  outputs; // per node, per pin
        std::vector<ScriptContext::NodeState>  states;
        std::vector<std::uint64_t>             evalStamp; // per node: the execution its pure outputs were computed for
        std::vector<Timer>                     timers;
        std::unordered_set<int>                reported; // nodes whose runtime error was logged
        std::string                            graph;    // ScriptComponent::graph it was made for
        struct Binding {
            std::uint64_t entity = 0, serial = 0;
            std::string   event;
        };
        std::unordered_map<std::string, std::vector<Binding>> bindings; // dispatcher -> bound custom events
        std::vector<int>                                      ticking;  // nodes run with kScriptTick per update
    };
    struct Breakpoint {
        ScriptBreakpointOptions options;
        std::uint32_t           hits = 0; // since play began (condition true)
    };
    enum class StepMode : std::uint8_t { None, Into, Over, Out };
    static std::unordered_map<std::uint32_t, Breakpoint> BreakpointsOf(const ScriptGraph& graph)
    {
        std::unordered_map<std::uint32_t, Breakpoint> out;
        for (std::uint32_t node : graph.breakpoints) {
            const auto o = graph.breakpointOptions.find(node);
            out[node]    = {o != graph.breakpointOptions.end() ? o->second : ScriptBreakpointOptions{}, 0};
        }
        return out;
    }
    struct Continuation {
        int          node = 0;
        std::int32_t data = 0;
    };
    struct CallFrame {
        int                       callNode = 0; // -1: called from another script (CallFunctionOn)
        int                       function = 0;
        std::vector<Continuation> stack;             // the caller's
        std::vector<ScriptValue>* results = nullptr; // callNode -1: the function's outputs
    };
    // A running chain: where it is and what comes after.
    struct ChainState {
        std::uint64_t             id    = 0; // distinguishes chains (stepping over / out)
        int                       node  = 0;
        int                       entry = 0;
        std::int32_t              data  = 0;
        std::vector<Continuation> stack;
        std::vector<CallFrame>    calls;
        bool                      skipBreak = false; // resuming at a breakpoint
        std::uint32_t             lastUse   = 0;     // Macro node (CompiledNode::use) of the last node run
        bool                      atUse     = false; // paused on entering a macro copy: report its Macro node
    };
    struct SuspendedChain { // paused by the debugger, or queued while it pauses
        std::uint64_t entity = 0, serial = 0;
        ChainState    state;
    };
    struct Waiting {
        std::uint64_t entity = 0, serial = 0;
        int           node   = 0;
        std::int32_t  data   = 0;
        double        wake   = 0.0;
    };
    struct QueuedCollision {
        CollisionEvent event;
    };

    // The context node implementations see: one per running node / evaluation.
    class Exec final : public ScriptContext {
    public:
        // Each context is one execution: pure nodes are evaluated once per context (cached by stamp).
        Exec(Impl& impl, Scene& scene, Instance& instance)
            : m_Impl(impl), m_Scene(scene), m_Instance(instance), stamp(++impl.evalCounter)
        {
        }

        int                       node       = 0;
        std::int32_t              resumeData = 0;
        std::vector<Continuation> pushed;
        bool                      suspended = false;
        float                     suspendSeconds = 0.0f;
        std::int32_t              suspendData    = 0;
        int                       depth          = 0; // pure evaluation recursion

        const CompiledNode& Node() const { return m_Instance.program->nodes[static_cast<std::size_t>(node)]; }

        ScriptValue In(int pin) override
        {
            const CompiledNode& c   = Node();
            const PinRef        src = c.source[static_cast<std::size_t>(pin)];
            if (src.node < 0)
                return c.defaults[static_cast<std::size_t>(pin)];
            const CompiledNode& from = m_Instance.program->nodes[static_cast<std::size_t>(src.node)];
            if (from.desc->kind == NodeKind::Pure && m_Instance.evalStamp[static_cast<std::size_t>(src.node)] != stamp) {
                if (depth > 256) {
                    Error("Pure evaluation too deep");
                    return DefaultValue(c.pins[static_cast<std::size_t>(pin)].type);
                }
                const int saved = node;
                node            = src.node;
                ++depth;
                if (from.callFunction >= 0)
                    m_Impl.RunPureFunction(m_Scene, m_Instance, *this);
                else
                    from.desc->evaluate(*this);
                --depth;
                node = saved;
                m_Instance.evalStamp[static_cast<std::size_t>(src.node)] = stamp;
            }
            return ConvertFrom(m_Instance.outputs[static_cast<std::size_t>(src.node)][static_cast<std::size_t>(src.pin)],
                               from.pins[static_cast<std::size_t>(src.pin)].type, c.pins[static_cast<std::size_t>(pin)].type);
        }
        void Out(int pin, ScriptValue value) override
        {
            m_Instance.outputs[static_cast<std::size_t>(node)][static_cast<std::size_t>(pin)] = std::move(value);
        }
        bool Connected(int pin) const override { return Node().source[static_cast<std::size_t>(pin)].node >= 0; }
        const std::string& Param() const override { return Node().node->param; }
        std::span<const PinInfo> Pins() const override { return Node().pins; }
        NodeState&         State() override { return m_Instance.states[static_cast<std::size_t>(node)]; }
        Entity             Self() const override { return m_Instance.entity; }
        Scene&             GetScene() override { return m_Scene; }
        double             Time() const override { return m_Impl.time; }
        PhysicsWorld*      Physics() override { return m_Impl.physics; }
        AssetManager*      Assets() override { return m_Impl.assets; }
        AudioSystem*       Audio() override { return m_Impl.audio; }
        const Input*       GetInput() override { return m_Impl.acceptInput ? m_Impl.input : nullptr; }
        ScriptViewport     Viewport() const override { return m_Impl.viewport; }
        ScriptValue*       Variable(const std::string& name) override
        {
            const Program& p     = *m_Instance.program;
            const int      scope = Node().scope;
            if (scope >= 0) {
                const CompiledFunction& f = p.functions[static_cast<std::size_t>(scope)];
                if (const auto it = f.localIndex.find(name); it != f.localIndex.end())
                    return &m_Instance.locals[static_cast<std::size_t>(scope)][static_cast<std::size_t>(it->second)];
            }
            const auto it = p.variableIndex.find(name);
            return it != p.variableIndex.end() ? &m_Instance.variables[static_cast<std::size_t>(it->second)] : nullptr;
        }
        ScriptValue* InstanceVariable(Entity entity, const std::string& name) override
        {
            Instance* other = m_Impl.Find(entity);
            if (!other || !other->program)
                return nullptr;
            const auto it = other->program->variableIndex.find(name);
            return it != other->program->variableIndex.end() ? &other->variables[static_cast<std::size_t>(it->second)]
                                                             : nullptr;
        }
        bool CallEventOn(Entity entity, const std::string& name) override
        {
            Instance* other = m_Impl.Find(entity);
            if (!other || !other->program)
                return false;
            m_Impl.CallCustomEvent(m_Scene, *other, name);
            return true;
        }
        void Print(std::string text, float seconds) override { m_Impl.PrintMessage(std::move(text), seconds, false); }
        void Error(std::string message) override { m_Impl.RuntimeError(m_Instance, node, std::move(message)); }
        void PushContinuation(std::int32_t data) override { pushed.push_back({node, data}); }
        void Suspend(float seconds, std::int32_t data) override
        {
            suspended      = true;
            suspendSeconds = seconds;
            suspendData    = data;
        }
        std::int32_t ResumeData() const override { return resumeData; }
        void CallEvent(const std::string& name, std::vector<ScriptValue> args) override
        {
            m_Impl.CallCustomEvent(m_Scene, m_Instance, name, args);
        }
        bool CallFunctionOn(Entity target, const std::string& function, std::vector<ScriptValue> args,
                            std::vector<ScriptValue>& results) override
        {
            Instance* other = m_Impl.Find(target);
            if (!other || !other->program)
                return false;
            const auto it = other->program->functionIndex.find(function);
            if (it == other->program->functionIndex.end())
                return false;
            return m_Impl.CallFunction(m_Scene, *other, it->second, args, results);
        }
        bool Implements(Entity target, const std::string& interfaceName) override
        {
            const Instance* other = m_Impl.Find(target);
            return other && other->program &&
                   std::ranges::find(other->program->graph.interfaces, interfaceName) != other->program->graph.interfaces.end();
        }
        void CallDispatcher(const std::string& name, std::vector<ScriptValue> args) override
        {
            const auto it = m_Instance.bindings.find(name);
            if (it == m_Instance.bindings.end())
                return;
            const std::vector<Instance::Binding> bound = it->second; // events may bind / unbind meanwhile
            bool                                 dead  = false;
            for (const Instance::Binding& b : bound) {
                const auto t = m_Impl.instances.find(b.entity);
                if (t == m_Impl.instances.end() || t->second->serial != b.serial || !t->second->program) {
                    dead = true;
                    continue;
                }
                m_Impl.CallCustomEvent(m_Scene, *t->second, b.event, args);
            }
            if (dead)
                if (const auto again = m_Instance.bindings.find(name); again != m_Instance.bindings.end())
                    std::erase_if(again->second, [&](const Instance::Binding& b) {
                        const auto t = m_Impl.instances.find(b.entity);
                        return t == m_Impl.instances.end() || t->second->serial != b.serial;
                    });
        }
        bool BindDispatcher(Entity target, const std::string& dispatcher, const std::string& event, bool bind) override
        {
            Instance* other = m_Impl.Find(target);
            if (!other || !other->program || !other->program->graph.FindDispatcher(dispatcher))
                return false;
            std::vector<Instance::Binding>& list = other->bindings[dispatcher];
            const auto same = [&](const Instance::Binding& b) {
                return b.entity == EntityKey(m_Instance.entity) && b.serial == m_Instance.serial && b.event == event;
            };
            if (!bind)
                std::erase_if(list, same);
            else if (std::ranges::none_of(list, same))
                list.push_back({EntityKey(m_Instance.entity), m_Instance.serial, event});
            return true;
        }
        bool UnbindAll(Entity target, const std::string& dispatcher) override
        {
            Instance* other = m_Impl.Find(target);
            if (!other || !other->program || !other->program->graph.FindDispatcher(dispatcher))
                return false;
            other->bindings.erase(dispatcher);
            return true;
        }
        void         KeepModel(std::uint32_t index, std::uint32_t generation) override
        {
            if (m_Impl.constructing) // lives with the constructed entities
                m_Impl.constructionModels[m_Impl.constructing].push_back(ModelHandle{index, generation});
            else
                m_Impl.spawned.push_back(ModelHandle{index, generation});
        }
        std::int32_t SetTimer(const std::string& event, float seconds, bool loop) override
        {
            if (m_Impl.constructing) {
                Error("Timers do not run in the construction script");
                return 0;
            }
            if (event.empty() || !(seconds > 0.0f)) {
                Error("A timer needs an event name and a time > 0");
                return 0;
            }
            const std::int32_t handle = ++m_Impl.nextTimer;
            m_Instance.timers.push_back({handle, event, seconds, m_Impl.time + seconds, loop});
            return handle;
        }
        void ClearTimer(std::int32_t handle) override
        {
            std::erase_if(m_Instance.timers, [&](const Timer& t) { return t.handle == handle; });
        }
        void SetTicking(bool on) override
        {
            if (m_Impl.constructing) {
                if (on)
                    Error("Timelines and tweens do not run in the construction script");
                return;
            }
            std::vector<int>& t = m_Instance.ticking;
            const auto        it = std::ranges::find(t, node);
            if (on && it == t.end())
                t.push_back(node);
            else if (!on && it != t.end())
                t.erase(it);
        }
        float              DeltaTime() const override { return m_Impl.frameDt; }
        const ScriptGraph& Graph() const override { return m_Instance.program->graph; }
        const InputMap&    Inputs() const override { return m_Impl.inputMap; }
        void SaveSet(const std::string& slot, const std::string& key, const ScriptValue& value, PinType type) override
        {
            if (nlohmann::json* j = SlotOrError(slot))
                (*j)[key] = {{"type", ToString(type)}, {"value", SaveValueToJson(value, m_Scene.GetRegistry())}};
        }
        std::optional<ScriptValue> SaveGet(const std::string& slot, const std::string& key, PinType type) override
        {
            const nlohmann::json* j = SlotOrError(slot);
            if (!j || !j->contains(key))
                return std::nullopt;
            const nlohmann::json& entry = j->at(key);
            const auto stored = PinTypeFromString(entry.value("type", std::string()));
            if (!stored || !CanConvert(*stored, type))
                return std::nullopt;
            return ConvertFrom(SaveValueFromJson(entry.value("value", nlohmann::json()), *stored, m_Scene), *stored, type);
        }
        bool SaveWrite(const std::string& slot) override { return SlotOrError(slot) && m_Impl.WriteSlot(slot); }
        bool SaveRead(const std::string& slot) override { return ValidSlot(slot) && m_Impl.ReadSlot(slot); }
        bool SaveExists(const std::string& slot) override
        {
            std::error_code ec;
            return ValidSlot(slot) && !m_Impl.saveDirectory.empty() && std::filesystem::exists(m_Impl.SlotFile(slot), ec);
        }
        bool SaveDelete(const std::string& slot) override
        {
            if (!ValidSlot(slot))
                return false;
            m_Impl.saves.erase(slot);
            std::error_code ec;
            return !m_Impl.saveDirectory.empty() && std::filesystem::remove(m_Impl.SlotFile(slot), ec);
        }
        std::vector<std::string> SaveSlots() override
        {
            std::vector<std::string> names;
            std::error_code          ec;
            if (!m_Impl.saveDirectory.empty())
                for (std::filesystem::directory_iterator it(m_Impl.saveDirectory, ec), end; !ec && it != end; it.increment(ec))
                    if (it->path().extension() == ".sav")
                        names.push_back(PathToUtf8(it->path().stem()));
            for (const auto& [slot, values] : m_Impl.saves) // no save directory: the slots live in memory only
                if (m_Impl.saveDirectory.empty() && std::ranges::find(names, slot) == names.end())
                    names.push_back(slot);
            std::ranges::sort(names);
            return names;
        }
        void Construct(Entity root) override
        {
            Registry& r = m_Scene.GetRegistry();
            if (!r.Valid(root) || !r.Has<Hierarchy>(root))
                return;
            std::vector<Entity> subtree{root};
            for (std::size_t i = 0; i < subtree.size(); ++i)
                for (Entity child : r.Get<Hierarchy>(subtree[i]).children)
                    subtree.push_back(child);
            for (Entity e : subtree)
                if (r.Valid(e) && r.Has<ScriptComponent>(e))
                    (void)m_Impl.Construct(m_Scene, e);
        }
        void RequestLevel(std::string scene, bool quit) override { m_Impl.levelRequest = ScriptLevelRequest{std::move(scene), quit}; }
        const std::string& CurrentLevel() const override { return m_Impl.currentLevel; }

        float TimerRemaining(std::int32_t handle) const override
        {
            for (const Timer& t : m_Instance.timers)
                if (t.handle == handle)
                    return static_cast<float>(std::max(t.next - m_Impl.time, 0.0));
            return -1.0f;
        }

    private:
        bool ValidSlot(const std::string& slot)
        {
            if (IsValidScriptName(slot))
                return true;
            Error("Invalid save slot name '" + slot + "' (letters, digits, '_', ' ')");
            return false;
        }
        nlohmann::json* SlotOrError(const std::string& slot) { return ValidSlot(slot) ? &m_Impl.Slot(slot) : nullptr; }

        Impl&     m_Impl;
        Scene&    m_Scene;
        Instance& m_Instance;

    public:
        const std::uint64_t stamp;
    };

    Impl(EventBus& bus, const Input* in, PhysicsWorld* phys, AssetManager* am, AudioSystem* au)
        : events(bus), input(in), physics(phys), assets(am), audio(au)
    {
        collisionSub = events.Subscribe<CollisionEvent>([this](const CollisionEvent& e) {
            if (running)
                collisions.push_back({e});
            return false;
        });
    }

    Instance* Find(Entity e)
    {
        const auto it = instances.find(EntityKey(e));
        return it != instances.end() ? it->second.get() : nullptr;
    }

    // --- Save games ---

    std::filesystem::path SlotFile(const std::string& slot) const { return saveDirectory / PathFromUtf8(slot + ".sav"); }

    // The cached values of a slot (read from its file the first time).
    nlohmann::json& Slot(const std::string& slot)
    {
        if (const auto it = saves.find(slot); it != saves.end())
            return it->second;
        if (!ReadSlot(slot))
            saves[slot] = nlohmann::json::object();
        return saves[slot];
    }

    bool ReadSlot(const std::string& slot)
    {
        if (saveDirectory.empty())
            return saves.contains(slot); // memory only
        std::ifstream in(SlotFile(slot), std::ios::binary);
        if (!in)
            return false;
        try {
            const nlohmann::json root = nlohmann::json::parse(in);
            saves[slot]               = root.value("values", nlohmann::json::object());
            return true;
        } catch (const std::exception& e) {
            PrintMessage("Save slot '" + slot + "' is damaged: " + e.what(), 4.0f, true);
            return false;
        }
    }

    bool WriteSlot(const std::string& slot)
    {
        if (saveDirectory.empty())
            return true; // memory only
        std::error_code ec;
        std::filesystem::create_directories(saveDirectory, ec);
        const std::filesystem::path file = SlotFile(slot), temp = file.string() + ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << nlohmann::json{{"version", 1}, {"values", Slot(slot)}}.dump(2);
            if (!out)
                return false;
        }
        std::filesystem::rename(temp, file, ec);
        return !ec;
    }

    // --- Programs ---

    std::shared_ptr<const Program> ProgramFor(const std::string& graphPath)
    {
        const std::string key = ScriptSystem::Key(graphPath);
        if (const auto it = programs.find(key); it != programs.end())
            return it->second;
        ScriptGraph                   graph;
        std::vector<ScriptDiagnostic> diagnostics;
        if (const auto o = overrides.find(key); o != overrides.end()) {
            graph = o->second;
        } else {
            try {
                graph = LoadScriptGraph(graphPath);
            } catch (const std::exception& e) {
                diagnostics.push_back({0, e.what(), true});
            }
        }
        if (diagnostics.empty())
            diagnostics = ValidateScriptGraph(graph);
        if (!breakpoints.contains(key))
            breakpoints[key] = BreakpointsOf(graph);
        // Macros and library functions: copied in. Problems the copies cause (e.g. recursion through
        // a macro) are reported at their origin.
        std::unordered_map<std::uint32_t, ScriptOrigin> origins;
        if (std::ranges::none_of(diagnostics, &ScriptDiagnostic::error)) {
            ScriptGraph expanded = graph;
            if (std::string problem = ExpandScriptGraph(expanded, origins); !problem.empty()) {
                diagnostics.push_back({0, std::move(problem), true});
            } else {
                for (const ScriptDiagnostic& d : ValidateExpandedScriptGraph(expanded)) {
                    const auto       o = origins.find(d.node);
                    ScriptDiagnostic mapped =
                        o == origins.end()             ? d
                        : o->second.library.empty() ? ScriptDiagnostic{o->second.node, d.message, d.error}
                                                     : ScriptDiagnostic{o->second.use, "Library '" + o->second.library + "': " + d.message, d.error};
                    if (std::ranges::none_of(diagnostics, [&](const ScriptDiagnostic& x) {
                            return x.node == mapped.node && x.message == mapped.message;
                        }))
                        diagnostics.push_back(std::move(mapped));
                }
                graph = std::move(expanded);
            }
        }
        auto program = Compile(key, std::move(graph), diagnostics, origins);
        for (std::size_t i = 1; i < program->files.size(); ++i) // breakpoints saved in libraries
            if (const ScriptLibrary* lib = ScriptRegistry::FindLibrary(program->fileLibraries[i]);
                lib && !breakpoints.contains(program->files[i]))
                breakpoints[program->files[i]] = BreakpointsOf(lib->graph);
        ScriptDebugInfo& info = debug[key];
        info.diagnostics      = program->diagnostics;
        if (!program->ok) {
            for (const ScriptDiagnostic& d : program->diagnostics)
                if (d.error)
                    ENGINE_ERROR("[Script] {}: {}{}", graphPath, d.node ? "node " + std::to_string(d.node) + ": " : "",
                                 d.message);
            PrintMessage("Script '" + std::filesystem::path(graphPath).filename().string() + "' has errors (not run)",
                         5.0f, true);
        }
        programs[key] = program;
        return program;
    }

    // --- Instances ---

    Instance* CreateInstance(Scene& scene, Entity e, const std::string& graph)
    {
        auto      inst = MakeInstance(scene, e, graph);
        Instance* raw  = inst.get();
        instances[EntityKey(e)] = std::move(inst);
        return raw;
    }

    std::unique_ptr<Instance> MakeInstance(Scene& scene, Entity e, const std::string& graph)
    {
        auto program  = ProgramFor(graph);
        auto inst     = std::make_unique<Instance>();
        inst->entity  = e;
        inst->serial  = ++nextSerial;
        inst->graph   = graph;
        inst->program = program->ok ? program : nullptr;
        if (inst->program) {
            for (const ScriptVariable& v : program->graph.variables)
                inst->variables.push_back(v.value);
            // Instance-editable variables: the entity's values.
            if (const ScriptComponent* sc = scene.GetRegistry().TryGet<ScriptComponent>(e))
                for (const auto& [name, value] : sc->variables) {
                    const auto it = program->variableIndex.find(name);
                    if (it == program->variableIndex.end())
                        continue;
                    const ScriptVariable& v = program->graph.variables[static_cast<std::size_t>(it->second)];
                    ScriptValue&          slot = inst->variables[static_cast<std::size_t>(it->second)];
                    if (!v.exposed)
                        continue;
                    if (v.type == PinType::Entity)
                        slot = value.entityUuid ? scene.FindByUuid(value.entityUuid) : NullEntity;
                    else if (TypeOf(value.value) == v.type || CanConvert(TypeOf(value.value), v.type))
                        slot = Convert(value.value, v.type);
                }
            for (const CompiledFunction& f : program->functions) {
                std::vector<ScriptValue> locals;
                for (const ScriptVariable& v : f.locals)
                    locals.push_back(v.value);
                inst->locals.push_back(std::move(locals));
            }
            inst->active.assign(program->functions.size(), 0);
            inst->outputs.resize(program->nodes.size());
            for (std::size_t i = 0; i < program->nodes.size(); ++i) {
                const CompiledNode& c = program->nodes[i];
                inst->outputs[i].resize(c.pins.size(), false);
                for (std::size_t p = 0; p < c.pins.size(); ++p)
                    if (c.pins[p].output && c.pins[p].type != PinType::Exec)
                        inst->outputs[i][p] = DefaultValue(c.pins[p].type);
            }
            inst->states.resize(program->nodes.size());
            inst->evalStamp.assign(program->nodes.size(), 0);
            for (std::size_t i = 0; i < program->nodes.size(); ++i) // auto-play timelines
                if (program->nodes[i].node->type == "Timeline.Play")
                    if (const ScriptTimeline* t = program->graph.FindTimeline(program->nodes[i].node->param); t && t->autoPlay) {
                        inst->states[i].flag = true;
                        inst->ticking.push_back(static_cast<int>(i));
                    }
        }
        return inst;
    }

    // --- Construction scripts ---

    // Destroys what the owner's construction script made and releases its models.
    void DestroyConstructed(Scene& scene, std::uint64_t owner)
    {
        std::vector<Entity> owned;
        scene.GetRegistry().ViewOf<ConstructionOwned>().Each([&](Entity e, ConstructionOwned& o) {
            if (o.owner == owner)
                owned.push_back(e);
        });
        for (Entity e : owned)
            if (scene.GetRegistry().Valid(e))
                scene.DestroyEntity(e);
        if (const auto it = constructionModels.find(owner); it != constructionModels.end()) {
            if (assets)
                for (ModelHandle h : it->second)
                    assets->Release(h);
            constructionModels.erase(it);
        }
    }

    // Runs Event.Construction of the entity's script on a temporary instance; the entities it
    // creates are marked as owned (rebuilt next time, not saved). False: no construction script.
    bool Construct(Scene& scene, Entity e)
    {
        Registry& r = scene.GetRegistry();
        if (!r.Valid(e) || !r.Has<Uuid>(e) || r.Has<ConstructionOwned>(e))
            return false;
        const std::uint64_t owner = r.Get<Uuid>(e).value;
        DestroyConstructed(scene, owner);
        RestoreOwner(scene, e, owner); // like UE: every run starts from the owner without the script's changes
        const ScriptComponent* sc = r.TryGet<ScriptComponent>(e);
        if (!sc || sc->graph.empty())
            return false;
        const std::shared_ptr<const Program> program = ProgramFor(sc->graph);
        if (!program->ok || !program->events.contains("Event.Construction"))
            return false;
        const std::string                 ownerBefore = SnapshotEntityState(scene, e);
        std::unordered_set<std::uint64_t> before;
        r.ViewOf<Uuid>().Each([&](Entity, Uuid& u) { before.insert(u.value); });
        const std::unique_ptr<Instance> inst  = MakeInstance(scene, e, sc->graph);
        const std::uint64_t             outer = std::exchange(constructing, owner); // spawned prefabs construct nested
        FireSimple(scene, *inst, "Event.Construction");
        constructing = outer;
        std::vector<Entity> created;
        r.ViewOf<Uuid>().Each([&](Entity x, Uuid& u) {
            if (!before.contains(u.value))
                created.push_back(x);
        });
        for (Entity x : created) // entities of nested constructions keep their own owner
            if (r.Valid(x) && !r.Has<ConstructionOwned>(x))
                r.Emplace<ConstructionOwned>(x, ConstructionOwned{owner});
        scene.UpdateTransforms();
        if (r.Valid(e))
            constructionStates[owner] = {ownerBefore, SnapshotEntityState(scene, e)};
        return true;
    }

    // The owner as it was before its construction script last ran, with the edits made since
    // (the difference to the script's result) on top.
    void RestoreOwner(Scene& scene, Entity e, std::uint64_t owner)
    {
        const auto it = constructionStates.find(owner);
        if (it == constructionStates.end())
            return;
        const auto [before, after] = it->second;
        constructionStates.erase(it);
        const std::string current = SnapshotEntityState(scene, e);
        ApplyEntityState(scene, e, before);
        if (current != after)
            (void)ApplyEntityStateDiff(scene, e, after, current);
    }

    std::size_t ConstructAll(Scene& scene)
    {
        Registry&                         r = scene.GetRegistry();
        std::vector<Entity>               owners;
        std::unordered_set<std::uint64_t> alive;
        r.ViewOf<ScriptComponent>().Each([&](Entity e, ScriptComponent&) {
            if (!r.Has<ConstructionOwned>(e)) {
                owners.push_back(e);
                alive.insert(r.Get<Uuid>(e).value);
            }
        });
        // Leftovers of owners that are gone (or lost their script).
        std::unordered_set<std::uint64_t> orphaned;
        r.ViewOf<ConstructionOwned>().Each([&](Entity, ConstructionOwned& o) {
            if (!alive.contains(o.owner))
                orphaned.insert(o.owner);
        });
        for (const auto& [owner, models] : constructionModels)
            if (!alive.contains(owner))
                orphaned.insert(owner);
        for (std::uint64_t owner : orphaned) {
            DestroyConstructed(scene, owner);
            constructionStates.erase(owner);
        }
        std::size_t count = 0;
        for (Entity e : owners)
            count += Construct(scene, e) ? 1 : 0;
        return count;
    }

    // Adds / removes instances to match the scene's ScriptComponents. Decides first, then fires
    // (scripts may create / destroy entities, which must not happen while the view iterates).
    void SyncInstances(Scene& scene)
    {
        Registry&                                   registry = scene.GetRegistry();
        std::unordered_set<std::uint64_t>           seen;
        std::vector<std::pair<Entity, std::string>> added;
        std::vector<std::uint64_t>                  replaced;
        registry.ViewOf<ScriptComponent>().Each([&](Entity e, ScriptComponent& script) {
            if (script.graph.empty())
                return;
            seen.insert(EntityKey(e));
            const auto it = instances.find(EntityKey(e));
            if (it != instances.end() && it->second->graph == script.graph)
                return;
            if (it != instances.end())
                replaced.push_back(EntityKey(e));
            added.emplace_back(e, script.graph);
        });
        std::vector<std::unique_ptr<Instance>> ended; // kept alive while their EndPlay runs
        for (auto it = instances.begin(); it != instances.end();) {
            if (seen.contains(it->first) && std::ranges::find(replaced, it->first) == replaced.end()) {
                ++it;
                continue;
            }
            ended.push_back(std::move(it->second));
            it = instances.erase(it);
        }
        for (const auto& inst : ended) // graph changed or component removed: the entity lives on
            if (registry.Valid(inst->entity) && registry.Has<Transform>(inst->entity))
                FireSimple(scene, *inst, "Event.EndPlay");
        for (const auto& [e, graph] : added)
            if (registry.Valid(e) && registry.Has<ScriptComponent>(e))
                FireSimple(scene, *CreateInstance(scene, e, graph), "Event.BeginPlay");
    }

    // --- Execution ---

    void MarkNode(const Instance& inst, int node)
    {
        const Program&      p = *inst.program;
        const CompiledNode& c = p.nodes[static_cast<std::size_t>(node)];
        debug[p.files[static_cast<std::size_t>(c.file)]].nodeTimes[c.id] = time;
        if (c.use) // a copy of a macro: its Macro node runs too
            debug[p.key].nodeTimes[c.use] = time;
    }

    // Exec output (node, pin) -> next (node, input pin); records the link for the debugger.
    bool Follow(const Instance& inst, int node, int pin, int& nextNode, int& nextEntry)
    {
        const CompiledNode& c = inst.program->nodes[static_cast<std::size_t>(node)];
        if (pin < 0 || static_cast<std::size_t>(pin) >= c.target.size())
            return false;
        const PinRef t = c.target[static_cast<std::size_t>(pin)];
        if (t.node < 0)
            return false;
        debug[inst.program->files[static_cast<std::size_t>(c.file)]].linkTimes[{c.id, c.pins[static_cast<std::size_t>(pin)].name}] = time;
        nextNode  = t.node;
        nextEntry = t.pin;
        return true;
    }

    enum class Enter { Entered, Empty, Failed };

    // Impure / pure call: arguments into the Entry node, fresh locals, a new frame. The chain
    // continues at the function body (Entered) or returns at once (Empty: nothing connected).
    Enter EnterFunction(Scene& scene, Instance& inst, ChainState& st, Exec* caller)
    {
        const Program&          p    = *inst.program;
        const CompiledNode&     call = p.nodes[static_cast<std::size_t>(st.node)];
        const auto              f    = static_cast<std::size_t>(call.callFunction);
        const CompiledFunction& fn   = p.functions[f];
        if (inst.active[f]) {
            RuntimeError(inst, st.node, "Recursive call of '" + fn.name + "' (not supported)");
            return Enter::Failed;
        }
        Exec        own(*this, scene, inst);
        Exec&       ctx   = caller ? *caller : own;
        const int   saved = ctx.node;
        ctx.node          = st.node;
        for (std::size_t i = 0; i < call.argPins.size(); ++i)
            inst.outputs[static_cast<std::size_t>(fn.entry)][i + 1] = ctx.In(call.argPins[i]);
        ctx.node = saved;
        for (std::size_t i = 0; i < fn.locals.size(); ++i)
            inst.locals[f][i] = fn.locals[i].value;
        inst.active[f] = 1;
        st.calls.push_back({st.node, static_cast<int>(f), std::move(st.stack)});
        st.stack.clear();
        MarkNode(inst, fn.entry);
        int next = 0, entry = 0;
        if (!Follow(inst, fn.entry, 0, next, entry))
            return Enter::Empty;
        st.node  = next;
        st.entry = entry;
        st.data  = 0;
        return Enter::Entered;
    }

    // Leaves the innermost function: the caller's loop stack comes back. Returns the call node.
    int PopFrame(Instance& inst, ChainState& st)
    {
        CallFrame frame = std::move(st.calls.back());
        st.calls.pop_back();
        st.stack = std::move(frame.stack);
        inst.active[static_cast<std::size_t>(frame.function)] = 0;
        return frame.callNode;
    }

    bool ShouldBreak(Instance& inst, ChainState& st)
    {
        if (st.skipBreak || constructing)
            return false;
        switch (stepMode) {
        case StepMode::Into: return true;
        case StepMode::Over:
            if (st.id != stepChain || st.calls.size() <= stepDepth)
                return true;
            break;
        case StepMode::Out:
            if (st.id != stepChain || st.calls.size() < stepDepth)
                return true;
            break;
        case StepMode::None: break;
        }
        const CompiledNode& c = inst.program->nodes[static_cast<std::size_t>(st.node)];
        // Entering a copy of a macro whose Macro node has a breakpoint, else the node's own.
        Breakpoint* use = nullptr;
        if (c.use && c.use != st.lastUse)
            if (const auto own = breakpoints.find(inst.program->key); own != breakpoints.end())
                if (const auto b = own->second.find(c.use); b != own->second.end())
                    use = &b->second;
        Breakpoint* node = nullptr;
        if (const auto it = breakpoints.find(inst.program->files[static_cast<std::size_t>(c.file)]); it != breakpoints.end())
            if (const auto b = it->second.find(c.id); b != it->second.end())
                node = &b->second;
        if (use && Hit(inst, c, *use)) {
            st.atUse = true;
            return true;
        }
        return node && Hit(inst, c, *node);
    }

    // A breakpoint reached: its condition and hit count decide.
    bool Hit(Instance& inst, const CompiledNode& c, Breakpoint& b)
    {
        if (!b.options.condition.empty()) {
            // Variables as the node sees them: the running function's locals first.
            const auto lookup = [&](std::string_view name) -> const ScriptValue* {
                if (c.scope >= 0) {
                    const CompiledFunction& f = inst.program->functions[static_cast<std::size_t>(c.scope)];
                    if (const auto l = f.localIndex.find(std::string(name)); l != f.localIndex.end())
                        return &inst.locals[static_cast<std::size_t>(c.scope)][static_cast<std::size_t>(l->second)];
                }
                const auto v = inst.program->variableIndex.find(std::string(name));
                return v != inst.program->variableIndex.end() ? &inst.variables[static_cast<std::size_t>(v->second)] : nullptr;
            };
            std::string error;
            const auto  result = EvaluateScriptCondition(b.options.condition, lookup, &error);
            if (!result)
                RuntimeError(inst, static_cast<int>(&c - inst.program->nodes.data()), "Breakpoint condition: " + error); // stops: fix it
            else if (!*result)
                return false;
        }
        return ++b.hits >= b.options.hitCount;
    }

    // Runs a chain until it ends (true) or stops at a breakpoint (false, state kept in `paused`).
    // nested: a pure function call inside an evaluation - never pauses, ends when its frame returns.
    bool RunState(Scene& scene, Instance& inst, ChainState& st, bool nested)
    {
        // Instances are only created / dropped in SyncInstances and Clear, never while a chain runs.
        const std::uint64_t key = EntityKey(inst.entity), serial = inst.serial;
        const std::size_t   base  = nested ? st.calls.size() - 1 : 0;
        const Program&      p     = *inst.program;
        std::uint32_t       steps = 0;
        for (;;) {
            if (++steps > maxSteps) {
                RuntimeError(inst, st.node, "Too many steps in one event (infinite loop?) - chain aborted");
                while (st.calls.size() > base)
                    PopFrame(inst, st);
                return true;
            }
            if (!nested && ShouldBreak(inst, st)) {
                stepMode = StepMode::None;
                paused   = SuspendedChain{key, serial, std::move(st)};
                return false;
            }
            st.skipBreak = false;
            st.atUse     = false;
            ++stats.nodesExecuted;
            MarkNode(inst, st.node);
            st.lastUse = p.nodes[static_cast<std::size_t>(st.node)].use;
            const CompiledNode& c    = p.nodes[static_cast<std::size_t>(st.node)];
            int                 from = st.node;
            int                 out  = kScriptStop;
            if (c.callFunction >= 0) { // impure call: into the function
                const Enter e = EnterFunction(scene, inst, st, nullptr);
                if (e == Enter::Entered)
                    continue;
                out = e == Enter::Failed ? 1 : kScriptStop; // Empty: returns right away (below)
            } else {
                Exec ctx(*this, scene, inst);
                ctx.node       = st.node;
                ctx.resumeData = st.data;
                out            = c.desc->execute ? c.desc->execute(ctx, st.entry) : kScriptStop;
                // Continuations pushed now run after the chain that starts here: LIFO, pushed last = next.
                for (auto it = ctx.pushed.rbegin(); it != ctx.pushed.rend(); ++it)
                    st.stack.push_back(*it);
                if (ctx.suspended && constructing)
                    RuntimeError(inst, st.node, "Latent nodes do not run in the construction script");
                else if (ctx.suspended)
                    waiting.push_back({key, serial, st.node, ctx.suspendData, time + ctx.suspendSeconds});
                if (out == kScriptReturn) {
                    out = kScriptStop;
                    if (!st.calls.empty()) { // hand the outputs to the caller, back after the call
                        const CallFrame& frame = st.calls.back();
                        if (frame.callNode < 0) {
                            for (std::size_t i = 0; i < frame.results->size(); ++i)
                                (*frame.results)[i] = ctx.In(static_cast<int>(i) + 1);
                        } else {
                            const CompiledNode& callNode = p.nodes[static_cast<std::size_t>(frame.callNode)];
                            for (std::size_t i = 0; i < callNode.resultPins.size(); ++i)
                                inst.outputs[static_cast<std::size_t>(frame.callNode)]
                                            [static_cast<std::size_t>(callNode.resultPins[i])] = ctx.In(static_cast<int>(i) + 1);
                        }
                        st.stack.clear();
                        from = PopFrame(inst, st);
                        if (nested && st.calls.size() == base)
                            return true;
                        out = 1; // "Then" of the call
                    }
                }
            }
            // Next node: the exec output, else a pushed continuation, else the end of a function.
            for (;;) {
                int next = 0, entry = 0;
                if (out >= 0 && Follow(inst, from, out, next, entry)) {
                    st.node  = next;
                    st.entry = entry;
                    st.data  = 0;
                    break;
                }
                if (!st.stack.empty()) {
                    st.node  = st.stack.back().node;
                    st.data  = st.stack.back().data;
                    st.entry = kScriptResume;
                    st.stack.pop_back();
                    break;
                }
                if (st.calls.size() > base) { // function body ended without a Return
                    from = PopFrame(inst, st);
                    if (nested && st.calls.size() == base)
                        return true;
                    out = 1;
                    continue;
                }
                return true;
            }
        }
    }

    // Pure function: runs now, its Return writes the call node's outputs.
    void RunPureFunction(Scene& scene, Instance& inst, Exec& caller)
    {
        ChainState st;
        st.node = caller.node; // the call node
        switch (EnterFunction(scene, inst, st, &caller)) {
        case Enter::Failed: return;
        case Enter::Empty: PopFrame(inst, st); return;
        case Enter::Entered: break;
        }
        RunState(scene, inst, st, true);
    }

    // Called from another script (interfaces): runs the function to its end now, like a pure one.
    bool CallFunction(Scene& scene, Instance& inst, int function, const std::vector<ScriptValue>& args,
                      std::vector<ScriptValue>& results)
    {
        const Program&          p  = *inst.program;
        const auto              f  = static_cast<std::size_t>(function);
        const CompiledFunction& fn = p.functions[f];
        const ScriptFunction&   sf = p.graph.functions[f];
        if (inst.active[f]) {
            PrintMessage("Recursive call of '" + fn.name + "' (not supported)", 3.0f, true);
            return false;
        }
        const auto fit = [](const ScriptValue& v, PinType t) {
            return ValueFits(v, t) ? v : CanConvert(TypeOf(v), t) ? Convert(v, t) : DefaultValue(t);
        };
        for (std::size_t i = 0; i < sf.inputs.size(); ++i)
            inst.outputs[static_cast<std::size_t>(fn.entry)][i + 1] =
                i < args.size() ? fit(args[i], sf.inputs[i].type) : DefaultValue(sf.inputs[i].type);
        results.clear();
        for (const ScriptParam& out : sf.outputs)
            results.push_back(DefaultValue(out.type));
        for (std::size_t i = 0; i < fn.locals.size(); ++i)
            inst.locals[f][i] = fn.locals[i].value;
        inst.active[f] = 1;
        ChainState st;
        st.calls.push_back({-1, function, {}, &results});
        MarkNode(inst, fn.entry);
        int next = 0, entry = 0;
        if (!Follow(inst, fn.entry, 0, next, entry)) {
            PopFrame(inst, st);
            return true;
        }
        st.node  = next;
        st.entry = entry;
        RunState(scene, inst, st, true);
        return true;
    }

    // A new chain: queued while the debugger pauses.
    void RunChain(Scene& scene, Instance& inst, int node, int entry, std::int32_t data)
    {
        ChainState st;
        st.id        = ++nextChain;
        st.lastUse   = entry < 0 ? inst.program->nodes[static_cast<std::size_t>(node)].use : 0; // resumed inside a macro copy
        st.node      = node;
        st.entry     = entry;
        st.data      = data;
        st.skipBreak = entry == kScriptTick; // a breakpoint on a timeline stops when it is triggered, not every frame
        if (paused) {
            queued.push_back({EntityKey(inst.entity), inst.serial, std::move(st)});
            return;
        }
        RunState(scene, inst, st, false);
    }

    // Resumes the paused chain, then the queued ones, until all ran or one pauses.
    void ResumeChains(Scene& scene)
    {
        const auto resume = [&](SuspendedChain chain, bool skip) {
            const auto it = instances.find(chain.entity);
            if (it == instances.end() || it->second->serial != chain.serial || !it->second->program)
                return;
            const std::uint64_t chainId = chain.state.id;
            chain.state.skipBreak = skip;
            const bool finished = RunState(scene, *it->second, chain.state, false);
            // A step request belongs to the chain that was paused. If that chain ends without
            // reaching another node, do not carry the step mode into a later event/tick.
            if (finished && stepMode != StepMode::None && stepChain == chainId)
                stepMode = StepMode::None;
        };
        if (paused) {
            SuspendedChain chain = std::move(*paused);
            paused.reset();
            resume(std::move(chain), true);
        }
        while (!paused && !queued.empty()) {
            SuspendedChain chain = std::move(queued.front());
            queued.pop_front();
            resume(std::move(chain), false);
        }
    }

    // Fires all event nodes of `type` (matching param if given); `outputs` fills their output pins.
    void Fire(Scene& scene, Instance& inst, const std::string& type, const std::string* param,
              const std::function<void(const CompiledNode&, std::vector<ScriptValue>&)>& outputs)
    {
        if (!inst.program)
            return;
        const auto it = inst.program->events.find(type);
        if (it == inst.program->events.end())
            return;
        const std::vector<int> nodes = it->second;
        for (int node : nodes) {
            if (param && inst.program->nodes[static_cast<std::size_t>(node)].node->param != *param)
                continue;
            ++stats.eventsFired;
            MarkNode(inst, node);
            if (outputs)
                outputs(inst.program->nodes[static_cast<std::size_t>(node)], inst.outputs[static_cast<std::size_t>(node)]);
            int next = 0, entry = 0;
            if (Follow(inst, node, 0, next, entry))
                RunChain(scene, inst, next, entry, 0);
        }
    }

    void FireSimple(Scene& scene, Instance& inst, const std::string& type) { Fire(scene, inst, type, nullptr, {}); }

    // Arguments fill the event's parameter outputs in order (converted where possible).
    void CallCustomEvent(Scene& scene, Instance& inst, const std::string& name, const std::vector<ScriptValue>& args = {})
    {
        if (callDepth >= 32) {
            PrintMessage("Custom event recursion too deep: " + name, 3.0f, true);
            return;
        }
        ++callDepth;
        Fire(scene, inst, "Event.Custom", &name, [&](const CompiledNode& c, std::vector<ScriptValue>& out) {
            for (std::size_t i = 1; i < c.pins.size(); ++i) {
                const PinType t = c.pins[i].type;
                if (i - 1 >= args.size())
                    out[i] = DefaultValue(t);
                else if (ValueFits(args[i - 1], t))
                    out[i] = args[i - 1];
                else
                    out[i] = CanConvert(TypeOf(args[i - 1]), t) ? Convert(args[i - 1], t) : DefaultValue(t);
            }
        });
        --callDepth;
    }

    void PrintMessage(std::string text, float seconds, bool error)
    {
        if (error)
            ENGINE_WARN("[Script] {}", text);
        else
            ENGINE_INFO("[Script] {}", text);
        messages.push_back({std::move(text), time, std::max(seconds, 0.0f), error});
        if (messages.size() > 64)
            messages.erase(messages.begin());
    }

    void RuntimeError(Instance& inst, int node, std::string message)
    {
        ++stats.errors;
        if (!inst.reported.insert(node).second)
            return;
        const CompiledNode& c    = inst.program->nodes[static_cast<std::size_t>(node)];
        const std::string&  file = inst.program->files[static_cast<std::size_t>(c.file)];
        debug[file].diagnostics.push_back({c.id, message, true});
        PrintMessage(std::filesystem::path(file).filename().string() + ": " +
                         inst.program->nodes[static_cast<std::size_t>(node)].desc->title + ": " + message,
                     4.0f, true);
    }

    void Clear()
    {
        instances.clear();
        waiting.clear();
        collisions.clear();
        paused.reset();
        queued.clear();
        stepMode = StepMode::None;
        if (assets)
            for (ModelHandle h : spawned)
                assets->Release(h);
        spawned.clear();
    }

    EventBus&     events;
    const Input*  input;
    PhysicsWorld* physics;
    AssetManager* assets;
    AudioSystem*  audio;
    Subscription  collisionSub;

    bool           running     = false;
    bool           acceptInput = true;
    double         time        = 0.0;
    float          frameDt     = 0.0f;
    std::uint32_t  maxSteps    = 100000;
    int            callDepth   = 0;
    std::uint32_t  evalCounter = 0; // Exec stamps (pure evaluation cache)
    std::uint64_t  nextSerial  = 0;
    std::int32_t   nextTimer   = 0;
    ScriptViewport viewport;

    std::unordered_map<std::string, std::shared_ptr<const Program>> programs; // by key, cleared at Begin
    std::unordered_map<std::string, ScriptGraph>                    overrides;
    std::unordered_map<std::string, ScriptDebugInfo>                debug;
    std::unordered_map<std::uint64_t, std::unique_ptr<Instance>>    instances; // by entity
    std::vector<Waiting>                                            waiting;
    std::vector<QueuedCollision>                                    collisions;
    std::vector<ModelHandle>                                        spawned;
    std::vector<ScriptMessage>                                      messages;
    ScriptStats                                                     stats;
    InputMap                                                        inputMap;
    std::filesystem::path                                           saveDirectory;
    std::map<std::string, nlohmann::json>                           saves; // slot -> values (cache)
    std::optional<ScriptLevelRequest>                               levelRequest;
    std::uint64_t                                                   constructing = 0; // owner UUID while a construction script runs
    std::unordered_map<std::uint64_t, std::vector<ModelHandle>>     constructionModels; // by owner UUID
    // Owner state before / after its construction script last ran (by owner UUID).
    std::unordered_map<std::uint64_t, std::pair<std::string, std::string>> constructionStates;
    std::string                                                     currentLevel;

    // Debugger.
    std::unordered_map<std::string, std::unordered_map<std::uint32_t, Breakpoint>> breakpoints; // by program key
    std::optional<SuspendedChain>                                                   paused;
    std::deque<SuspendedChain>                                                      queued;
    StepMode                                                                        stepMode  = StepMode::None;
    std::size_t                                                                     stepDepth = 0;
    std::uint64_t                                                                   stepChain = 0;
    std::uint64_t                                                                   nextChain = 0;
};

ScriptSystem::ScriptSystem(EventBus& events, const Input* input, PhysicsWorld* physics, AssetManager* assets,
                           AudioSystem* audio)
    : m_Impl(std::make_unique<Impl>(events, input, physics, assets, audio))
{
}

ScriptSystem::~ScriptSystem()
{
    m_Impl->Clear();
    if (m_Impl->assets)
        for (const auto& [owner, models] : m_Impl->constructionModels)
            for (ModelHandle h : models)
                m_Impl->assets->Release(h);
}

std::string ScriptSystem::Key(const std::filesystem::path& file)
{
    if (const std::u8string name = file.generic_u8string(); name.starts_with(u8"library:"))
        return {name.begin(), name.end()}; // libraries registered in code (no file)
    std::error_code ec;
    const std::u8string s = std::filesystem::absolute(file, ec).lexically_normal().generic_u8string();
    return {s.begin(), s.end()};
}

void ScriptSystem::Begin(Scene& scene)
{
    Impl& w = *m_Impl;
    if (w.running)
        End(scene);
    w.programs.clear(); // graphs may have changed since the last play
    w.debug.clear();
    w.messages.clear();
    w.stats    = {};
    w.time     = 0.0;
    w.running  = true;
    w.maxSteps = maxStepsPerEvent;
    scene.UpdateTransforms();
    w.ConstructAll(scene); // fresh constructed entities before BeginPlay
    w.SyncInstances(scene);
    w.stats.instances = static_cast<std::uint32_t>(w.instances.size());
    for (const auto& [key, inst] : w.instances)
        w.stats.ticking += static_cast<std::uint32_t>(inst->ticking.size());
    scene.UpdateTransforms();
}

void ScriptSystem::Update(Scene& scene, float dt, bool acceptInput)
{
    Impl& w = *m_Impl;
    if (!w.running || w.paused) // the debugger holds everything
        return;
    w.maxSteps            = maxStepsPerEvent;
    w.acceptInput         = acceptInput;
    w.stats.nodesExecuted = 0;
    w.stats.eventsFired   = 0;
    w.time += std::max(dt, 0.0f);
    w.frameDt = std::max(dt, 0.0f);
    scene.UpdateTransforms();

    w.SyncInstances(scene);

    // Delays that are due (in wake order; a resumed chain may park new ones for later).
    std::vector<Impl::Waiting> due;
    std::erase_if(w.waiting, [&](const Impl::Waiting& t) {
        if (t.wake > w.time)
            return false;
        due.push_back(t);
        return true;
    });
    std::ranges::stable_sort(due, {}, &Impl::Waiting::wake);
    for (const Impl::Waiting& t : due) {
        const auto it = w.instances.find(t.entity);
        if (it != w.instances.end() && it->second->serial == t.serial && it->second->program)
            w.RunChain(scene, *it->second, t.node, kScriptResume, t.data);
    }

    // Instances may be added / removed by scripts meanwhile: iterate over a copy of the keys.
    std::vector<std::uint64_t> keys;
    for (const auto& [key, inst] : w.instances)
        keys.push_back(key);
    const auto forEach = [&](const std::function<void(Impl::Instance&)>& fn) {
        for (std::uint64_t key : keys)
            if (const auto it = w.instances.find(key); it != w.instances.end() && it->second->program)
                fn(*it->second);
    };

    // Timers that are due fire their Custom Event (looping ones once per frame at most).
    forEach([&](Impl::Instance& inst) {
        std::vector<std::string> fire;
        std::erase_if(inst.timers, [&](Impl::Timer& t) {
            if (t.next > w.time)
                return false;
            fire.push_back(t.event);
            if (!t.loop)
                return true;
            t.next += t.interval;
            if (t.next <= w.time)
                t.next = w.time + t.interval;
            return false;
        });
        for (const std::string& event : fire)
            w.CallCustomEvent(scene, inst, event);
    });

    // Timelines and tweens.
    forEach([&](Impl::Instance& inst) {
        const std::vector<int> ticking = inst.ticking; // nodes stop / start ticking meanwhile
        for (int node : ticking)
            if (std::ranges::find(inst.ticking, node) != inst.ticking.end())
                w.RunChain(scene, inst, node, kScriptTick, 0);
    });

    // Collisions since the last update (physics steps publish them).
    const std::vector<Impl::QueuedCollision> collisions = std::exchange(w.collisions, {});
    for (const Impl::QueuedCollision& q : collisions) {
        const CollisionEvent& e    = q.event;
        const std::string     type = e.begin ? "Event.CollisionBegin" : "Event.CollisionEnd";
        for (const auto& [self, other] : {std::pair{e.a, e.b}, std::pair{e.b, e.a}}) {
            const auto it = w.instances.find(static_cast<std::uint64_t>(self));
            if (it == w.instances.end())
                continue;
            const Entity otherEntity = other;
            const bool   trigger     = e.trigger;
            w.Fire(scene, *it->second, type, nullptr, [&](const CompiledNode&, std::vector<ScriptValue>& out) {
                out[1] = otherEntity;
                out[2] = trigger;
            });
        }
    }

    // Keys, mouse buttons and Tick.
    if (w.input && acceptInput) {
        const glm::vec2 mouse  = w.input->MousePosition() - w.viewport.origin;
        const bool      inView = mouse.x >= 0.0f && mouse.y >= 0.0f && mouse.x < w.viewport.size.x && mouse.y < w.viewport.size.y;
        forEach([&](Impl::Instance& inst) {
            for (const bool pressed : {true, false}) {
                const std::string type   = pressed ? "Event.KeyPressed" : "Event.KeyReleased";
                const auto        events = inst.program->events.find(type);
                if (events == inst.program->events.end())
                    continue;
                std::vector<std::string> fired;
                for (int node : events->second) {
                    const std::string& name = inst.program->nodes[static_cast<std::size_t>(node)].node->param;
                    const int          key  = KeyFromName(name);
                    const bool edge = key >= 0 && (pressed ? w.input->WasKeyPressed(key) : w.input->WasKeyReleased(key));
                    if (edge && std::ranges::find(fired, name) == fired.end())
                        fired.push_back(name);
                }
                for (const std::string& name : fired)
                    w.Fire(scene, inst, type, &name, {});
            }
            for (const bool pressed : {true, false}) {
                const std::string type = pressed ? "Event.MouseButtonPressed" : "Event.MouseButtonReleased";
                if (!inst.program->events.contains(type) || (pressed && !inView))
                    continue;
                for (const char* button : {"Left", "Right", "Middle"}) {
                    const int  index = std::string_view(button) == "Right" ? 1 : std::string_view(button) == "Middle" ? 2 : 0;
                    const bool edge  = pressed ? w.input->WasMousePressed(index) : w.input->WasMouseReleased(index);
                    if (!edge)
                        continue;
                    const std::string name = button;
                    w.Fire(scene, inst, type, &name, [&](const CompiledNode&, std::vector<ScriptValue>& out) {
                        out[1] = glm::vec3(mouse, 0.0f);
                    });
                }
            }
            // Input actions: pressed when the first bound key goes down (none was held), released
            // when the last one goes up.
            for (const bool pressed : {true, false}) {
                const std::string type   = pressed ? "Event.InputActionPressed" : "Event.InputActionReleased";
                const auto        events = inst.program->events.find(type);
                if (events == inst.program->events.end())
                    continue;
                std::vector<std::string> fired;
                for (int node : events->second) {
                    const std::string&        name   = inst.program->nodes[static_cast<std::size_t>(node)].node->param;
                    const InputActionBinding* action = w.inputMap.FindAction(name);
                    if (!action || std::ranges::find(fired, name) != fired.end())
                        continue;
                    const auto any = [&](KeyQuery q) {
                        return std::ranges::any_of(action->keys, [&](const std::string& k) { return QueryKey(*w.input, k, q); });
                    };
                    const bool heldBefore = std::ranges::any_of(action->keys, [&](const std::string& k) {
                        return QueryKey(*w.input, k, KeyQuery::Down) && !QueryKey(*w.input, k, KeyQuery::Pressed);
                    });
                    if (pressed ? any(KeyQuery::Pressed) && !heldBefore : any(KeyQuery::Released) && !any(KeyQuery::Down))
                        fired.push_back(name);
                }
                for (const std::string& name : fired)
                    w.Fire(scene, inst, type, &name, {});
            }
        });
    }
    forEach([&](Impl::Instance& inst) {
        w.Fire(scene, inst, "Event.Tick", nullptr, [&](const CompiledNode&, std::vector<ScriptValue>& out) { out[1] = dt; });
    });

    std::erase_if(w.messages, [&](const ScriptMessage& m) { return m.time + m.duration < w.time; });
    w.stats.instances = static_cast<std::uint32_t>(w.instances.size());
    w.stats.waiting   = static_cast<std::uint32_t>(w.waiting.size());
    w.stats.timers    = 0;
    for (const auto& [key, inst] : w.instances)
        w.stats.timers += static_cast<std::uint32_t>(inst->timers.size());
    w.stats.queued  = static_cast<std::uint32_t>(w.queued.size());
    w.stats.ticking = 0;
    for (const auto& [key, inst] : w.instances)
        w.stats.ticking += static_cast<std::uint32_t>(inst->ticking.size());
    scene.UpdateTransforms();
}

void ScriptSystem::End(Scene& scene)
{
    Impl& w = *m_Impl;
    if (!w.running)
        return;
    w.paused.reset(); // the debugger lets go: EndPlay runs to completion
    w.queued.clear();
    w.stepMode = Impl::StepMode::None;
    w.breakpoints.clear();
    std::vector<std::uint64_t> keys;
    for (const auto& [key, inst] : w.instances)
        keys.push_back(key);
    const Registry& registry = scene.GetRegistry();
    for (std::uint64_t key : keys)
        if (const auto it = w.instances.find(key);
            it != w.instances.end() && registry.Valid(it->second->entity) && registry.Has<Transform>(it->second->entity)) {
            w.FireSimple(scene, *it->second, "Event.EndPlay");
            if (w.paused) { // a breakpoint inside EndPlay: ignored now
                w.paused.reset();
                w.queued.clear();
            }
        }
    w.Clear();
    w.running = false;
    scene.UpdateTransforms();
}

bool ScriptSystem::Running() const { return m_Impl->running; }

void ScriptSystem::Provide(const std::filesystem::path& file, std::optional<ScriptGraph> graph)
{
    const std::string key = Key(file);
    if (graph)
        m_Impl->overrides[key] = std::move(*graph);
    else
        m_Impl->overrides.erase(key);
    m_Impl->programs.erase(key); // scripts created from now on use it
}

const ScriptDebugInfo* ScriptSystem::Debug(const std::filesystem::path& file) const
{
    const auto it = m_Impl->debug.find(Key(file));
    return it != m_Impl->debug.end() ? &it->second : nullptr;
}

std::span<const ScriptMessage> ScriptSystem::Messages() const { return m_Impl->messages; }
const ScriptStats&             ScriptSystem::Stats() const { return m_Impl->stats; }
double                         ScriptSystem::Time() const { return m_Impl->time; }

void ScriptSystem::SetViewport(const ScriptViewport& viewport) { m_Impl->viewport = viewport; }

// Outside play the compiled programs and breakpoints are not kept: graphs change while editing
// and Begin must start from their saved breakpoints.
bool ScriptSystem::RunConstruction(Scene& scene, Entity entity)
{
    Impl& w = *m_Impl;
    if (!w.running)
        w.programs.clear();
    auto breakpoints = w.breakpoints;
    w.maxSteps       = maxStepsPerEvent;
    const bool ran   = w.Construct(scene, entity);
    if (!w.running) {
        w.programs.clear();
        w.breakpoints = std::move(breakpoints);
    }
    return ran;
}

void ScriptSystem::ResetConstructed(Scene& scene)
{
    Impl&                                     w = *m_Impl;
    std::vector<std::pair<Entity, std::uint64_t>> owners;
    for (const auto& [owner, state] : w.constructionStates)
        if (const Entity e = scene.FindByUuid(owner); e != NullEntity)
            owners.emplace_back(e, owner);
    for (const auto& [e, owner] : owners) {
        w.DestroyConstructed(scene, owner);
        w.RestoreOwner(scene, e, owner);
    }
    w.constructionStates.clear();
    scene.UpdateTransforms();
}

std::size_t ScriptSystem::RunAllConstruction(Scene& scene)
{
    Impl& w = *m_Impl;
    if (!w.running)
        w.programs.clear();
    auto breakpoints = w.breakpoints;
    w.maxSteps       = maxStepsPerEvent;
    const std::size_t count = w.ConstructAll(scene);
    if (!w.running) {
        w.programs.clear();
        w.breakpoints = std::move(breakpoints);
    }
    return count;
}
void ScriptSystem::SetInputMap(InputMap map) { m_Impl->inputMap = std::move(map); }
void ScriptSystem::SetSaveDirectory(std::filesystem::path directory)
{
    m_Impl->saveDirectory = std::move(directory);
    m_Impl->saves.clear();
}
std::optional<ScriptLevelRequest> ScriptSystem::TakeLevelRequest() { return std::exchange(m_Impl->levelRequest, std::nullopt); }
void ScriptSystem::SetCurrentLevel(std::string scene) { m_Impl->currentLevel = std::move(scene); }

void ScriptSystem::SetBreakpoints(const std::filesystem::path& file, std::vector<std::uint32_t> nodes)
{
    std::vector<ScriptBreakpoint> list;
    for (std::uint32_t n : nodes)
        list.push_back({n, {}});
    SetBreakpointList(file, list);
}

void ScriptSystem::SetBreakpointList(const std::filesystem::path& file, const std::vector<ScriptBreakpoint>& list)
{
    auto& map = m_Impl->breakpoints[Key(file)];
    std::unordered_map<std::uint32_t, Impl::Breakpoint> next;
    for (const ScriptBreakpoint& b : list) { // hits count on while the options stay
        const auto old = map.find(b.node);
        next[b.node]   = {b.options, old != map.end() && old->second.options == b.options ? old->second.hits : 0};
    }
    map = std::move(next);
}

bool ScriptSystem::DebugPaused() const { return m_Impl->paused.has_value(); }

std::optional<ScriptDebugFrame> ScriptSystem::PausedAt() const
{
    const Impl& w = *m_Impl;
    if (!w.paused)
        return std::nullopt;
    const auto it = w.instances.find(w.paused->entity);
    if (it == w.instances.end() || !it->second->program)
        return std::nullopt;
    const Program&      p     = *it->second->program;
    const auto          frame = [&](int node) {
        const CompiledNode& c = p.nodes[static_cast<std::size_t>(node)];
        return ScriptDebugFrame{p.files[static_cast<std::size_t>(c.file)], c.id, c.function, it->second->entity, {}};
    };
    ScriptDebugFrame result = frame(w.paused->state.node);
    if (w.paused->state.atUse) { // stopped on a Macro node: it is what the graph shows
        const CompiledNode& c = p.nodes[static_cast<std::size_t>(w.paused->state.node)];
        result.file           = p.key;
        result.node           = c.use;
        result.function       = c.node->function;
    }
    for (const Impl::CallFrame& call : w.paused->state.calls)
        if (call.callNode >= 0)
            result.callers.push_back(frame(call.callNode));
    return result;
}

void ScriptSystem::DebugContinue(Scene& scene)
{
    Impl& w = *m_Impl;
    if (!w.paused)
        return;
    w.stepMode = Impl::StepMode::None;
    w.ResumeChains(scene);
    scene.UpdateTransforms();
}

void ScriptSystem::DebugStep(Scene& scene)
{
    Impl& w = *m_Impl;
    if (!w.paused)
        return;
    w.stepMode = Impl::StepMode::Into; // the next node that runs stops again (also in a later frame)
    w.ResumeChains(scene);
    scene.UpdateTransforms();
}

void ScriptSystem::DebugStepOver(Scene& scene)
{
    Impl& w = *m_Impl;
    if (!w.paused)
        return;
    w.stepMode  = Impl::StepMode::Over;
    w.stepDepth = w.paused->state.calls.size();
    w.stepChain = w.paused->state.id;
    w.ResumeChains(scene);
    scene.UpdateTransforms();
}

void ScriptSystem::DebugStepOut(Scene& scene)
{
    Impl& w = *m_Impl;
    if (!w.paused)
        return;
    w.stepMode  = Impl::StepMode::Out;
    w.stepDepth = w.paused->state.calls.size();
    w.stepChain = w.paused->state.id;
    w.ResumeChains(scene);
    scene.UpdateTransforms();
}

std::vector<Entity> ScriptSystem::InstancesOf(const std::filesystem::path& file) const
{
    const std::string   key = Key(file);
    std::vector<Entity> out;
    for (const auto& [k, inst] : m_Impl->instances)
        if (inst->program && inst->program->key == key)
            out.push_back(inst->entity);
    std::ranges::sort(out, {}, [](Entity e) { return static_cast<std::uint64_t>(e); });
    return out;
}

std::optional<ScriptWatch> ScriptSystem::Watch(const std::filesystem::path& file, Entity entity) const
{
    const Impl& w  = *m_Impl;
    const auto  it = w.instances.find(EntityKey(entity));
    if (it == w.instances.end() || !it->second->program || it->second->program->key != Key(file))
        return std::nullopt;
    const Impl::Instance& inst = *it->second;
    const Program&        p    = *inst.program;
    ScriptWatch           watch;
    watch.entity = entity;
    for (std::size_t i = 0; i < p.graph.variables.size(); ++i)
        watch.variables.emplace_back(p.graph.variables[i].name, inst.variables[i]);
    if (w.paused && w.paused->entity == EntityKey(entity)) {
        const int scope = p.nodes[static_cast<std::size_t>(w.paused->state.node)].scope;
        if (scope >= 0) {
            const CompiledFunction& f = p.functions[static_cast<std::size_t>(scope)];
            watch.function            = f.name;
            for (std::size_t i = 0; i < f.locals.size(); ++i)
                watch.locals.emplace_back(f.locals[i].name, inst.locals[static_cast<std::size_t>(scope)][i]);
        }
    }
    for (std::size_t n = 0; n < p.nodes.size(); ++n) {
        const CompiledNode& c = p.nodes[n];
        if (c.file != 0) // copied from a library
            continue;
        for (std::size_t pin = 0; pin < c.pins.size(); ++pin)
            if (c.pins[pin].output && c.pins[pin].type != PinType::Exec)
                watch.pins[{c.id, c.pins[pin].name}] = inst.outputs[n][pin];
    }
    return watch;
}

} // namespace Engine
