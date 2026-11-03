#include "Test.h"
#include "BlueprintTestUtil.h"

#include "Engine/Script/ScriptRegistry.h"

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
