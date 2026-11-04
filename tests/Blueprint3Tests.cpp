#include "Test.h"
#include "BlueprintTestUtil.h"

#include "Engine/Script/ScriptRegistry.h"

#include <map>

using namespace BlueprintTest;

namespace {
ScriptValue IntArray(std::initializer_list<std::int32_t> values)
{
    std::vector<ScriptValue> items;
    for (std::int32_t v : values)
        items.push_back(v);
    return MakeArray(PinType::Int, std::move(items));
}
} // namespace

TEST_CASE(Blueprint3_NestedContainersAndMapKeys)
{
    ScriptRegistry::Clear();
    // Type names: nested arrays / maps round trip, display names.
    for (const char* name : {"intArray[]", "intArray[][]", "array:map:string:int", "map:string:intArray", "map:int:map:string:float",
                             "map:float:int", "map:vec3:string", "array:map:float:vec3Array"}) {
        const auto t = PinTypeFromString(name);
        CHECK(t && ToString(*t) == name);
        if (!t || ToString(*t) != name)
            std::printf("  type %s -> %s\n", name, t ? ToString(*t).c_str() : "(none)");
    }
    const PinType grid = *PinTypeFromString("intArray[]");
    CHECK(grid == ArrayOf(PinType::IntArray) && ElementType(grid) == PinType::IntArray && grid != PinType::IntArray);
    CHECK(DisplayName(grid) == "int[][]" && DisplayName(*PinTypeFromString("array:map:string:int")) == "Map<string, int>[]" &&
          DisplayName(*PinTypeFromString("map:string:intArray")) == "Map<string, int[]>");
    CHECK(IsKeyType(PinType::Float) && IsKeyType(PinType::Vec3) && !IsKeyType(PinType::IntArray));

    // Values: defaults, fit, copy on write of inner arrays, deep equality, display.
    ScriptValue g = DefaultValue(grid);
    CHECK(ValueFits(g, grid) && ArrayItems(g).element == PinType::IntArray && !ValueFits(g, PinType::IntArray));
    MutableArray(g, PinType::IntArray).items = {IntArray({1, 2}), IntArray({3})};
    ScriptValue copy = g;
    MutableArray(MutableArray(copy, PinType::IntArray).items[0], PinType::Int).items.push_back(std::int32_t{9});
    CHECK(ArrayItems(ArrayItems(g).items[0]).items.size() == 2 && ArrayItems(ArrayItems(copy).items[0]).items.size() == 3);
    CHECK(!ValuesEqual(g, copy) && ToDisplayString(g) == "[[1, 2], [3]]");

    // Float / vec3 keys: sorted by value.
    const PinType byPos = PinType::Map(PinType::Vec3, PinType::String);
    ScriptValue   map   = DefaultValue(byPos);
    MutableMap(map, PinType::Vec3, PinType::String).items[glm::vec3(1, 0, 0)] = std::string("b");
    MutableMap(map, PinType::Vec3, PinType::String).items[glm::vec3(0, 5, 0)] = std::string("a");
    CHECK(MapOf(map).items.size() == 2 && std::get<std::string>(MapOf(map).items.begin()->second) == "a");

    // In a graph: variables of nested types survive JSON; nodes infer nested element types.
    Graph b;
    b.g.variables.push_back({"Grid", grid, g, false});
    const PinType   lists = PinType::Map(PinType::Float, PinType::IntArray);
    ScriptValue     lv    = DefaultValue(lists);
    MutableMap(lv, PinType::Float, PinType::IntArray).items[0.5f] = IntArray({4, 5, 6});
    b.g.variables.push_back({"Lists", lists, lv, false});
    const std::uint32_t begin = b.Node("Event.BeginPlay");
    const std::uint32_t get   = b.Node("Variable.Get", "Grid");
    const std::uint32_t at    = b.Node("Array.Get");
    const std::uint32_t len   = b.Node("Array.Length");
    const std::uint32_t p1    = Print(b);
    b.Link(get, "Value", at, "Array");
    b.Set(at, "Index", std::int32_t{0});
    b.Link(at, "Item", len, "Array");
    b.Link(len, "Length", p1, "Text");
    b.Link(begin, "Out", p1, "In");
    CHECK(b.g.FindNode(at)->param == "intArray" && b.g.FindNode(len)->param == "int");
    const std::uint32_t getLists = b.Node("Variable.Get", "Lists");
    const std::uint32_t find     = b.Node("Map.Find");
    const std::uint32_t len2     = b.Node("Array.Length");
    const std::uint32_t p2       = Print(b);
    b.Link(getLists, "Value", find, "Map");
    b.Set(find, "Key", 0.5f);
    b.Link(find, "Value", len2, "Array");
    b.Link(len2, "Length", p2, "Text");
    b.Link(p1, "Then", p2, "In");
    // Make Array of two int arrays.
    const std::uint32_t make = b.Node("Array.Make", "intArray:2");
    b.Set(make, "0", IntArray({1}));
    b.Set(make, "1", IntArray({2, 3}));
    const std::uint32_t len3 = b.Node("Array.Length");
    const std::uint32_t p3   = Print(b);
    b.Link(make, "Array", len3, "Array");
    b.Link(len3, "Length", p3, "Text");
    b.Link(p2, "Then", p3, "In");
    CHECK(b.Valid());

    const ScriptGraph loaded = ScriptGraphFromJson(ScriptGraphToJson(b.g));
    CHECK(loaded.variables.size() == 2 && loaded.variables[0].type == grid && ValuesEqual(loaded.variables[0].value, g));
    CHECK(loaded.variables[1].type == lists && ValuesEqual(loaded.variables[1].value, lv));

    Runner r;
    const Entity e = r.Add("Nested", "nested.ugraph", loaded);
    r.scripts.Begin(r.scene);
    CHECK(r.Printed("2") && r.Printed("3") && r.scripts.Stats().errors == 0);
    CHECK(ValuesEqual(r.Var("nested.ugraph", e, "Grid"), g));
    r.scripts.End(r.scene);
}

