#include "Test.h"

#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptNodes.h"
#include "Engine/Script/ScriptSystem.h"
#include "Engine/UI/UiLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace Engine;

namespace {

// A graph under construction plus a scene entity running it.
struct ScriptFixture {
    EventBus     bus;
    Scene        scene;
    ScriptSystem scripts{bus};
    ScriptGraph  graph;
    Entity       actor = scene.CreateEntity("Actor");

    std::uint32_t Node(const char* type, std::string param = {}) { return graph.AddNode(type, {}, std::move(param)); }
    void Link(std::uint32_t from, const char* fromPin, std::uint32_t to, const char* toPin)
    {
        const std::string error = graph.Connect(from, fromPin, to, toPin);
        CHECK(error.empty());
        if (!error.empty())
            std::printf("  connect %s -> %s: %s\n", fromPin, toPin, error.c_str());
    }
    void Set(std::uint32_t node, const char* pin, ScriptValue value) { graph.FindNode(node)->defaults[pin] = std::move(value); }
    std::uint32_t Print(std::string text)
    {
        const std::uint32_t n = Node("Debug.Print");
        Set(n, "Text", std::move(text));
        Set(n, "Duration", 1000.0f);
        return n;
    }
    void Start(const char* file = "test.ugraph")
    {
        scene.GetRegistry().Emplace<ScriptComponent>(actor, ScriptComponent{file});
        scripts.Provide(file, graph);
        scripts.Begin(scene);
    }
    void Run(float seconds)
    {
        for (int i = 0, n = static_cast<int>(std::round(seconds * 60.0f)); i < n; ++i)
            scripts.Update(scene, 1.0f / 60.0f);
    }
    [[nodiscard]] std::size_t Printed(const std::string& text) const
    {
        const auto messages = scripts.Messages();
        return static_cast<std::size_t>(std::count_if(messages.begin(), messages.end(),
                                                      [&](const ScriptMessage& m) { return m.text == text; }));
    }
    [[nodiscard]] bool Errors() const
    {
        const auto messages = scripts.Messages();
        return std::ranges::any_of(messages, [](const ScriptMessage& m) { return m.error; });
    }
};

} // namespace

