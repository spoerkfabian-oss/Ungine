#include "Test.h"

#include "Engine/Events/EventBus.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptNodes.h"
#include "Engine/Script/ScriptSystem.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

using namespace Engine;

namespace {

struct Graph {
    ScriptGraph g;

    std::uint32_t Node(const char* type, std::string param = {}, std::string function = {})
    {
        return g.AddNode(type, {}, std::move(param), std::move(function));
    }
    void Link(std::uint32_t from, const char* fromPin, std::uint32_t to, const char* toPin)
    {
        const std::string error = g.Connect(from, fromPin, to, toPin);
        CHECK(error.empty());
        if (!error.empty())
            std::printf("  connect %s -> %s: %s\n", fromPin, toPin, error.c_str());
    }
    void Set(std::uint32_t node, const char* pin, ScriptValue value) { g.FindNode(node)->defaults[pin] = std::move(value); }
    std::uint32_t Find(const char* type, const std::string& function) const
    {
        for (const ScriptNode& n : g.nodes)
            if (n.type == type && n.function == function)
                return n.id;
        return 0;
    }
    bool Valid() const
    {
        const auto d = ValidateScriptGraph(g);
        for (const ScriptDiagnostic& x : d)
            if (x.error)
                std::printf("  diagnostic (node %u): %s\n", x.node, x.message.c_str());
        return std::ranges::none_of(d, [](const ScriptDiagnostic& x) { return x.error; });
    }
};

bool HasError(const ScriptGraph& g, std::string_view text)
{
    return std::ranges::any_of(ValidateScriptGraph(g), [&](const ScriptDiagnostic& d) {
        return d.error && d.message.find(text) != std::string::npos;
    });
}

struct Runner {
    EventBus     bus;
    Scene        scene;
    ScriptSystem scripts{bus};

    Entity Add(const char* name, const char* file, const ScriptGraph& graph)
    {
        const Entity e = scene.CreateEntity(name);
        scene.GetRegistry().Emplace<ScriptComponent>(e, ScriptComponent{file});
        scripts.Provide(file, graph);
        return e;
    }
    void Run(float seconds, float dt = 0.1f)
    {
        for (float t = 0.0f; t < seconds - 1e-4f; t += dt)
            scripts.Update(scene, dt);
    }
    bool Printed(std::string_view text) const
    {
        return std::ranges::any_of(scripts.Messages(), [&](const ScriptMessage& m) { return m.text == text; });
    }
    ScriptValue Var(const char* file, Entity e, std::string_view name) const
    {
        const auto w = scripts.Watch(file, e);
        if (w)
            for (const auto& [n, v] : w->variables)
                if (n == name)
                    return v;
        return false;
    }
};

std::uint32_t Print(Graph& g, const char* text = "", std::string function = {})
{
    const std::uint32_t n = g.Node("Debug.Print", {}, std::move(function));
    g.Set(n, "Text", std::string(text));
    g.Set(n, "Duration", 1000.0f);
    return n;
}

} // namespace

TEST_CASE(Blueprint_ArrayValues)
{
    ScriptValue a = MakeArray(PinType::Int, {std::int32_t{1}, std::int32_t{2}});
    ScriptValue b = a; // shares the elements
    CHECK(TypeOf(a) == PinType::IntArray && ValuesEqual(a, b));
    MutableArray(b, PinType::Int).items.push_back(std::int32_t{3}); // copy on write
    CHECK(ArrayItems(a).items.size() == 2 && ArrayItems(b).items.size() == 3 && !ValuesEqual(a, b));
    CHECK(ToDisplayString(b) == "[1, 2, 3]");
    CHECK(CanConvert(PinType::IntArray, PinType::String) && !CanConvert(PinType::IntArray, PinType::FloatArray) &&
          !CanConvert(PinType::Int, PinType::IntArray));
    CHECK(ArrayOf(PinType::Vec3) == PinType::Vec3Array && ElementType(PinType::EntityArray) == PinType::Entity);
    CHECK(PinTypeFromString("stringArray") == PinType::StringArray && ArrayItems(DefaultValue(PinType::BoolArray)).items.empty());
    CHECK(TypeOf(DefaultValue(PinType::Vec3Array)) == PinType::Vec3Array);

    // Variables and defaults survive JSON.
    ScriptGraph g;
    g.variables.push_back({"Points", PinType::Vec3Array, MakeArray(PinType::Vec3, {glm::vec3(1, 2, 3)}), true});
    const ScriptGraph back = ScriptGraphFromJson(ScriptGraphToJson(g));
    CHECK(back.variables.size() == 1 && back.variables[0].exposed && ValuesEqual(back.variables[0].value, g.variables[0].value));
}

