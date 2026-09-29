#include "Engine/Script/ScriptSystem.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Input.h"
#include "Engine/Core/Log.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Script/ScriptNodes.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace Engine {

namespace {

std::uint64_t EntityKey(Entity e) { return static_cast<std::uint64_t>(e); }

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
};

struct Program {
    std::string                          key;
    ScriptGraph                          graph;
    std::vector<CompiledNode>            nodes;
    std::vector<ScriptDiagnostic>        diagnostics;
    bool                                 ok = false;
    std::unordered_map<std::string, int> variableIndex;
    std::map<std::string, std::vector<int>> events; // node type -> event nodes
};

std::shared_ptr<Program> Compile(std::string key, ScriptGraph graph, std::vector<ScriptDiagnostic> diagnostics)
{
    auto p         = std::make_shared<Program>();
    p->key         = std::move(key);
    p->graph       = std::move(graph);
    p->diagnostics = std::move(diagnostics);
    p->ok = std::ranges::none_of(p->diagnostics, [](const ScriptDiagnostic& d) { return d.error; });
    if (!p->ok)
        return p;

    std::unordered_map<std::uint32_t, int> index;
    for (const ScriptNode& n : p->graph.nodes) {
        index[n.id] = static_cast<int>(p->nodes.size());
        CompiledNode c;
        c.node = &n;
        c.desc = FindScriptNodeType(n.type);
        c.pins = NodePins(p->graph, n);
        c.source.resize(c.pins.size());
        c.target.resize(c.pins.size());
        c.defaults.resize(c.pins.size(), false);
        for (std::size_t i = 0; i < c.pins.size(); ++i)
            if (!c.pins[i].output && c.pins[i].type != PinType::Exec)
                c.defaults[i] = PinDefault(n, c.desc, c.pins[i]);
        if (c.desc->kind == NodeKind::Event)
            p->events[n.type].push_back(index[n.id]);
        p->nodes.push_back(std::move(c));
    }
    const auto pinIndex = [](const CompiledNode& c, const std::string& name, bool output) {
        for (std::size_t i = 0; i < c.pins.size(); ++i)
            if (c.pins[i].output == output && c.pins[i].name == name)
                return static_cast<int>(i);
        return -1;
    };
    for (const ScriptLink& l : p->graph.links) {
        const int from = index.at(l.fromNode), to = index.at(l.toNode);
        const int out = pinIndex(p->nodes[from], l.fromPin, true), in = pinIndex(p->nodes[to], l.toPin, false);
        if (out < 0 || in < 0)
            continue; // validated before
        if (p->nodes[from].pins[out].type == PinType::Exec)
            p->nodes[from].target[out] = {to, in};
        else
            p->nodes[to].source[in] = {from, out};
    }
    for (std::size_t i = 0; i < p->graph.variables.size(); ++i)
        p->variableIndex[p->graph.variables[i].name] = static_cast<int>(i);
    return p;
}

} // namespace

struct ScriptSystem::Impl {
    struct Instance {
        Entity                                 entity = NullEntity;
        std::uint64_t                          serial = 0; // distinguishes re-created instances
        std::shared_ptr<const Program>         program;
        std::vector<ScriptValue>               variables;
        std::vector<std::vector<ScriptValue>>  outputs; // per node, per pin
        std::vector<ScriptContext::NodeState>  states;
        std::unordered_set<int>                reported; // nodes whose runtime error was logged
        std::string                            graph;    // ScriptComponent::graph it was made for
    };
    struct Continuation {
        int          node = 0;
        std::int32_t data = 0;
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

    // The context node implementations see: one per running chain / evaluation.
    class Exec final : public ScriptContext {
    public:
        Exec(Impl& impl, Scene& scene, Instance& instance) : m_Impl(impl), m_Scene(scene), m_Instance(instance) {}

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
            if (from.desc->kind == NodeKind::Pure) {
                if (depth > 256) {
                    Error("Pure evaluation too deep");
                    return DefaultValue(c.pins[static_cast<std::size_t>(pin)].type);
                }
                const int saved = node;
                node            = src.node;
                ++depth;
                from.desc->evaluate(*this);
                --depth;
                node = saved;
            }
            return Convert(m_Instance.outputs[static_cast<std::size_t>(src.node)][static_cast<std::size_t>(src.pin)],
                           c.pins[static_cast<std::size_t>(pin)].type);
        }
        void Out(int pin, ScriptValue value) override
        {
            m_Instance.outputs[static_cast<std::size_t>(node)][static_cast<std::size_t>(pin)] = std::move(value);
        }
        bool Connected(int pin) const override { return Node().source[static_cast<std::size_t>(pin)].node >= 0; }
        const std::string& Param() const override { return Node().node->param; }
        NodeState&         State() override { return m_Instance.states[static_cast<std::size_t>(node)]; }
        Entity             Self() const override { return m_Instance.entity; }
        Scene&             GetScene() override { return m_Scene; }
        double             Time() const override { return m_Impl.time; }
        PhysicsWorld*      Physics() override { return m_Impl.physics; }
        AssetManager*      Assets() override { return m_Impl.assets; }
        const Input*       GetInput() override { return m_Impl.acceptInput ? m_Impl.input : nullptr; }
        ScriptValue*       Variable(const std::string& name) override
        {
            const auto it = m_Instance.program->variableIndex.find(name);
            return it != m_Instance.program->variableIndex.end() ? &m_Instance.variables[static_cast<std::size_t>(it->second)]
                                                                 : nullptr;
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
        void         CallEvent(const std::string& name) override { m_Impl.CallCustomEvent(m_Scene, m_Instance, name); }
        void         KeepModel(std::uint32_t index, std::uint32_t generation) override
        {
            m_Impl.spawned.push_back(ModelHandle{index, generation});
        }

    private:
        Impl&     m_Impl;
        Scene&    m_Scene;
        Instance& m_Instance;
    };