TEST_CASE(Blueprint3_RecursiveFunctions)
{
    ScriptRegistry::Clear();
    ScriptRegistry::AddInterface({"Counter", {{"Down", {{"N", PinType::Int}}, {{"Sum", PinType::Int}}}}, {}});
    Graph b;
    // Fact(N) = N < 2 ? 1 : N * Fact(N - 1)   (impure, Branch + Return)
    CHECK(b.g.AddFunction("Fact"));
    b.g.FindFunction("Fact")->inputs  = {{"N", PinType::Int}};
    b.g.FindFunction("Fact")->outputs = {{"Result", PinType::Int}};
    b.g.FunctionSignatureChanged("Fact");
    const std::uint32_t entry  = b.Find("Function.Entry", "Fact");
    const std::uint32_t ret1   = b.Find("Function.Return", "Fact");
    const std::uint32_t branch = b.Node("Flow.Branch", {}, "Fact");
    const std::uint32_t less   = b.Node("Compare.LessInt", {}, "Fact");
    b.Set(less, "B", std::int32_t{2});
    b.Link(entry, "N", less, "A");
    b.Link(less, "Result", branch, "Condition");
    b.Link(entry, "Then", branch, "In");
    b.Link(branch, "True", ret1, "In");
    b.Set(ret1, "Result", std::int32_t{1});
    const std::uint32_t sub  = b.Node("Math.SubtractInt", {}, "Fact");
    b.Set(sub, "B", std::int32_t{1});
    b.Link(entry, "N", sub, "A");
    const std::uint32_t self = b.Node("Function.Call", "Fact", "Fact");
    b.Link(sub, "Result", self, "N");
    b.Link(branch, "False", self, "In");
    const std::uint32_t mul  = b.Node("Math.MultiplyInt", {}, "Fact");
    b.Link(entry, "N", mul, "A"); // the outer call's N after the inner call returned
    b.Link(self, "Result", mul, "B");
    const std::uint32_t ret2 = b.Node("Function.Return", "Fact", "Fact");
    b.Link(self, "Then", ret2, "In");
    b.Link(mul, "Result", ret2, "Result");

    // Interface implementation calling itself on its own entity: Sum = N + Down(N - 1), Down(0) = 0.
    b.g.interfaces = {"Counter"};
    CHECK(b.g.AddFunction("Down"));
    b.g.FindFunction("Down")->inputs  = {{"N", PinType::Int}};
    b.g.FindFunction("Down")->outputs = {{"Sum", PinType::Int}};
    b.g.FunctionSignatureChanged("Down");
    const std::uint32_t dEntry = b.Find("Function.Entry", "Down");
    const std::uint32_t dRet0  = b.Find("Function.Return", "Down");
    const std::uint32_t dBr    = b.Node("Flow.Branch", {}, "Down");
    const std::uint32_t dLess  = b.Node("Compare.LessInt", {}, "Down");
    b.Set(dLess, "B", std::int32_t{1});
    b.Link(dEntry, "N", dLess, "A");
    b.Link(dLess, "Result", dBr, "Condition");
    b.Link(dEntry, "Then", dBr, "In");
    b.Link(dBr, "True", dRet0, "In");
    const std::uint32_t dSub = b.Node("Math.SubtractInt", {}, "Down");
    b.Set(dSub, "B", std::int32_t{1});
    b.Link(dEntry, "N", dSub, "A");
    const std::uint32_t dCall = b.Node("Interface.Call", "Counter.Down", "Down");
    b.Link(dSub, "Result", dCall, "N");
    b.Link(dBr, "False", dCall, "In");
    const std::uint32_t dAdd = b.Node("Math.AddInt", {}, "Down");
    b.Link(dEntry, "N", dAdd, "A");
    b.Link(dCall, "Sum", dAdd, "B");
    const std::uint32_t dRet = b.Node("Function.Return", "Down", "Down");
    b.Link(dCall, "Then", dRet, "In");
    b.Link(dAdd, "Result", dRet, "Sum");

    // Pure function without an end: stopped at the depth limit, reported, no crash.
    CHECK(b.g.AddFunction("Forever"));
    b.g.FindFunction("Forever")->inputs  = {{"N", PinType::Int}};
    b.g.FindFunction("Forever")->outputs = {{"R", PinType::Int}};
    b.g.SetFunctionPure("Forever", true);
    b.g.FunctionSignatureChanged("Forever");
    const std::uint32_t fCall = b.Node("Function.CallPure", "Forever", "Forever");
    b.Link(b.Find("Function.Entry", "Forever"), "N", fCall, "N");
    b.Link(fCall, "R", b.Find("Function.Return", "Forever"), "R");

    const std::uint32_t begin = b.Node("Event.BeginPlay");
    const std::uint32_t call  = b.Node("Function.Call", "Fact");
    b.Set(call, "N", std::int32_t{5});
    const std::uint32_t p1 = Print(b);
    b.Link(begin, "Out", call, "In");
    b.Link(call, "Then", p1, "In");
    b.Link(call, "Result", p1, "Text");
    const std::uint32_t down = b.Node("Interface.Call", "Counter.Down");
    b.Set(down, "N", std::int32_t{4});
    const std::uint32_t p2 = Print(b);
    b.Link(p1, "Then", down, "In");
    b.Link(down, "Then", p2, "In");
    b.Link(down, "Sum", p2, "Text");
    const std::uint32_t forever = b.Node("Function.CallPure", "Forever");
    const std::uint32_t p3      = Print(b);
    b.Link(p2, "Then", p3, "In");
    b.Link(forever, "R", p3, "Text");
    CHECK(b.Valid());

    Runner r;
    (void)r.Add("Rec", "rec.ugraph", b.g);
    r.scripts.Begin(r.scene);
    CHECK(r.Printed("120") && r.Printed("10"));
    CHECK(r.scripts.Stats().errors >= 1); // the endless pure recursion
    // Still usable afterwards (depth counters went back to zero).
    r.Run(0.2f);
    r.scripts.End(r.scene);
}