TEST_CASE(Blueprint_FunctionsRunAndEdit)
{
    Graph g;
    g.g.variables.push_back({"Total", PinType::Float, 0.0f, false});
    // AddScaled(A, B) -> Result = A + B * Scale (local, 2).
    CHECK(g.g.AddFunction("AddScaled") && !g.g.AddFunction("AddScaled") && !g.g.AddFunction("bad/name"));
    ScriptFunction* f = g.g.FindFunction("AddScaled");
    f->inputs  = {{"A", PinType::Float}, {"B", PinType::Float}};
    f->outputs = {{"Result", PinType::Float}};
    f->locals  = {{"Scale", PinType::Float, 2.0f, false}};
    g.g.FunctionSignatureChanged("AddScaled");
    const std::uint32_t entry = g.Find("Function.Entry", "AddScaled"), ret = g.Find("Function.Return", "AddScaled");
    CHECK(entry && ret);
    const std::uint32_t mul   = g.Node("Math.MultiplyFloat", {}, "AddScaled");
    const std::uint32_t add   = g.Node("Math.AddFloat", {}, "AddScaled");
    const std::uint32_t scale = g.Node("Variable.Get", "Scale", "AddScaled");
    g.Link(entry, "A", add, "A");
    g.Link(entry, "B", mul, "A");
    g.Link(scale, "Value", mul, "B");
    g.Link(mul, "Result", add, "B");
    g.Link(add, "Result", ret, "Result");
    // Double(X) -> Y = X * 2, pure.
    CHECK(g.g.AddFunction("Double"));
    ScriptFunction* d = g.g.FindFunction("Double");
    d->inputs  = {{"X", PinType::Int}};
    d->outputs = {{"Y", PinType::Int}};
    g.g.SetFunctionPure("Double", true);
    const std::uint32_t mulI = g.Node("Math.MultiplyInt", {}, "Double");
    g.Set(mulI, "B", std::int32_t{2});
    g.Link(g.Find("Function.Entry", "Double"), "X", mulI, "A");
    g.Link(mulI, "Result", g.Find("Function.Return", "Double"), "Y");

    // Event graph: Total = AddScaled(1, 3); print it; print Double(21).
    const std::uint32_t begin = g.Node("Event.BeginPlay");
    const std::uint32_t call  = g.Node("Function.Call", "AddScaled");
    g.Set(call, "A", 1.0f);
    g.Set(call, "B", 3.0f);
    const std::uint32_t set = g.Node("Variable.Set", "Total");
    const std::uint32_t p1  = Print(g);
    const std::uint32_t pure = g.Node("Function.CallPure", "Double");
    g.Set(pure, "X", std::int32_t{21});
    const std::uint32_t p2 = Print(g);
    g.Link(begin, "Out", call, "In");
    g.Link(call, "Then", set, "In");
    g.Link(call, "Result", set, "Value");
    g.Link(set, "Then", p1, "In");
    g.Link(set, "Value", p1, "Text");
    g.Link(p1, "Then", p2, "In");
    g.Link(pure, "Y", p2, "Text");
    CHECK(!g.g.Connect(entry, "A", p1, "Text").empty()); // across functions
    CHECK(g.Valid());

    {
        Runner       r;
        const Entity e = r.Add("Actor", "fn.ugraph", g.g);
        r.scripts.Begin(r.scene);
        CHECK(r.Printed("7") && r.Printed("42") && r.scripts.Stats().errors == 0);
        CHECK(ValuesEqual(r.Var("fn.ugraph", e, "Total"), 7.0f));
        r.scripts.End(r.scene);
    }

    // JSON round trip (version 2).
    const ScriptGraph back = ScriptGraphFromJson(ScriptGraphToJson(g.g));
    CHECK(back.functions.size() == 2 && back.FindFunction("Double")->pure && back.FindFunction("AddScaled")->locals.size() == 1);
    CHECK(back.FindNode(mul)->function == "AddScaled" && back.links.size() == g.g.links.size());
    CHECK(std::ranges::none_of(ValidateScriptGraph(back), [](const ScriptDiagnostic& x) { return x.error; }));

    // Errors: latent / events inside functions, recursion, call kind.
    {
        Graph bad = g;
        bad.Node("Flow.Delay", {}, "AddScaled");
        CHECK(HasError(bad.g, "latent"));
        Graph ev = g;
        ev.Node("Event.Tick", {}, "AddScaled");
        CHECK(HasError(ev.g, "inside a function"));
        Graph rec = g;
        rec.Node("Function.Call", "AddScaled", "AddScaled");
        CHECK(HasError(rec.g, "Recursive"));
        Graph kind = g;
        kind.Node("Function.Call", "Double");
        CHECK(HasError(kind.g, "is pure"));
    }

    // Rename / purity / removal keep the graph consistent.
    CHECK(g.g.RenameFunction("AddScaled", "Combine") && g.g.FindNode(call)->param == "Combine" &&
          g.g.FindNode(mul)->function == "Combine" && g.Valid());
    CHECK(g.g.RenameVariable("Scale", "Factor", "Combine") && g.g.FindNode(scale)->param == "Factor" && g.Valid());
    g.g.SetFunctionPure("Double", false);
    CHECK(g.g.FindNode(pure)->type == "Function.Call");
    g.g.RemoveFunction("Double");
    CHECK(!g.g.FindNode(pure) && !g.g.FindFunction("Double") && g.Valid());
}

