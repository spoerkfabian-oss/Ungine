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