TEST_CASE(Script_GraphEditingValidationAndJson)
{
    ScriptGraph g;
    g.variables.push_back({"Health", PinType::Float, 100.0f});
    const std::uint32_t begin  = g.AddNode("Event.BeginPlay", {0.0f, 0.0f});
    const std::uint32_t branch = g.AddNode("Flow.Branch", {200.0f, 0.0f});
    const std::uint32_t less   = g.AddNode("Compare.LessFloat", {100.0f, 100.0f});
    const std::uint32_t get    = g.AddNode("Variable.Get", {0.0f, 100.0f}, "Health");
    const std::uint32_t count  = g.AddNode("Math.AddInt", {0.0f, 200.0f});
    CHECK(g.Connect(begin, "Out", branch, "In").empty());
    CHECK(g.Connect(get, "Value", less, "A").empty());
    CHECK(g.Connect(less, "Result", branch, "Condition").empty());
    CHECK(g.Connect(count, "Result", less, "B").empty());   // int -> float: implicit
    CHECK(!g.Connect(begin, "Out", less, "A").empty());       // exec -> data
    CHECK(!g.Connect(get, "Value", branch, "In").empty());    // data -> exec
    CHECK(!g.Connect(branch, "True", branch, "In").empty());  // self
    CHECK(!g.Connect(branch, "In", less, "A").empty());       // input -> input
    CHECK(g.links.size() == 4);
    // Single-link pins: a new source replaces the old one.
    const std::uint32_t get2 = g.AddNode("Variable.Get", {}, "Health");
    CHECK(g.Connect(get2, "Value", less, "A").empty());
    CHECK(std::ranges::count_if(g.links, [&](const ScriptLink& l) { return l.toNode == less && l.toPin == "A"; }) == 1);
    CHECK(std::ranges::none_of(ValidateScriptGraph(g), [](const ScriptDiagnostic& d) { return d.error; }));

    // Variables: rename follows, a type change drops incompatible links.
    CHECK(g.RenameVariable("Health", "Hp") && g.FindNode(get)->param == "Hp");
    g.SetVariableType("Hp", PinType::Vec3);
    CHECK(std::ranges::none_of(g.links, [&](const ScriptLink& l) { return l.fromNode == get || l.fromNode == get2; }));

    // JSON round trip (stable), defaults, comments.
    g.FindNode(branch)->defaults["Condition"] = true;
    g.AddComment({-10.0f, -10.0f}, {400.0f, 300.0f}, "Start");
    const std::string json  = ScriptGraphToJson(g);
    const ScriptGraph again = ScriptGraphFromJson(json);
    CHECK(ScriptGraphToJson(again) == json);
    CHECK(again.nodes.size() == g.nodes.size() && again.links == g.links && again.comments.size() == 1);
    CHECK(std::get<bool>(again.FindNode(branch)->defaults.at("Condition")));
    CHECK(again.nextId == g.nextId);

    // Validation: unknown type, unknown variable, pure cycle.
    ScriptGraph bad;
    bad.AddNode("Nope.Nothing", {});
    bad.AddNode("Variable.Get", {}, "Missing");
    const std::uint32_t a = bad.AddNode("Math.AddFloat", {});
    const std::uint32_t b = bad.AddNode("Math.AddFloat", {});
    CHECK(bad.Connect(a, "Result", b, "A").empty() && bad.Connect(b, "Result", a, "A").empty());
    const auto diagnostics = ValidateScriptGraph(bad);
    const auto has = [&](const char* text) {
        return std::ranges::any_of(diagnostics, [&](const ScriptDiagnostic& d) { return d.error && d.message.find(text) != std::string::npos; });
    };
    CHECK(has("Unknown node type") && has("Unknown variable") && has("Cycle"));
    bool threw = false;
    try {
        (void)ScriptGraphFromJson("{ broken");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(KeyFromName("Space") == 32 && KeyFromName("A") == 65 && KeyFromName("F5") == 294 && KeyFromName("??") < 0);
}

TEST_CASE(Script_FlowControlVariablesAndMath)
{
    ScriptFixture f;
    f.graph.variables.push_back({"Sum", PinType::Int, std::int32_t{0}});
    // BeginPlay -> Sequence: [0] ForLoop 1..4 adds Index to Sum, [1] Branch(Sum == 10) prints.
    const std::uint32_t begin = f.Node("Event.BeginPlay");
    const std::uint32_t seq   = f.Node("Flow.Sequence", "3");
    const std::uint32_t loop  = f.Node("Flow.ForLoop");
    f.Set(loop, "First Index", std::int32_t{1});
    f.Set(loop, "Last Index", std::int32_t{4});
    const std::uint32_t get = f.Node("Variable.Get", "Sum");
    const std::uint32_t add = f.Node("Math.AddInt");
    const std::uint32_t set = f.Node("Variable.Set", "Sum");
    f.Link(begin, "Out", seq, "In");
    f.Link(seq, "Then 0", loop, "In");
    f.Link(loop, "Loop Body", set, "In");
    f.Link(get, "Value", add, "A");
    f.Link(loop, "Index", add, "B");
    f.Link(add, "Result", set, "Value");
    const std::uint32_t equal  = f.Node("Compare.EqualInt");
    const std::uint32_t branch = f.Node("Flow.Branch");
    f.Set(equal, "B", std::int32_t{10});
    f.Link(get, "Value", equal, "A");
    f.Link(equal, "Result", branch, "Condition");
    f.Link(seq, "Then 1", branch, "In");
    f.Link(branch, "True", f.Print("sum ok"), "In");
    f.Link(branch, "False", f.Print("sum wrong"), "In");
    // [2] DoOnce twice through a second Sequence + FlipFlop via Tick.
    const std::uint32_t seq2 = f.Node("Flow.Sequence");
    const std::uint32_t once = f.Node("Flow.DoOnce");
    f.Link(seq, "Then 2", seq2, "In");
    f.Link(seq2, "Then 0", once, "In");
    f.Link(seq2, "Then 1", once, "In");
    const std::uint32_t printOnce = f.Print("once");
    f.Link(once, "Completed", printOnce, "In");
    // Tick -> FlipFlop -> A: print "flip" / B: print "flop".
    const std::uint32_t tick = f.Node("Event.Tick");
    const std::uint32_t flip = f.Node("Flow.FlipFlop");
    f.Link(tick, "Out", flip, "In");
    f.Link(flip, "A", f.Print("flip"), "In");
    f.Link(flip, "B", f.Print("flop"), "In");
    // Pure math chain: Append(ToString(2.5 * 4), "!") printed at BeginPlay via Custom Event.
    const std::uint32_t call   = f.Node("Flow.CallEvent", "Report");
    const std::uint32_t custom = f.Node("Event.Custom", "Report");
    const std::uint32_t mul    = f.Node("Math.MultiplyFloat");
    const std::uint32_t append = f.Node("String.Append");
    const std::uint32_t print  = f.Print("");
    f.Set(mul, "A", 2.5f);
    f.Set(mul, "B", 4.0f);
    f.Set(append, "B", std::string("!"));
    f.Link(mul, "Result", append, "A"); // float -> string: implicit
    f.Link(append, "Result", print, "Text");
    f.Link(custom, "Out", print, "In");
    f.Link(printOnce, "Then", call, "In");

    CHECK(std::ranges::none_of(ValidateScriptGraph(f.graph), [](const ScriptDiagnostic& d) { return d.error; }));
    f.Start();
    CHECK(f.Printed("sum ok") == 1 && f.Printed("sum wrong") == 0);
    CHECK(f.Printed("once") == 1);
    CHECK(f.Printed("10!") == 1);
    f.Run(4.0f / 60.0f); // 4 ticks: flip, flop, flip, flop
    CHECK(f.Printed("flip") == 2 && f.Printed("flop") == 2);
    CHECK(!f.Errors());
    const ScriptDebugInfo* debug = f.scripts.Debug("test.ugraph");
    CHECK(debug && debug->nodeTimes.contains(loop) && debug->linkTimes.contains({seq, "Then 1"}));
    f.scripts.End(f.scene);
    CHECK(!f.scripts.Running() && f.scripts.Stats().instances == 1);
}

TEST_CASE(Script_DelayTransformAndLoopGuard)
{
    ScriptFixture f;
    // Tick: move +X by 2 m/s (world), BeginPlay: Delay 0.5 -> print, While(true) -> aborted.
    const std::uint32_t tick   = f.Node("Event.Tick");
    const std::uint32_t offset = f.Node("Entity.AddOffset");
    const std::uint32_t scale  = f.Node("Vector.Scale");
    f.Set(scale, "Vector", glm::vec3(2.0f, 0.0f, 0.0f));
    f.Link(tick, "Delta Seconds", scale, "Scale");
    f.Link(scale, "Result", offset, "Offset");
    f.Link(tick, "Out", offset, "In");

    const std::uint32_t begin = f.Node("Event.BeginPlay");
    const std::uint32_t seq   = f.Node("Flow.Sequence");
    const std::uint32_t delay = f.Node("Flow.Delay");
    f.Set(delay, "Duration", 0.5f);
    f.Link(begin, "Out", seq, "In");
    f.Link(seq, "Then 0", delay, "In");
    f.Link(delay, "Completed", f.Print("late"), "In");
    f.graph.variables.push_back({"Spins", PinType::Int, std::int32_t{0}});
    const std::uint32_t loop = f.Node("Flow.WhileLoop");
    const std::uint32_t get  = f.Node("Variable.Get", "Spins");
    const std::uint32_t inc  = f.Node("Math.AddInt");
    const std::uint32_t set  = f.Node("Variable.Set", "Spins");
    f.Set(loop, "Condition", true);
    f.Set(inc, "B", std::int32_t{1});
    f.Link(seq, "Then 1", loop, "In");
    f.Link(loop, "Loop Body", set, "In");
    f.Link(get, "Value", inc, "A");
    f.Link(inc, "Result", set, "Value");
    f.scripts.maxStepsPerEvent = 5000;

    const Entity parent = f.scene.CreateEntity("Parent");
    f.scene.EditTransform(parent).position = {0.0f, 0.0f, 10.0f};
    f.scene.EditTransform(parent).rotation = glm::angleAxis(1.0f, glm::vec3(0.0f, 1.0f, 0.0f));
    f.scene.SetParent(f.actor, parent);
    f.Start();
    CHECK(f.Printed("late") == 0 && f.Errors()); // the while loop was aborted, the delay still waits
    f.Run(0.45f);
    CHECK(f.Printed("late") == 0);
    f.Run(0.1f);
    CHECK(f.Printed("late") == 1);
    f.Run(0.45f); // 1 s in total
    const glm::vec3 world = f.scene.GetRegistry().Get<WorldTransform>(f.actor).matrix[3];
    CHECK(std::abs(world.x - 2.0f) < 0.05f && std::abs(world.z - 10.0f) < 1e-3f); // world +X under a rotated parent
    CHECK(f.scene.CountStaleTransforms() == 0);
    f.scripts.End(f.scene);
}

TEST_CASE(Script_UiClickDispatchesCustomEvent)
{
    ScriptFixture f;
    const std::uint32_t clicked = f.Node("Event.UIClicked");
    const std::uint32_t print = f.Print("button clicked");
    f.Link(clicked, "Out", print, "In");
    const std::uint32_t changed = f.Node("Event.UIValueChanged");
    const std::uint32_t printValue = f.Print("");
    f.Link(changed, "Out", printValue, "In");
    f.Link(changed, "Value", printValue, "Text");
    const std::uint32_t checked = f.Node("Event.UICheckedChanged");
    const std::uint32_t branch = f.Node("Flow.Branch");
    f.Link(checked, "Out", branch, "In");
    f.Link(checked, "Checked", branch, "Condition");
    f.Link(branch, "True", f.Print("checked"), "In");
    f.Link(branch, "False", f.Print("unchecked"), "In");
    f.Start("ui.ugraph");
    f.scripts.DispatchUiEvent(f.scene, UiEvent{.entity = f.actor, .type = UiEventType::Clicked});
    f.scripts.DispatchUiEvent(f.scene, UiEvent{.entity = f.actor, .type = UiEventType::ValueChanged, .value = 0.75f});
    f.scripts.DispatchUiEvent(f.scene, UiEvent{.entity = f.actor, .type = UiEventType::CheckedChanged, .checked = true});
    CHECK(f.Printed("button clicked") == 1);
    CHECK(f.Printed("0.75") == 1);
    CHECK(f.Printed("checked") == 1 && f.Printed("unchecked") == 0);
    CHECK(!f.Errors());
}

TEST_CASE(Script_AnimationControls)
{
    ScriptFixture f;
    f.scene.GetRegistry().Emplace<Animator>(f.actor);
    f.scene.GetRegistry().Emplace<ModelInstance>(f.actor);

    const std::uint32_t begin = f.Node("Event.BeginPlay");
    const std::uint32_t clip = f.Node("Animation.SetClip");
    const std::uint32_t speed = f.Node("Animation.SetSpeed");
    const std::uint32_t looping = f.Node("Animation.SetLooping");
    const std::uint32_t blend = f.Node("Animation.BlendTo");
    const std::uint32_t rootMotion = f.Node("Animation.SetRootMotion");
    const std::uint32_t play = f.Node("Animation.Play");
    const std::uint32_t branch = f.Node("Flow.Branch");
    const std::uint32_t playing = f.Node("Animation.IsPlaying");
    const std::uint32_t print = f.Print("animation started");
    const std::uint32_t stop = f.Node("Animation.Stop");
    f.Set(clip, "Clip", std::int32_t{2});
    f.Set(speed, "Speed", 1.5f);
    f.Set(looping, "Loop", false);
    f.Set(blend, "Clip", std::int32_t{4});
    f.Set(blend, "Weight", 0.25f);
    f.Set(rootMotion, "Enabled", true);
    f.Link(begin, "Out", clip, "In");
    f.Link(clip, "Then", speed, "In");
    f.Link(speed, "Then", looping, "In");
    f.Link(looping, "Then", blend, "In");
    f.Link(blend, "Then", rootMotion, "In");
    f.Link(rootMotion, "Then", play, "In");
    f.Link(play, "Then", branch, "In");
    f.Link(playing, "Playing", branch, "Condition");
    f.Link(branch, "True", print, "In");
    f.Link(print, "Then", stop, "In");

    f.Start("animation.ugraph");
    const Animator& animator = f.scene.GetRegistry().Get<Animator>(f.actor);
    CHECK(animator.clipIndex == 2);
    CHECK(animator.speed == 1.5f);
    CHECK(!animator.looping);
    CHECK(animator.blendClipIndex == 4);
    CHECK(animator.blendWeight == 0.25f);
    CHECK(animator.applyRootMotion);
    CHECK(!animator.playing); // the IsPlaying pure pin saw Play=true; Stop then ran
    CHECK(f.Printed("animation started") == 1);
    CHECK(!f.Errors());
    f.scripts.End(f.scene);
}

TEST_CASE(Script_CollisionEventsAndImpulse)
{
    ThreadPool   pool{2};
    ScriptFixture f;
    PhysicsWorld physics{pool, f.bus};
    ScriptSystem scripts{f.bus, nullptr, &physics, nullptr};

    // Ground + falling ball with a script: On Collision Begin -> print "hit " + other's name.
    const Entity ground = f.scene.CreateEntity("Ground");
    f.scene.EditTransform(ground).position = {0.0f, -0.5f, 0.0f};
    Collider box;
    box.halfExtents = {10.0f, 0.5f, 10.0f};
    f.scene.GetRegistry().Emplace<RigidBody>(ground, RigidBody{.type = BodyType::Static});
    f.scene.GetRegistry().Emplace<Collider>(ground, box);
    f.scene.EditTransform(f.actor).position = {0.0f, 2.0f, 0.0f};
    Collider sphere;
    sphere.shape  = ColliderShape::Sphere;
    sphere.radius = 0.5f;
    f.scene.GetRegistry().Emplace<RigidBody>(f.actor);
    f.scene.GetRegistry().Emplace<Collider>(f.actor, sphere);

    const std::uint32_t hit    = f.Node("Event.CollisionBegin");
    const std::uint32_t name   = f.Node("String.FromEntity");
    const std::uint32_t append = f.Node("String.Append");
    const std::uint32_t print  = f.Print("");
    f.Set(append, "A", std::string("hit "));
    f.Link(hit, "Other", name, "Entity");
    f.Link(name, "Result", append, "B");
    f.Link(append, "Result", print, "Text");
    f.Link(hit, "Out", print, "In");
    // BeginPlay: impulse upwards on self is applied before the first step -> lands later.
    const std::uint32_t begin   = f.Node("Event.BeginPlay");
    const std::uint32_t impulse = f.Node("Physics.AddImpulse");
    f.Set(impulse, "Impulse", glm::vec3(0.0f, 2.0f, 0.0f));
    f.Link(begin, "Out", impulse, "In");

    f.scene.GetRegistry().Emplace<ScriptComponent>(f.actor, ScriptComponent{"hit.ugraph"});
    scripts.Provide("hit.ugraph", f.graph);
    physics.Sync(f.scene); // bodies exist before BeginPlay
    scripts.Begin(f.scene);
    CHECK(physics.LinearVelocity(f.actor).y > 1.0f);
    for (int i = 0; i < 180; ++i) {
        physics.Step(f.scene, 1.0f / 60.0f);
        scripts.Update(f.scene, 1.0f / 60.0f);
    }
    const auto messages = scripts.Messages();
    CHECK(std::ranges::count_if(messages, [](const ScriptMessage& m) { return m.text == "hit Ground"; }) == 1);

    // Removing the component ends the script; a destroyed entity's script is dropped.
    f.scene.GetRegistry().Remove<ScriptComponent>(f.actor);
    scripts.Update(f.scene, 1.0f / 60.0f);
    CHECK(scripts.Stats().instances == 0);
    scripts.End(f.scene);
}

TEST_CASE(Script_ErrorsKeepGraphFromRunning)
{
    ScriptFixture f;
    const std::uint32_t begin = f.Node("Event.BeginPlay");
    const std::uint32_t get   = f.Node("Variable.Get", "Missing");
    const std::uint32_t print = f.Print("never");
    f.Link(begin, "Out", print, "In");
    (void)get;
    f.Start("broken.ugraph");
    CHECK(f.Printed("never") == 0 && f.Errors());
    const ScriptDebugInfo* debug = f.scripts.Debug("broken.ugraph");
    CHECK(debug && !debug->diagnostics.empty() && debug->diagnostics[0].node == get);

    // Snapshots keep the script path; missing files are reported, not fatal.
    const std::string state = SnapshotEntityState(f.scene, f.actor);
    f.scene.GetRegistry().Remove<ScriptComponent>(f.actor);
    ApplyEntityState(f.scene, f.actor, state);
    CHECK(f.scene.GetRegistry().Get<ScriptComponent>(f.actor).graph == "broken.ugraph");
    f.scripts.End(f.scene);
    f.scripts.Provide("broken.ugraph", std::nullopt);
    f.scripts.Begin(f.scene); // file does not exist
    CHECK(f.Errors());
    f.scripts.End(f.scene);
}

TEST_CASE(Script_SampleGraphsValidateAndRun)
{
    const std::filesystem::path dir = std::filesystem::path(ENGINE_ASSET_DIR) / "scripts";
    for (const char* name : {"Rotator.ugraph", "RainOnSpace.ugraph"}) {
        const ScriptGraph graph = LoadScriptGraph(dir / name);
        const auto        diagnostics = ValidateScriptGraph(graph);
        CHECK(std::ranges::none_of(diagnostics, [](const ScriptDiagnostic& d) { return d.error; }));
        for (const ScriptDiagnostic& d : diagnostics)
            std::printf("  %s: node %u: %s\n", name, d.node, d.message.c_str());
        CHECK(ScriptGraphFromJson(ScriptGraphToJson(graph)).nodes.size() == graph.nodes.size());
    }

    // Rotator: 45 degrees per second around world Y.
    ScriptFixture f;
    f.scene.GetRegistry().Emplace<ScriptComponent>(f.actor, ScriptComponent{(dir / "Rotator.ugraph").string()});
    f.scripts.Begin(f.scene);
    CHECK(f.Printed("Rotator started") == 1);
    f.Run(1.0f);
    const glm::quat q     = f.scene.GetTransform(f.actor).rotation;
    const float     angle = glm::degrees(2.0f * std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f)));
    CHECK(std::abs(angle - 45.0f) < 1.0f && std::abs(q.x) < 1e-4f && std::abs(q.z) < 1e-4f);
    f.scripts.End(f.scene);
}