TEST_CASE(Blueprint_ArraysForEachAndInference)
{
    Graph g;
    g.g.variables.push_back({"Items", PinType::IntArray, MakeArray(PinType::Int), false});
    g.g.variables.push_back({"Sum", PinType::Int, std::int32_t{0}, false});
    const std::uint32_t begin = g.Node("Event.BeginPlay");
    const std::uint32_t add1  = g.Node("Array.Add", "Items");
    const std::uint32_t add2  = g.Node("Array.Add", "Items");
    const std::uint32_t ins   = g.Node("Array.Insert", "Items");
    g.Set(add1, "Item", std::int32_t{5});
    g.Set(add2, "Item", std::int32_t{7});
    g.Set(ins, "Item", std::int32_t{1});
    g.Set(ins, "Index", std::int32_t{0});
    const std::uint32_t loop = g.Node("Flow.ForEach");
    const std::uint32_t get  = g.Node("Variable.Get", "Items");
    CHECK(g.g.FindNode(loop)->param == "float");
    g.Link(get, "Value", loop, "Array"); // the loop adopts the element type
    CHECK(g.g.FindNode(loop)->param == "int");
    const std::uint32_t sumGet = g.Node("Variable.Get", "Sum");
    const std::uint32_t plus   = g.Node("Math.AddInt");
    const std::uint32_t sumSet = g.Node("Variable.Set", "Sum");
    g.Link(sumGet, "Value", plus, "A");
    g.Link(loop, "Element", plus, "B");
    g.Link(plus, "Result", sumSet, "Value");
    const std::uint32_t find = g.Node("Array.Find");
    g.Link(get, "Value", find, "Array");
    g.Set(find, "Item", std::int32_t{7});
    const std::uint32_t make = g.Node("Array.Make", "string:3");
    g.Set(make, "0", std::string("a"));
    const std::uint32_t length = g.Node("Array.Length");
    g.Link(make, "Array", length, "Array");
    CHECK(g.g.FindNode(length)->param == "string");
    const std::uint32_t pSum = Print(g), pFind = Print(g), pLen = Print(g), pArr = Print(g);
    g.Link(begin, "Out", add1, "In");
    g.Link(add1, "Then", add2, "In");
    g.Link(add2, "Then", ins, "In");
    g.Link(ins, "Then", loop, "In");
    g.Link(loop, "Loop Body", sumSet, "In");
    g.Link(loop, "Completed", pSum, "In");
    g.Link(sumSet, "Value", pSum, "Text");
    g.Link(pSum, "Then", pFind, "In");
    g.Link(find, "Index", pFind, "Text");
    g.Link(pFind, "Then", pLen, "In");
    g.Link(length, "Length", pLen, "Text");
    g.Link(pLen, "Then", pArr, "In");
    g.Link(get, "Value", pArr, "Text");

    // Reroutes take the type of what is connected.
    const std::uint32_t reroute = g.Node("Utility.Reroute");
    g.Link(get, "Value", reroute, "In");
    CHECK(g.g.FindNode(reroute)->param == "intArray");
    CHECK(!g.g.Connect(reroute, "Out", plus, "A").empty()); // int[] -> int: no
    // An array node needs an array variable.
    Graph bad = g;
    bad.Node("Array.Clear", "Sum");
    CHECK(HasError(bad.g, "not an array"));
    CHECK(g.Valid());

    Runner       r;
    const Entity e = r.Add("Actor", "arrays.ugraph", g.g);
    r.scripts.Begin(r.scene);
    CHECK(r.Printed("13") && r.Printed("2") && r.Printed("3") && r.Printed("[1, 5, 7]"));
    CHECK(ValuesEqual(r.Var("arrays.ugraph", e, "Items"),
                      MakeArray(PinType::Int, {std::int32_t{1}, std::int32_t{5}, std::int32_t{7}})));
    CHECK(r.scripts.Stats().errors == 0);
}

TEST_CASE(Blueprint_Timers)
{
    Graph g;
    g.g.variables.push_back({"Count", PinType::Int, std::int32_t{0}, false});
    g.g.variables.push_back({"Handle", PinType::Int, std::int32_t{0}, false});
    const std::uint32_t begin = g.Node("Event.BeginPlay");
    const std::uint32_t timer = g.Node("Timer.Set");
    g.Set(timer, "Event", std::string("Step"));
    g.Set(timer, "Time", 0.5f);
    g.Set(timer, "Looping", true);
    const std::uint32_t keep = g.Node("Variable.Set", "Handle");
    const std::uint32_t stop = g.Node("Timer.Set");
    g.Set(stop, "Event", std::string("Stop"));
    g.Set(stop, "Time", 1.7f);
    g.Link(begin, "Out", timer, "In");
    g.Link(timer, "Handle", keep, "Value");
    g.Link(timer, "Then", keep, "In");
    g.Link(keep, "Then", stop, "In");
    const std::uint32_t step = g.Node("Event.Custom", "Step");
    const std::uint32_t get  = g.Node("Variable.Get", "Count");
    const std::uint32_t inc  = g.Node("Math.AddInt");
    g.Set(inc, "B", std::int32_t{1});
    const std::uint32_t set = g.Node("Variable.Set", "Count");
    g.Link(get, "Value", inc, "A");
    g.Link(inc, "Result", set, "Value");
    g.Link(step, "Out", set, "In");
    const std::uint32_t onStop = g.Node("Event.Custom", "Stop");
    const std::uint32_t clear  = g.Node("Timer.Clear");
    const std::uint32_t handle = g.Node("Variable.Get", "Handle");
    g.Link(handle, "Value", clear, "Handle");
    g.Link(onStop, "Out", clear, "In");
    CHECK(g.Valid());

    Runner       r;
    const Entity e = r.Add("Actor", "timers.ugraph", g.g);
    r.scripts.Begin(r.scene);
    CHECK(r.scripts.Stats().instances == 1);
    r.Run(1.6f);
    CHECK(ValuesEqual(r.Var("timers.ugraph", e, "Count"), std::int32_t{3}));
    CHECK(r.scripts.Stats().timers == 2);
    r.Run(1.4f); // Stop at 1.7 s clears the looping timer
    CHECK(ValuesEqual(r.Var("timers.ugraph", e, "Count"), std::int32_t{3}) && r.scripts.Stats().timers == 0);
}