TEST_CASE(Blueprint3_Inheritance)
{
    ScriptRegistry::Clear();
    ScriptRegistry::AddInterface({"Named", {{"GetName", {}, {{"Name", PinType::String}}}}, {}});
    std::map<std::string, ScriptGraph> files; // in-memory graph files for ResolveScriptParent
    const auto loader = [&](const std::filesystem::path& f) -> std::optional<ScriptGraph> {
        const auto it = files.find(f.lexically_normal().generic_string());
        return it != files.end() ? std::optional<ScriptGraph>(it->second) : std::nullopt;
    };

    // Base: Health 100, Describe() -> "base", GetName (interface), BeginPlay prints B, Describe, Health.
    Graph base;
    base.g.variables.push_back({"Health", PinType::Int, std::int32_t{100}, false});
    base.g.interfaces = {"Named"};
    CHECK(base.g.AddFunction("Describe"));
    base.g.FindFunction("Describe")->outputs = {{"Text", PinType::String}};
    base.g.FunctionSignatureChanged("Describe");
    base.Set(base.Find("Function.Return", "Describe"), "Text", std::string("base"));
    CHECK(base.g.AddFunction("GetName"));
    base.g.FindFunction("GetName")->outputs = {{"Name", PinType::String}};
    base.g.FunctionSignatureChanged("GetName");
    {
        const std::uint32_t begin = base.Node("Event.BeginPlay");
        const std::uint32_t p1    = Print(base, "B");
        const std::uint32_t call  = base.Node("Function.Call", "Describe");
        const std::uint32_t p2    = Print(base);
        const std::uint32_t get   = base.Node("Variable.Get", "Health");
        const std::uint32_t p3    = Print(base);
        base.Link(begin, "Out", p1, "In");
        base.Link(p1, "Then", call, "In");
        base.Link(call, "Then", p2, "In");
        base.Link(call, "Text", p2, "Text");
        base.Link(p2, "Then", p3, "In");
        base.Link(get, "Value", p3, "Text");
    }
    CHECK(base.Valid());
    files["base.ugraph"] = base.g;

    // Child: Health default 50, Describe overridden (parent + "+child"), BeginPlay overridden
    // (C, parent's BeginPlay, IsA / Cast To / inherited interface, "C done").
    Graph child;
    child.g.parent             = "base.ugraph";
    child.g.defaults["Health"] = std::int32_t{50};
    CHECK(ResolveScriptParent(child.g, "child.ugraph", loader).empty() && child.g.inherited);
    CHECK(!child.g.AddFunction("Describe")); // the name is taken by the parent
    CHECK(child.g.OverrideFunction("Describe") && !child.g.OverrideFunction("Describe"));
    {
        const std::uint32_t parentCall = child.Find("Function.CallParent", "Describe");
        const std::uint32_t ret        = child.Find("Function.Return", "Describe");
        const std::uint32_t append     = child.Node("String.Append", {}, "Describe");
        child.Link(parentCall, "Text", append, "A");
        child.Set(append, "B", std::string("+child"));
        child.Link(append, "Result", ret, "Text");
    }
    const std::uint32_t cBegin = child.Node("Event.BeginPlay");
    {
        const std::uint32_t c1   = Print(child, "C");
        const std::uint32_t sup  = child.Node("Flow.CallParentEvent", "Event.BeginPlay");
        const std::uint32_t isA  = child.Node("Class.IsA", "base.ugraph");
        const std::uint32_t isA2 = child.Node("Class.IsA", "grand.ugraph");
        const std::uint32_t c2   = Print(child);
        const std::uint32_t c3   = Print(child);
        const std::uint32_t cast = child.Node("Class.CastTo", "child.ugraph");
        const std::uint32_t c4   = Print(child, "cast ok");
        const std::uint32_t impl = child.Node("Interface.Implements", "Named");
        const std::uint32_t c5   = Print(child);
        const std::uint32_t c6   = Print(child, "C done");
        child.Link(cBegin, "Out", c1, "In");
        child.Link(c1, "Then", sup, "In");
        child.Link(sup, "Then", c2, "In");
        child.Link(isA, "Result", c2, "Text");
        child.Link(c2, "Then", c3, "In");
        child.Link(isA2, "Result", c3, "Text");
        child.Link(c3, "Then", cast, "In");
        child.Link(cast, "Success", c4, "In");
        child.Link(c4, "Then", c5, "In");
        child.Link(impl, "Result", c5, "Text");
        child.Link(c5, "Then", c6, "In");
    }
    CHECK(child.Valid());
    // JSON keeps parent + defaults (not the resolved chain).
    const ScriptGraph reloaded = ScriptGraphFromJson(ScriptGraphToJson(child.g));
    CHECK(reloaded.parent == "base.ugraph" && !reloaded.inherited && reloaded.defaults.size() == 1 &&
          ValuesEqual(reloaded.defaults.at("Health"), ScriptValue(std::int32_t{50})));
    CHECK(HasError(reloaded, "is not loaded"));
    files["child.ugraph"] = child.g;

    // Grandchild: no own BeginPlay (the child's runs), Describe overridden again, Health 7.
    Graph grand;
    grand.g.parent             = "child.ugraph";
    grand.g.defaults["Health"] = std::int32_t{7};
    CHECK(ResolveScriptParent(grand.g, "grand.ugraph", loader).empty());
    CHECK(ScriptParentChain(grand.g).size() == 2);
    CHECK(grand.g.OverrideFunction("Describe"));
    {
        const std::uint32_t append = grand.Node("String.Append", {}, "Describe");
        grand.Link(grand.Find("Function.CallParent", "Describe"), "Text", append, "A");
        grand.Set(append, "B", std::string("+grand"));
        grand.Link(append, "Result", grand.Find("Function.Return", "Describe"), "Text");
    }
    CHECK(grand.Valid());

    {
        Runner r;
        (void)r.Add("Base", "base.ugraph", base.g);
        r.scripts.Begin(r.scene);
        CHECK(r.Printed("B") && r.Printed("base") && r.Printed("100") && !r.Printed("C"));
        r.scripts.End(r.scene);
    }
    {
        Runner r;
        r.scripts.Provide("base.ugraph", base.g);
        const Entity e = r.Add("Child", "child.ugraph", child.g);
        r.scripts.Begin(r.scene);
        CHECK(r.scripts.Stats().errors == 0);
        CHECK(r.Printed("C") && r.Printed("B") && r.Printed("base+child") && r.Printed("50") && !r.Printed("100"));
        CHECK(r.Printed("true") && r.Printed("false") && r.Printed("cast ok") && r.Printed("C done"));
        CHECK(ValuesEqual(r.Var("child.ugraph", e, "Health"), ScriptValue(std::int32_t{50})));
        // The debugger finds the instance under its parent class too.
        CHECK(r.scripts.InstancesOf("base.ugraph").size() == 1 && r.scripts.Watch("base.ugraph", e).has_value());
        r.scripts.End(r.scene);
    }
    {
        Runner r;
        r.scripts.Provide("base.ugraph", base.g);
        r.scripts.Provide("child.ugraph", child.g);
        (void)r.Add("Grand", "grand.ugraph", grand.g);
        r.scripts.Begin(r.scene);
        CHECK(r.scripts.Stats().errors == 0);
        CHECK(r.Printed("C") && r.Printed("B") && r.Printed("base+child+grand") && r.Printed("7") && r.Printed("C done"));
        CHECK(!r.Printed("false")); // IsA grand.ugraph is true here
        r.scripts.End(r.scene);
    }

    // Errors.
    {
        Graph bad;
        bad.g.parent = "base.ugraph";
        CHECK(ResolveScriptParent(bad.g, "bad.ugraph", loader).empty());
        bad.g.variables.push_back({"Health", PinType::Int, std::int32_t{1}, false});
        bad.g.defaults["Nope"] = 1.0f;
        CHECK(HasError(bad.g, "already defined in the parent class") && HasError(bad.g, "no such variable"));
        bad.g.variables.clear();
        bad.g.defaults.clear();
        CHECK(bad.g.OverrideFunction("Describe"));
        bad.g.FindFunction("Describe")->outputs.push_back({"Extra", PinType::Int});
        CHECK(HasError(bad.g, "overrides the parent's version"));
        bad.g.FindFunction("Describe")->outputs.pop_back();
        const std::uint32_t sup = bad.Node("Flow.CallParentEvent", "Event.BeginPlay");
        CHECK(HasError(bad.g, "does not override the event"));
        bad.g.RemoveNode(sup);
        (void)bad.Node("Flow.CallParentEvent", "Event.Tick");
        CHECK(HasError(bad.g, "No parent class handles"));
    }
    {
        Graph a, b2;
        a.g.parent  = "b.ugraph";
        b2.g.parent = "a.ugraph";
        files["a.ugraph"] = a.g;
        files["b.ugraph"] = b2.g;
        CHECK(ResolveScriptParent(a.g, "a.ugraph", loader).find("cycle") != std::string::npos);
        Graph missing;
        missing.g.parent = "nowhere.ugraph";
        CHECK(!ResolveScriptParent(missing.g, "m.ugraph", loader).empty() && !missing.g.inherited);
        Runner r;
        (void)r.Add("Missing", "m.ugraph", missing.g);
        r.scripts.Begin(r.scene);
        CHECK(r.scripts.Debug("m.ugraph") && !r.scripts.Debug("m.ugraph")->diagnostics.empty());
        r.scripts.End(r.scene);
    }
    CHECK(ScriptParentPath("Content/Scripts/Enemies/Orc.ugraph", "Content/Scripts/Enemy.ugraph") == "../Enemy.ugraph");
}