    Impl(EventBus& bus, const Input* in, PhysicsWorld* phys, AssetManager* am)
        : events(bus), input(in), physics(phys), assets(am)
    {
        collisionSub = events.Subscribe<CollisionEvent>([this](const CollisionEvent& e) {
            if (running)
                collisions.push_back({e});
            return false;
        });
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
        auto program = Compile(key, std::move(graph), diagnostics);
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

    Instance* CreateInstance(Entity e, const std::string& graph)
    {
        auto program = ProgramFor(graph);
        auto inst    = std::make_unique<Instance>();
        inst->entity = e;
        inst->serial = ++nextSerial;
        inst->graph  = graph;
        inst->program = program->ok ? program : nullptr;
        if (inst->program) {
            for (const ScriptVariable& v : program->graph.variables)
                inst->variables.push_back(v.value);
            inst->outputs.resize(program->nodes.size());
            for (std::size_t i = 0; i < program->nodes.size(); ++i) {
                const CompiledNode& c = program->nodes[i];
                inst->outputs[i].resize(c.pins.size(), false);
                for (std::size_t p = 0; p < c.pins.size(); ++p)
                    if (c.pins[p].output && c.pins[p].type != PinType::Exec)
                        inst->outputs[i][p] = DefaultValue(c.pins[p].type);
            }
            inst->states.resize(program->nodes.size());
        }
        Instance* raw = inst.get();
        instances[EntityKey(e)] = std::move(inst);
        return raw;
    }

    // Adds / removes instances to match the scene's ScriptComponents. Decides first, then fires
    // (scripts may create / destroy entities, which must not happen while the view iterates).
    void SyncInstances(Scene& scene)
    {
        Registry&                                       registry = scene.GetRegistry();
        std::unordered_set<std::uint64_t>               seen;
        std::vector<std::pair<Entity, std::string>>     added;
        std::vector<std::uint64_t>                      replaced;
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
                FireSimple(scene, *CreateInstance(e, graph), "Event.BeginPlay");
    }

    // --- Execution ---

    void MarkNode(const Instance& inst, int node)
    {
        debug[inst.program->key].nodeTimes[inst.program->nodes[static_cast<std::size_t>(node)].node->id] = time;
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
        debug[inst.program->key].linkTimes[{c.node->id, c.pins[static_cast<std::size_t>(pin)].name}] = time;
        nextNode  = t.node;
        nextEntry = t.pin;
        return true;
    }

    // Runs a chain starting at (node, entry) until it ends; continuations pushed on the way run after.
    void RunChain(Scene& scene, Instance& inst, int node, int entry, std::int32_t data)
    {
        // Instances are only created / dropped in SyncInstances and Clear, never while a chain runs.
        const std::uint64_t       key = EntityKey(inst.entity), serial = inst.serial;
        std::vector<Continuation> stack;
        std::uint32_t             steps = 0;
        for (;;) {
            if (++steps > maxSteps) {
                RuntimeError(inst, node, "Too many steps in one event (infinite loop?) - chain aborted");
                return;
            }
            ++stats.nodesExecuted;
            MarkNode(inst, node);
            Exec ctx(*this, scene, inst);
            ctx.node       = node;
            ctx.resumeData = data;
            const CompiledNode& c = inst.program->nodes[static_cast<std::size_t>(node)];
            const int out = c.desc->execute ? c.desc->execute(ctx, entry) : kScriptStop;
            // Continuations pushed now run after the chain that starts here: LIFO, pushed last = next.
            for (auto it = ctx.pushed.rbegin(); it != ctx.pushed.rend(); ++it)
                stack.push_back(*it);
            if (ctx.suspended)
                waiting.push_back({key, serial, node, ctx.suspendData, time + ctx.suspendSeconds});
            int nextNode = 0, nextEntry = 0;
            if (out >= 0 && Follow(inst, node, out, nextNode, nextEntry)) {
                node  = nextNode;
                entry = nextEntry;
                data  = 0;
                continue;
            }
            if (stack.empty())
                return;
            node  = stack.back().node;
            data  = stack.back().data;
            entry = kScriptResume;
            stack.pop_back();
        }
    }

    // Fires all event nodes of `type` (matching param if given); `outputs` fills their output pins.
    void Fire(Scene& scene, Instance& inst, const std::string& type, const std::string* param,
              const std::function<void(std::vector<ScriptValue>&)>& outputs)
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
                outputs(inst.outputs[static_cast<std::size_t>(node)]);
            int next = 0, entry = 0;
            if (Follow(inst, node, 0, next, entry))
                RunChain(scene, inst, next, entry, 0);
        }
    }