TEST_CASE(Blueprint_DebuggerBreakStepContinue)
{
    Graph g;
    g.g.variables.push_back({"N", PinType::Int, std::int32_t{0}, false});
    const std::uint32_t tick = g.Node("Event.Tick");
    const std::uint32_t get  = g.Node("Variable.Get", "N");
    const std::uint32_t inc  = g.Node("Math.AddInt");
    g.Set(inc, "B", std::int32_t{1});
    const std::uint32_t set = g.Node("Variable.Set", "N");
    const std::uint32_t out = Print(g);
    g.Link(get, "Value", inc, "A");
    g.Link(inc, "Result", set, "Value");
    g.Link(tick, "Out", set, "In");
    g.Link(set, "Then", out, "In");
    g.Link(set, "Value", out, "Text");
    g.g.SetBreakpoint(set, true);
    CHECK(g.Valid());

    Runner       r;
    const Entity a = r.Add("A", "debug.ugraph", g.g);
    const Entity b = r.scene.CreateEntity("B");
    r.scene.GetRegistry().Emplace<ScriptComponent>(b, ScriptComponent{"debug.ugraph"});
    r.scripts.Begin(r.scene);
    r.scripts.Update(r.scene, 0.1f);
    CHECK(r.scripts.DebugPaused());
    const auto at = r.scripts.PausedAt();
    CHECK(at && at->node == set && at->function.empty());
    const Entity first = at ? at->entity : NullEntity;
    CHECK(first == a || first == b);
    CHECK(r.scripts.Stats().queued == 1); // the other instance's Tick waits
    const double time = r.scripts.Time();
    r.scripts.Update(r.scene, 0.1f); // held
    CHECK(r.scripts.Time() == time && ValuesEqual(r.Var("debug.ugraph", first, "N"), std::int32_t{0}));

    r.scripts.DebugStep(r.scene); // runs Set, stops at Print
    CHECK(r.scripts.DebugPaused() && r.scripts.PausedAt()->node == out);
    CHECK(ValuesEqual(r.Var("debug.ugraph", first, "N"), std::int32_t{1}));
    const auto watch = r.scripts.Watch("debug.ugraph", first);
    CHECK(watch && ValuesEqual(watch->pins.at({set, "Value"}), std::int32_t{1}));
    CHECK(r.scripts.InstancesOf("debug.ugraph").size() == 2);

    r.scripts.DebugContinue(r.scene); // Print runs; the queued Tick of the other one hits the breakpoint
    CHECK(r.Printed("1") && r.scripts.DebugPaused() && r.scripts.PausedAt()->entity != first);
    r.scripts.SetBreakpoints("debug.ugraph", {});
    r.scripts.DebugContinue(r.scene);
    CHECK(!r.scripts.DebugPaused());
    r.Run(0.3f);
    CHECK(ValuesEqual(r.Var("debug.ugraph", a, "N"), std::int32_t{4}) && ValuesEqual(r.Var("debug.ugraph", b, "N"), std::int32_t{4}));
    r.scripts.End(r.scene);

    // A breakpoint inside a function: the locals are visible.
    Graph fn;
    CHECK(fn.g.AddFunction("F"));
    fn.g.FindFunction("F")->locals = {{"L", PinType::Int, std::int32_t{5}, false}};
    const std::uint32_t entry = fn.Find("Function.Entry", "F"), ret = fn.Find("Function.Return", "F");
    const std::uint32_t setL  = fn.Node("Variable.Set", "L", "F");
    fn.Set(setL, "Value", std::int32_t{9});
    fn.Link(entry, "Then", setL, "In");
    fn.Link(setL, "Then", ret, "In");
    const std::uint32_t begin = fn.Node("Event.BeginPlay");
    const std::uint32_t call  = fn.Node("Function.Call", "F");
    fn.Link(begin, "Out", call, "In");
    fn.g.SetBreakpoint(setL, true);
    CHECK(fn.Valid());
    Runner       r2;
    const Entity e = r2.Add("E", "fnbreak.ugraph", fn.g);
    r2.scripts.Begin(r2.scene); // BeginPlay stops inside F
    CHECK(r2.scripts.DebugPaused() && r2.scripts.PausedAt()->function == "F");
    auto w = r2.scripts.Watch("fnbreak.ugraph", e);
    CHECK(w && w->function == "F" && w->locals.size() == 1 && ValuesEqual(w->locals[0].second, std::int32_t{5}));
    r2.scripts.DebugStep(r2.scene); // stops at Return
    CHECK(r2.scripts.DebugPaused() && r2.scripts.PausedAt()->node == ret);
    w = r2.scripts.Watch("fnbreak.ugraph", e);
    CHECK(w && ValuesEqual(w->locals[0].second, std::int32_t{9}));
    r2.scripts.SetBreakpoints("fnbreak.ugraph", {});
    r2.scripts.DebugContinue(r2.scene);
    r2.scripts.Update(r2.scene, 0.1f); // the step mode ends at the next node that runs: none here
    CHECK(!r2.scripts.DebugPaused() && r2.scripts.Stats().errors == 0);
    r2.scripts.End(r2.scene);
}