TEST_CASE(Blueprint3_LatentNodesInFunctions)
{
    ScriptRegistry::Clear();
    ScriptRegistry::AddInterface({"Waiter", {{"Later", {}, {{"Done", PinType::Bool}}}}, {}});
    Graph b;
    // AddLater(N) = N + 1, after a Delay; the local L keeps N across the wait (two calls overlap).
    CHECK(b.g.AddFunction("AddLater"));
    {
        ScriptFunction& f = *b.g.FindFunction("AddLater");
        f.inputs          = {{"N", PinType::Int}};
        f.outputs         = {{"R", PinType::Int}};
        f.locals          = {{"L", PinType::Int, std::int32_t{0}, false}};
        b.g.FunctionSignatureChanged("AddLater");
        const std::uint32_t entry = b.Find("Function.Entry", "AddLater");
        const std::uint32_t ret   = b.Find("Function.Return", "AddLater");
        const std::uint32_t set   = b.Node("Variable.Set", "L", "AddLater");
        const std::uint32_t delay = b.Node("Flow.Delay", {}, "AddLater");
        const std::uint32_t get   = b.Node("Variable.Get", "L", "AddLater");
        const std::uint32_t add   = b.Node("Math.AddInt", {}, "AddLater");
        b.Set(delay, "Duration", 0.5f);
        b.Set(add, "B", std::int32_t{1});
        std::erase_if(b.g.links, [&](const ScriptLink& l) { return l.fromNode == entry && l.toNode == ret; });
        b.Link(entry, "Then", set, "In");
        b.Link(entry, "N", set, "Value");
        b.Link(set, "Then", delay, "In");
        b.Link(delay, "Completed", ret, "In");
        b.Link(get, "Value", add, "A");
        b.Link(add, "Result", ret, "R");
    }
    // Moved(): a tween to (0, 0, 5) - the caller continues when it completes.
    CHECK(b.g.AddFunction("Moved"));
    {
        const std::uint32_t entry = b.Find("Function.Entry", "Moved");
        const std::uint32_t ret   = b.Find("Function.Return", "Moved");
        const std::uint32_t tween = b.Node("Tween.MoveTo", "Linear", "Moved");
        b.Set(tween, "Location", glm::vec3(0.0f, 0.0f, 5.0f));
        b.Set(tween, "Duration", 0.3f);
        std::erase_if(b.g.links, [&](const ScriptLink& l) { return l.fromNode == entry && l.toNode == ret; });
        b.Link(entry, "Then", tween, "In");
        b.Link(tween, "Completed", ret, "In");
    }
    // Debounce(): a Retriggerable Delay - per call (each call has its own countdown).
    CHECK(b.g.AddFunction("Debounce"));
    {
        const std::uint32_t entry = b.Find("Function.Entry", "Debounce");
        const std::uint32_t ret   = b.Find("Function.Return", "Debounce");
        const std::uint32_t delay = b.Node("Flow.RetriggerableDelay", {}, "Debounce");
        b.Set(delay, "Duration", 0.3f);
        std::erase_if(b.g.links, [&](const ScriptLink& l) { return l.fromNode == entry && l.toNode == ret; });
        b.Link(entry, "Then", delay, "In");
        b.Link(delay, "Completed", ret, "In");
    }
    // Waiter.Later (interface): waits too - an interface caller does not, the rest runs afterwards.
    b.g.interfaces = {"Waiter"};
    CHECK(b.g.AddFunction("Later"));
    {
        b.g.FindFunction("Later")->outputs = {{"Done", PinType::Bool}};
        b.g.FunctionSignatureChanged("Later");
        const std::uint32_t entry = b.Find("Function.Entry", "Later");
        const std::uint32_t ret   = b.Find("Function.Return", "Later");
        const std::uint32_t delay = b.Node("Flow.Delay", {}, "Later");
        const std::uint32_t p     = Print(b, "later done", "Later");
        b.Set(delay, "Duration", 0.2f);
        b.Set(ret, "Done", true);
        std::erase_if(b.g.links, [&](const ScriptLink& l) { return l.fromNode == entry && l.toNode == ret; });
        b.Link(entry, "Then", delay, "In");
        b.Link(delay, "Completed", p, "In");
        b.Link(p, "Then", ret, "In");
    }
    // Two BeginPlay chains overlap in AddLater (1 -> 2, 5 -> 6), then Debounce and Moved.
    for (const std::int32_t n : {1, 5}) {
        const std::uint32_t begin = b.Node("Event.BeginPlay");
        const std::uint32_t call  = b.Node("Function.Call", "AddLater");
        const std::uint32_t p     = Print(b);
        const std::uint32_t deb   = b.Node("Function.Call", "Debounce");
        const std::uint32_t p2    = Print(b, n == 1 ? "first debounced" : "second debounced");
        b.Set(call, "N", n);
        b.Link(begin, "Out", call, "In");
        b.Link(call, "Then", p, "In");
        b.Link(call, "R", p, "Text");
        b.Link(p, "Then", deb, "In");
        b.Link(deb, "Then", p2, "In");
    }
    {
        const std::uint32_t begin = b.Node("Event.BeginPlay");
        const std::uint32_t moved = b.Node("Function.Call", "Moved");
        const std::uint32_t p     = Print(b, "moved");
        const std::uint32_t later = b.Node("Interface.Call", "Waiter.Later");
        const std::uint32_t p2    = Print(b);
        b.Link(begin, "Out", later, "In");
        b.Link(later, "Then", p2, "In");
        b.Link(later, "Done", p2, "Text"); // false: the interface caller does not wait
        b.Link(p2, "Then", moved, "In");
        b.Link(moved, "Then", p, "In");
    }
    CHECK(b.Valid());
    // Pure functions stay synchronous.
    {
        Graph pure = b;
        pure.g.FindFunction("AddLater")->pure = true;
        CHECK(HasError(pure.g, "latent"));
    }

    Runner r;
    const Entity e = r.Add("Waiter", "latent.ugraph", b.g);
    r.scripts.Begin(r.scene);
    CHECK(r.Printed("false") && !r.Printed("2") && !r.Printed("moved"));
    r.Run(0.2f);
    CHECK(!r.Printed("2") && !r.Printed("6") && !r.Printed("moved"));
    r.Run(0.5f);
    CHECK(r.Printed("later done") && r.Printed("moved"));
    CHECK(std::abs(r.scene.GetRegistry().Get<Transform>(e).position.z - 5.0f) < 1e-3f);
    CHECK(r.Printed("2") && r.Printed("6")); // each call kept its own N
    r.Run(0.6f);
    CHECK(r.Printed("second debounced") && r.Printed("first debounced")); // latent nodes wait per call
    CHECK(r.scripts.Stats().errors == 0);
    // The functions are free again afterwards (no stale call stacks).
    r.scripts.End(r.scene);
}