    void FireSimple(Scene& scene, Instance& inst, const std::string& type) { Fire(scene, inst, type, nullptr, {}); }

    void CallCustomEvent(Scene& scene, Instance& inst, const std::string& name)
    {
        if (callDepth >= 32) {
            PrintMessage("Custom event recursion too deep: " + name, 3.0f, true);
            return;
        }
        ++callDepth;
        Fire(scene, inst, "Event.Custom", &name, {});
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
        const std::uint32_t id = inst.program->nodes[static_cast<std::size_t>(node)].node->id;
        debug[inst.program->key].diagnostics.push_back({id, message, true});
        PrintMessage(std::filesystem::path(inst.program->key).filename().string() + ": " +
                         inst.program->nodes[static_cast<std::size_t>(node)].desc->title + ": " + message,
                     4.0f, true);
    }

    void Clear()
    {
        instances.clear();
        waiting.clear();
        collisions.clear();
        if (assets)
            for (ModelHandle h : spawned)
                assets->Release(h);
        spawned.clear();
    }

    EventBus&     events;
    const Input*  input;
    PhysicsWorld* physics;
    AssetManager* assets;
    Subscription  collisionSub;

    bool          running     = false;
    bool          acceptInput = true;
    double        time        = 0.0;
    std::uint32_t maxSteps    = 100000;
    int           callDepth   = 0;
    std::uint64_t nextSerial  = 0;

    std::unordered_map<std::string, std::shared_ptr<const Program>> programs; // by key, cleared at Begin
    std::unordered_map<std::string, ScriptGraph>                    overrides;
    std::unordered_map<std::string, ScriptDebugInfo>                debug;
    std::unordered_map<std::uint64_t, std::unique_ptr<Instance>>    instances; // by entity
    std::vector<Waiting>                                            waiting;
    std::vector<QueuedCollision>                                    collisions;
    std::vector<ModelHandle>                                        spawned;
    std::vector<ScriptMessage>                                      messages;
    ScriptStats                                                     stats;
};

ScriptSystem::ScriptSystem(EventBus& events, const Input* input, PhysicsWorld* physics, AssetManager* assets)
    : m_Impl(std::make_unique<Impl>(events, input, physics, assets))
{
}

ScriptSystem::~ScriptSystem() { m_Impl->Clear(); }

std::string ScriptSystem::Key(const std::filesystem::path& file)
{
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
    w.SyncInstances(scene);
    w.stats.instances = static_cast<std::uint32_t>(w.instances.size());
    scene.UpdateTransforms();
}

void ScriptSystem::Update(Scene& scene, float dt, bool acceptInput)
{
    Impl& w = *m_Impl;
    if (!w.running)
        return;
    w.maxSteps            = maxStepsPerEvent;
    w.acceptInput         = acceptInput;
    w.stats.nodesExecuted = 0;
    w.stats.eventsFired   = 0;
    w.time += std::max(dt, 0.0f);
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
            w.Fire(scene, *it->second, type, nullptr, [&](std::vector<ScriptValue>& out) {
                out[1] = otherEntity;
                out[2] = trigger;
            });
        }
    }

    // Keys and Tick. Instances may be added / removed by scripts meanwhile: iterate over a copy.
    std::vector<std::uint64_t> keys;
    for (const auto& [key, inst] : w.instances)
        keys.push_back(key);
    const auto forEach = [&](const std::function<void(Impl::Instance&)>& fn) {
        for (std::uint64_t key : keys)
            if (const auto it = w.instances.find(key); it != w.instances.end() && it->second->program)
                fn(*it->second);
    };
    if (w.input && acceptInput)
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
        });
    forEach([&](Impl::Instance& inst) {
        w.Fire(scene, inst, "Event.Tick", nullptr, [&](std::vector<ScriptValue>& out) { out[1] = dt; });
    });

    std::erase_if(w.messages, [&](const ScriptMessage& m) { return m.time + m.duration < w.time; });
    w.stats.instances = static_cast<std::uint32_t>(w.instances.size());
    w.stats.waiting   = static_cast<std::uint32_t>(w.waiting.size());
    scene.UpdateTransforms();
}

void ScriptSystem::End(Scene& scene)
{
    Impl& w = *m_Impl;
    if (!w.running)
        return;
    std::vector<std::uint64_t> keys;
    for (const auto& [key, inst] : w.instances)
        keys.push_back(key);
    const Registry& registry = scene.GetRegistry();
    for (std::uint64_t key : keys)
        if (const auto it = w.instances.find(key);
            it != w.instances.end() && registry.Valid(it->second->entity) && registry.Has<Transform>(it->second->entity))
            w.FireSimple(scene, *it->second, "Event.EndPlay");
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

} // namespace Engine