TEST_CASE(Blueprint_InstanceVariablesTagsAndQueries)
{
    // a.ugraph: exposed Speed / Target, hidden Hidden; Ping sets Hidden; BeginPlay tags itself.
    Graph a;
    a.g.variables.push_back({"Speed", PinType::Float, 1.0f, true});
    a.g.variables.push_back({"Hidden", PinType::Float, 1.0f, false});
    a.g.variables.push_back({"Target", PinType::Entity, NullEntity, true});
    const std::uint32_t begin = a.Node("Event.BeginPlay");
    const std::uint32_t tag   = a.Node("Entity.AddTag");
    a.Set(tag, "Tag", std::string("enemy"));
    const std::uint32_t speed  = a.Node("Variable.Get", "Speed");
    const std::uint32_t target = a.Node("Variable.Get", "Target");
    const std::uint32_t name   = a.Node("String.FromEntity");
    const std::uint32_t p1 = Print(a), p2 = Print(a);
    a.Link(begin, "Out", tag, "In");
    a.Link(tag, "Then", p1, "In");
    a.Link(speed, "Value", p1, "Text");
    a.Link(p1, "Then", p2, "In");
    a.Link(target, "Value", name, "Entity");
    a.Link(name, "Result", p2, "Text");
    const std::uint32_t ping = a.Node("Event.Custom", "Ping");
    const std::uint32_t setH = a.Node("Variable.Set", "Hidden");
    a.Set(setH, "Value", 99.0f);
    a.Link(ping, "Out", setH, "In");
    CHECK(a.Valid());

    // b.ugraph (on the buddy): first Tick pings Actor1, sets its Speed, counts the enemies, reads Hidden.
    Graph b;
    const std::uint32_t tick = b.Node("Event.Tick");
    const std::uint32_t once = b.Node("Flow.DoOnce");
    const std::uint32_t find = b.Node("Entity.FindByName");
    b.Set(find, "Name", std::string("Actor1"));
    const std::uint32_t call = b.Node("Script.CallEvent");
    b.Set(call, "Event", std::string("Ping"));
    const std::uint32_t set = b.Node("Script.SetVariable", "float");
    b.Set(set, "Name", std::string("Speed"));
    b.Set(set, "Value", 42.0f);
    const std::uint32_t all = b.Node("Entity.GetAllWithTag");
    b.Set(all, "Tag", std::string("enemy"));
    const std::uint32_t len = b.Node("Array.Length");
    b.Link(all, "Entities", len, "Array");
    const std::uint32_t getH = b.Node("Script.GetVariable", "float");
    b.Set(getH, "Name", std::string("Hidden"));
    const std::uint32_t p3 = Print(b), p4 = Print(b);
    b.Link(tick, "Out", once, "In");
    b.Link(once, "Completed", call, "In");
    b.Link(find, "Entity", call, "Target");
    b.Link(call, "Then", set, "In");
    b.Link(find, "Entity", set, "Target");
    b.Link(set, "Then", p3, "In");
    b.Link(len, "Length", p3, "Text");
    b.Link(p3, "Then", p4, "In");
    b.Link(find, "Entity", getH, "Target");
    b.Link(getH, "Value", p4, "Text");
    CHECK(b.Valid());

    Runner       r;
    const Entity buddy  = r.scene.CreateEntity("Buddy");
    const Entity actor1 = r.Add("Actor1", "a.ugraph", a.g);
    ScriptComponent& sc = r.scene.GetRegistry().Get<ScriptComponent>(actor1);
    sc.variables["Speed"]  = {5.0f, 0};
    sc.variables["Hidden"] = {7.0f, 0}; // not exposed: ignored
    sc.variables["Target"] = {NullEntity, r.scene.GetRegistry().Get<Uuid>(buddy).value};
    r.scene.GetRegistry().Emplace<ScriptComponent>(buddy, ScriptComponent{"b.ugraph"});
    r.scripts.Provide("b.ugraph", b.g);

    // The overrides and tags are part of the entity state (undo, scene files).
    const std::string     state = SnapshotEntityState(r.scene, actor1);
    const ScriptComponent copy  = r.scene.GetRegistry().Get<ScriptComponent>(actor1); // sc dangles after Emplace
    r.scene.GetRegistry().Get<ScriptComponent>(actor1).variables.clear();
    ApplyEntityState(r.scene, actor1, state);
    CHECK(r.scene.GetRegistry().Get<ScriptComponent>(actor1) == copy);

    r.scripts.Begin(r.scene);
    CHECK(r.Printed("5") && r.Printed("Buddy"));
    CHECK(r.scene.GetRegistry().Has<Tags>(actor1) && r.scene.GetRegistry().Get<Tags>(actor1).Has("enemy"));
    r.scripts.Update(r.scene, 0.1f);
    CHECK(r.Printed("1") && r.Printed("99"));
    CHECK(ValuesEqual(r.Var("a.ugraph", actor1, "Speed"), 42.0f) && r.scripts.Stats().errors == 0);
    r.scripts.End(r.scene);

    Tags tags;
    tags.values = {"a", "b"};
    r.scene.GetRegistry().Emplace<Tags>(buddy, tags);
    const std::string tagState = SnapshotEntityState(r.scene, buddy);
    r.scene.GetRegistry().Remove<Tags>(buddy);
    ApplyEntityState(r.scene, buddy, tagState);
    CHECK(r.scene.GetRegistry().Has<Tags>(buddy) && r.scene.GetRegistry().Get<Tags>(buddy) == tags);
}