TEST_CASE(Blueprint3_DispatcherSignatures)
{
    ScriptRegistry::Clear();
    Graph b;
    b.g.dispatchers.push_back({"OnHit", {{"Damage", PinType::Float}}});
    b.g.events.push_back({"Good", {{"Amount", PinType::Int}}}); // float -> int converts
    b.g.events.push_back({"Bad", {{"Who", PinType::Entity}}});
    // Self binding (target unconnected): checked when validating.
    const std::uint32_t begin = b.Node("Event.BeginPlay");
    const std::uint32_t bind  = b.Node("Dispatcher.Bind", "OnHit");
    b.Set(bind, "Event", std::string("Good"));
    b.Link(begin, "Out", bind, "In");
    const std::uint32_t good = b.Node("Event.Custom", "Good");
    const std::uint32_t pg   = Print(b);
    b.Link(good, "Out", pg, "In");
    b.Link(good, "Amount", pg, "Text");
    const std::uint32_t badEv = b.Node("Event.Custom", "Bad");
    b.Link(badEv, "Out", Print(b, "bad ran"), "In");
    const std::uint32_t call = b.Node("Dispatcher.Call", "OnHit");
    b.Set(call, "Damage", 7.0f);
    b.Link(bind, "Then", call, "In");
    CHECK(b.Valid());
    {
        Graph bad = b;
        bad.Set(bind, "Event", std::string("Bad"));
        CHECK(HasError(bad.g, "does not fit"));
        bad.Set(bind, "Event", std::string("Plain")); // undeclared: no parameters
        CHECK(HasError(bad.g, "passes 1 argument(s)"));
    }
    // Through a connected target the check happens when binding: refused, reported.
    const std::uint32_t self  = b.Node("Entity.Self");
    const std::uint32_t bind2 = b.Node("Dispatcher.Bind", "OnHit");
    b.Set(bind2, "Event", std::string("Bad"));
    b.Link(self, "Self", bind2, "Target");
    b.g.links.erase(std::ranges::find_if(b.g.links, [&](const ScriptLink& l) { return l.fromNode == bind && l.toNode == call; }));
    b.Link(bind, "Then", bind2, "In");
    b.Link(bind2, "Then", call, "In");
    CHECK(b.Valid());

    Runner r;
    (void)r.Add("Hitter", "disp.ugraph", b.g);
    r.scripts.Begin(r.scene);
    CHECK(r.Printed("7") && !r.Printed("bad ran"));
    CHECK(r.scripts.Stats().errors == 1);
    r.scripts.End(r.scene);
}