TEST_CASE(Blueprint_CameraAndMouseNodes)
{
    Graph g;
    const std::uint32_t begin = g.Node("Event.BeginPlay");
    const std::uint32_t ray   = g.Node("Camera.ScreenToWorld");
    g.Set(ray, "Screen Position", glm::vec3(400.0f, 300.0f, 0.0f));
    const std::uint32_t screen = g.Node("Camera.WorldToScreen");
    g.Set(screen, "Location", glm::vec3(0.0f));
    const std::uint32_t mouse = g.Node("Input.MousePosition");
    const std::uint32_t p1 = Print(g), p2 = Print(g), p3 = Print(g);
    g.Link(begin, "Out", p1, "In");
    g.Link(ray, "Direction", p1, "Text");
    g.Link(p1, "Then", p2, "In");
    g.Link(screen, "Screen Position", p2, "Text");
    g.Link(p2, "Then", p3, "In");
    g.Link(mouse, "In View", p3, "Text");
    CHECK(g.Valid());

    Runner       r;
    const Entity camera = r.scene.CreateEntity("Camera");
    r.scene.EditTransform(camera).position = {0.0f, 0.0f, 5.0f};
    r.scene.GetRegistry().Emplace<CameraComponent>(camera);
    r.Add("Actor", "camera.ugraph", g.g);
    r.scripts.SetViewport({.origin = {10.0f, 20.0f}, .size = {800.0f, 600.0f}});
    r.scripts.Begin(r.scene);
    CHECK(r.Printed("(0, 0, -1)") && r.Printed("(400, 300, 0)"));
    CHECK(r.Printed("false")); // no input: the mouse is nowhere
    CHECK(r.scripts.Stats().errors == 0);
}

namespace {
std::vector<Entity> Children(const Scene& scene, Entity e) { return scene.GetRegistry().Get<Hierarchy>(e).children; }
std::uint64_t       UuidOf(const Scene& scene, Entity e) { return scene.GetRegistry().Get<Uuid>(e).value; }
std::string         ReadText(const std::filesystem::path& file)
{
    std::ifstream      in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}
bool HasKeys(const std::vector<std::string>& keys, std::initializer_list<const char*> expected)
{
    return keys.size() == expected.size() &&
           std::ranges::all_of(expected, [&](const char* k) { return std::ranges::find(keys, k) != keys.end(); });
}
} // namespace

TEST_CASE(Prefab_InstancesOverridesApplyRevertAndFiles)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("ungine_prefab_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir / "Scripts");
    const fs::path prefabFile = dir / "Prefabs" / "Lamp.uprefab";
    const AssetManager* none  = nullptr;
    std::vector<ModelHandle> models; // stays empty: no asset manager

    Scene     scene;
    Registry& r    = scene.GetRegistry();
    const Entity lamp = scene.CreateEntity("Lamp");
    scene.EditTransform(lamp).position = {5.0f, 0.0f, 0.0f};
    Light lampLight;
    lampLight.intensity = 2.0f;
    r.Emplace<Light>(lamp, lampLight);
    r.Emplace<Tags>(lamp, Tags{{"lamp"}});
    const Entity bulb = scene.CreateEntity("Bulb", lamp);
    scene.EditTransform(bulb).position = {0.0f, 1.0f, 0.0f};
    Light bulbLight;
    bulbLight.color = {1.0f, 0.5f, 0.0f};
    r.Emplace<Light>(bulb, bulbLight);
    const Entity     sw = scene.CreateEntity("Switch", lamp);
    ScriptComponent script{(dir / "Scripts" / "Switch.ugraph").string()};
    script.variables["Target"] = {NullEntity, UuidOf(scene, bulb)};
    r.Emplace<ScriptComponent>(sw, script);

    // Create: the subtree becomes the first instance; the file stores relative paths.
    CreatePrefab(prefabFile, scene, none, lamp);
    CHECK(fs::exists(prefabFile) && r.Has<PrefabInstance>(lamp) && !r.Has<PrefabLink>(lamp));
    CHECK(r.Get<PrefabLink>(bulb).instance == UuidOf(scene, lamp) && PrefabInstanceRoot(scene, sw) == lamp);
    CHECK(PrefabOverrides(scene, none, lamp).empty() && r.Get<Transform>(lamp).position.x == 5.0f);
    CHECK(ReadText(prefabFile).find("\"../Scripts/Switch.ugraph\"") != std::string::npos);

    // A second instance: same components, its own UUIDs, references mapped into the instance.
    Transform placement;
    placement.position = {-5.0f, 0.0f, 0.0f};
    const Entity lamp2 = InstantiatePrefab(scene, nullptr, prefabFile, NullEntity, placement, models);
    CHECK(r.Get<Name>(lamp2).value == "Lamp" && r.Get<Transform>(lamp2).position.x == -5.0f);
    CHECK(r.Get<Light>(lamp2).intensity == 2.0f && r.Get<Tags>(lamp2).Has("lamp"));
    CHECK(Children(scene, lamp2).size() == 2);
    const Entity bulb2 = Children(scene, lamp2)[0];
    const Entity sw2   = Children(scene, lamp2)[1];
    CHECK(r.Get<Name>(bulb2).value == "Bulb" && r.Get<Transform>(bulb2).position.y == 1.0f && UuidOf(scene, bulb2) != UuidOf(scene, bulb));
    CHECK(r.Get<ScriptComponent>(sw2).variables.at("Target").entityUuid == UuidOf(scene, bulb2));
    CHECK(r.Get<ScriptComponent>(sw2).graph.ends_with("Scripts/Switch.ugraph"));
    CHECK(PrefabOverrides(scene, none, lamp2).empty() && models.empty());

    // Overrides: a member's light; moving the root is not one.
    r.Get<Light>(bulb2).intensity = 7.0f;
    scene.EditTransform(lamp2).position.z = 3.0f;
    CHECK(HasKeys(PrefabOverriddenKeys(scene, none, bulb2), {"light"}) && PrefabOverriddenKeys(scene, none, lamp2).empty());

    // Apply the first instance (new color + intensity, a new child): the other keeps its intensity.
    r.Get<Light>(bulb).color     = {0.0f, 1.0f, 0.0f};
    r.Get<Light>(bulb).intensity = 3.0f;
    const Entity shade           = scene.CreateEntity("Shade", lamp);
    ApplyPrefabInstance(scene, nullptr, lamp, models);
    CHECK(r.Has<PrefabLink>(shade) && PrefabOverrides(scene, none, lamp).empty());
    CHECK(r.Valid(bulb2) && r.Get<Light>(bulb2).color == glm::vec3(0.0f, 1.0f, 0.0f) && r.Get<Light>(bulb2).intensity == 7.0f);
    CHECK(Children(scene, lamp2).size() == 3 && r.Get<Name>(Children(scene, lamp2)[2]).value == "Shade");
    CHECK(r.Get<Transform>(lamp2).position == glm::vec3(-5.0f, 0.0f, 3.0f));

    // Remove a member, add a plain child below a member, rename one: saved as overrides.
    const Entity shade2 = Children(scene, lamp2)[2];
    scene.DestroyEntity(shade2);
    const Entity attachment = scene.CreateEntity("Attachment", sw2);
    r.Get<Name>(sw2).value  = "Switch B";
    const auto overrides    = PrefabOverrides(scene, none, lamp2);
    CHECK(overrides.size() == 3);
    CHECK(std::ranges::count_if(overrides, [](const PrefabOverride& o) { return o.removed; }) == 1);
    const fs::path level = dir / "Level.scene";
    SaveSceneFile(level, scene, none);
    const std::string levelText = ReadText(level);
    CHECK(levelText.find("\"Shade\"") == std::string::npos && levelText.find("\"prefab\"") != std::string::npos);
    {
        Scene loaded;
        const auto handles = LoadSceneFile(level, loaded, static_cast<AssetManager*>(nullptr));
        CHECK(handles.empty());
        const Registry& lr = loaded.GetRegistry();
        const Entity    l2 = loaded.FindByUuid(UuidOf(scene, lamp2));
        CHECK(l2 != NullEntity && lr.Has<PrefabInstance>(l2) && Children(loaded, l2).size() == 2);
        const Entity b = loaded.FindByUuid(UuidOf(scene, bulb2));
        const Entity a = loaded.FindByUuid(UuidOf(scene, attachment));
        CHECK(b != NullEntity && lr.Get<Light>(b).intensity == 7.0f && lr.Get<Light>(b).color == glm::vec3(0.0f, 1.0f, 0.0f));
        CHECK(a != NullEntity && lr.Get<Hierarchy>(a).parent == loaded.FindByUuid(UuidOf(scene, sw2)) && !lr.Has<PrefabLink>(a));
        CHECK(lr.Get<Name>(loaded.FindByUuid(UuidOf(scene, sw2))).value == "Switch B");
        CHECK(lr.Get<Transform>(l2).position == glm::vec3(-5.0f, 0.0f, 3.0f) && PrefabOverrides(loaded, none, l2).size() == 3);
        CHECK(Children(loaded, loaded.FindByUuid(UuidOf(scene, lamp))).size() == 3); // the first instance as saved
    }

    // Revert one key, then everything: the removed member returns, the plain child stays.
    RevertPrefabOverrides(scene, nullptr, sw2, "name", models);
    CHECK(r.Get<Name>(sw2).value == "Switch" && PrefabOverrides(scene, none, lamp2).size() == 2);
    RevertPrefabOverrides(scene, nullptr, lamp2, "", models);
    CHECK(PrefabOverrides(scene, none, lamp2).empty() && r.Get<Light>(bulb2).intensity == 3.0f);
    CHECK(Children(scene, lamp2).size() == 3 && r.Valid(attachment) && r.Get<Hierarchy>(attachment).parent == sw2);

    // The file changes on disk: both instances follow.
    {
        std::string text = ReadText(prefabFile);
        const auto  at   = text.find("\"Bulb\"");
        CHECK(at != std::string::npos);
        text.replace(at, 6, "\"Glow\"");
        std::ofstream(prefabFile, std::ios::binary | std::ios::trunc) << text;
        fs::last_write_time(prefabFile, fs::last_write_time(prefabFile) + std::chrono::seconds(2));
    }
    CHECK(RefreshPrefabInstances(scene, nullptr, models) == 2);
    CHECK(r.Get<Name>(bulb).value == "Glow" && r.Get<Name>(bulb2).value == "Glow" && r.Valid(attachment));
    CHECK(RefreshPrefabInstances(scene, nullptr, models) == 0);

    // Duplicates: members follow a duplicated root; a member duplicated alone becomes plain.
    const Entity dupRoot = RestoreEntities(scene, SnapshotEntities(scene, std::vector<Entity>{lamp}), RestoreMode::Duplicate)[0];
    CHECK(r.Has<PrefabInstance>(dupRoot) && Children(scene, dupRoot).size() == 3);
    CHECK(r.Get<PrefabLink>(Children(scene, dupRoot)[0]).instance == UuidOf(scene, dupRoot) &&
          PrefabOverrides(scene, none, dupRoot).empty());
    const Entity dupBulb = RestoreEntities(scene, SnapshotEntities(scene, std::vector<Entity>{bulb}), RestoreMode::Duplicate)[0];
    CHECK(!r.Has<PrefabLink>(dupBulb) && PrefabInstanceRoot(scene, dupBulb) == NullEntity);
    scene.DestroyEntity(dupBulb);

    // Unlink: plain entities again.
    UnlinkPrefabInstance(scene, dupRoot);
    CHECK(!r.Has<PrefabInstance>(dupRoot) && !r.Has<PrefabLink>(Children(scene, dupRoot)[0]));

    // A missing prefab keeps the instance data for saving.
    const fs::path movedPrefab = dir / "Lamp.moved";
    SaveSceneFile(level, scene, none);
    fs::rename(prefabFile, movedPrefab);
    {
        Scene loaded;
        (void)LoadSceneFile(level, loaded, static_cast<AssetManager*>(nullptr));
        const Entity l2 = loaded.FindByUuid(UuidOf(scene, lamp2));
        CHECK(l2 != NullEntity && loaded.GetRegistry().Has<PrefabInstance>(l2) && Children(loaded, l2).size() == 1);
        SaveSceneFile(dir / "Level2.scene", loaded, none);
        const std::string text = ReadText(dir / "Level2.scene");
        CHECK(text.find(std::to_string(UuidOf(scene, bulb2))) != std::string::npos); // members kept
    }
    fs::rename(movedPrefab, prefabFile);

    // Spawn Prefab node.
    {
        Runner r2;
        Graph  g;
        const std::uint32_t begin = g.Node("Event.BeginPlay");
        const std::uint32_t spawn = g.Node("Entity.SpawnPrefab");
        g.Set(spawn, "Prefab", prefabFile.string());
        g.Set(spawn, "Location", glm::vec3(1.0f, 2.0f, 3.0f));
        const std::uint32_t name = g.Node("String.FromEntity");
        const std::uint32_t p    = Print(g);
        g.Link(begin, "Out", spawn, "In");
        g.Link(spawn, "Then", p, "In");
        g.Link(spawn, "Spawned", name, "Entity");
        g.Link(name, "Result", p, "Text");
        CHECK(g.Valid());
        r2.Add("Spawner", "spawner.ugraph", g.g);
        r2.scripts.Begin(r2.scene);
        CHECK(r2.Printed("Lamp") && r2.scripts.Stats().errors == 0);
        Entity spawned = NullEntity;
        r2.scene.GetRegistry().ViewOf<PrefabInstance>().Each([&](Entity e, PrefabInstance&) { spawned = e; });
        CHECK(spawned != NullEntity && r2.scene.GetTransform(spawned).position == glm::vec3(1.0f, 2.0f, 3.0f));
        CHECK(Children(r2.scene, spawned).size() == 3);
        r2.scripts.End(r2.scene);
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}
