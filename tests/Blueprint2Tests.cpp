#include "Test.h"
#include "BlueprintTestUtil.h"

#include "Engine/Script/ScriptRegistry.h"

#include <filesystem>
#include <random>

using namespace BlueprintTest;

namespace {

namespace fs = std::filesystem;

// Color {Red, Green, Blue}; Item {count: int = 1, name: string = "x", color: Color = Blue}.
void RegisterTestTypes()
{
    ScriptRegistry::Clear();
    ScriptRegistry::AddEnum({"Color", {"Red", "Green", "Blue"}, {}});
    ScriptRegistry::AddStruct({"Item",
                               {{"count", PinType::Int, std::int32_t{1}},
                                {"name", PinType::String, std::string("x")},
                                {"color", PinType::Enum("Color"), std::int32_t{2}}},
                               {}});
}

fs::path TempDir(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / (std::string(name) + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE(Blueprint2_TypesValuesAndRegistry)
{
    RegisterTestTypes();
    // Type names round trip.
    for (const char* name : {"int", "intArray", "enum:Color", "struct:Item", "struct:Item[]", "enum:Color[]", "map:string:int",
                             "map:enum:Color:struct:Item", "map:entity:vec3"}) {
        const auto t = PinTypeFromString(name);
        CHECK(t && ToString(*t) == name);
    }
    CHECK(!PinTypeFromString("map:float:int") && !PinTypeFromString("struct:") && !PinTypeFromString("nonsense"));
    CHECK(DisplayName(*PinTypeFromString("map:string:struct:Item")) == "Map<string, Item>" &&
          DisplayName(PinType::Struct("Item")) == "Item" && DisplayName(ArrayOf(PinType::Enum("Color"))) == "Color[]");
    CHECK(CanConvert(PinType::Enum("Color"), PinType::Int) && CanConvert(PinType::Int, PinType::Enum("Color")) &&
          !CanConvert(PinType::Enum("Color"), PinType::Enum("Other")) && !CanConvert(PinType::Struct("Item"), PinType::Int) &&
          CanConvert(PinType::Struct("Item"), PinType::String));

    // Struct values: field defaults, copy on write, display with enum names.
    ScriptValue item = DefaultValue(PinType::Struct("Item"));
    CHECK(ValueFits(item, PinType::Struct("Item")) && ValuesEqual(*StructField(item, "count"), std::int32_t{1}));
    ScriptValue copy = item;
    SetStructField(copy, "Item", "count", std::int32_t{5});
    CHECK(ValuesEqual(*StructField(item, "count"), std::int32_t{1}) && ValuesEqual(*StructField(copy, "count"), std::int32_t{5}));
    CHECK(ToDisplayString(copy) == "{count: 5, name: x, color: Blue}");
    CHECK(std::get<std::string>(ConvertFrom(std::int32_t{1}, PinType::Enum("Color"), PinType::String)) == "Green");

    // Maps: sorted keys, copy on write, deep equality.
    ScriptValue map = DefaultValue(PinType::Map(PinType::String, PinType::Int));
    MutableMap(map, PinType::String, PinType::Int).items[std::string("b")] = std::int32_t{2};
    MutableMap(map, PinType::String, PinType::Int).items[std::string("a")] = std::int32_t{1};
    ScriptValue map2 = map;
    CHECK(ValuesEqual(map, map2) && ToDisplayString(map) == "{a: 1, b: 2}");
    MutableMap(map2, PinType::String, PinType::Int).items.erase(std::string("a"));
    CHECK(MapOf(map).items.size() == 2 && MapOf(map2).items.size() == 1 && !ValuesEqual(map, map2));

    // Graph variables of the new types survive JSON.
    ScriptGraph g;
    g.variables.push_back({"Inventory", PinType::Map(PinType::String, PinType::Struct("Item")),
                           MakeMap(PinType::String, PinType::Struct("Item"))});
    MutableMap(g.variables.back().value, PinType::String, PinType::Struct("Item")).items[std::string("sword")] = copy;
    g.variables.push_back({"Paint", PinType::Enum("Color"), std::int32_t{1}});
    g.variables.push_back({"Items", ArrayOf(PinType::Struct("Item")), MakeArray(PinType::Struct("Item"), {item, copy})});
    const ScriptGraph back = ScriptGraphFromJson(ScriptGraphToJson(g));
    CHECK(back.variables.size() == 3);
    for (std::size_t i = 0; i < 3 && i < back.variables.size(); ++i)
        CHECK(back.variables[i].type == g.variables[i].type && ValuesEqual(back.variables[i].value, g.variables[i].value));
    CHECK(std::ranges::none_of(ValidateScriptGraph(back), [](const ScriptDiagnostic& d) { return d.error; }));

    // Unknown types are errors.
    ScriptGraph bad;
    bad.variables.push_back({"X", PinType::Struct("Nope"), MakeStruct("Nope")});
    CHECK(HasError(bad, "unknown type"));

    // Files: save, load a directory, validate.
    const fs::path dir = TempDir("ungine_types_");
    fs::create_directories(dir / "Sub");
    ScriptRegistry::SaveEnumFile(dir / "Color.uenum", *ScriptRegistry::FindEnum("Color"));
    ScriptRegistry::Clear();
    ScriptRegistry::SaveStructFile(dir / "Sub" / "Item.ustruct", ScriptStructDef{"Item",
                                                                                 {{"count", PinType::Int, std::int32_t{3}},
                                                                                  {"color", PinType::Enum("Color"), std::int32_t{1}}},
                                                                                 {}});
    CHECK(ScriptRegistry::LoadDirectory(dir).empty());
    CHECK(ScriptRegistry::FindEnum("Color") && ScriptRegistry::FindStruct("Item") &&
          ScriptRegistry::FindStruct("Item")->fields.size() == 2 && ScriptRegistry::Validate().empty());
    CHECK(ValuesEqual(*StructField(MakeStruct("Item"), "count"), std::int32_t{3}));
    ScriptRegistry::AddStruct({"Loop", {{"self", PinType::Struct("Loop"), std::int32_t{0}}}, {}});
    ScriptRegistry::AddStruct({"Broken", {{"x", PinType::Struct("Missing"), std::int32_t{0}}}, {}});
    const auto problems = ScriptRegistry::Validate();
    CHECK(std::ranges::any_of(problems, [](const std::string& p) { return p.find("contains itself") != std::string::npos; }) &&
          std::ranges::any_of(problems, [](const std::string& p) { return p.find("unknown type") != std::string::npos; }));
    std::error_code ec;
    fs::remove_all(dir, ec);
    ScriptRegistry::Clear();
}

TEST_CASE(Blueprint2_StructEnumMapNodes)
{
    RegisterTestTypes();
    Graph g;
    g.g.variables.push_back({"Stock", PinType::Map(PinType::String, PinType::Int), MakeMap(PinType::String, PinType::Int)});
    const std::uint32_t begin = g.Node("Event.BeginPlay");
    // Make Item (count 7, color Green) -> Set name -> Break -> print "7 apple Green".
    const std::uint32_t make = g.Node("Struct.Make", "Item");
    g.Set(make, "count", std::int32_t{7});
    const std::uint32_t green = g.Node("Enum.Literal", "Color.Green");
    g.Link(green, "Value", make, "color");
    const std::uint32_t setName = g.Node("Struct.SetField", "Item.name");
    g.Set(setName, "Value", std::string("apple"));
    g.Link(make, "Struct", setName, "Struct");
    const std::uint32_t brk = g.Node("Struct.Break");
    g.Link(setName, "Struct", brk, "Struct"); // Break adopts the struct type
    CHECK(g.g.FindNode(brk)->param == "Item");
    const std::uint32_t a1 = g.Node("String.Append"), a2 = g.Node("String.Append"), a3 = g.Node("String.Append"),
                        a4 = g.Node("String.Append");
    g.Link(brk, "count", a1, "A");
    g.Set(a1, "B", std::string(" "));
    g.Link(a1, "Result", a2, "A");
    g.Link(brk, "name", a2, "B");
    g.Link(a2, "Result", a3, "A");
    g.Set(a3, "B", std::string(" "));
    g.Link(a3, "Result", a4, "A");
    g.Link(brk, "color", a4, "B"); // enum -> string: the value's name
    const std::uint32_t p1 = Print(g);
    g.Link(begin, "Out", p1, "In");
    g.Link(a4, "Result", p1, "Text");
    // Switch on the enum.
    const std::uint32_t sw = g.Node("Enum.Switch");
    g.Link(brk, "color", sw, "Selection");
    CHECK(g.g.FindNode(sw)->param == "Color");
    const std::uint32_t pGreen = Print(g, "is green"), pOther = Print(g, "not green");
    g.Link(p1, "Then", sw, "In");
    g.Link(sw, "Green", pGreen, "In");
    g.Link(sw, "Red", pOther, "In");
    // Map: Add a=1, b=2, Remove a, then Find b / Contains a / Length.
    const std::uint32_t add1 = g.Node("Map.Add", "Stock"), add2 = g.Node("Map.Add", "Stock"), rem = g.Node("Map.Remove", "Stock");
    g.Set(add1, "Key", std::string("a"));
    g.Set(add1, "Value", std::int32_t{1});
    g.Set(add2, "Key", std::string("b"));
    g.Set(add2, "Value", std::int32_t{2});
    g.Set(rem, "Key", std::string("a"));
    g.Link(pGreen, "Then", add1, "In");
    g.Link(add1, "Then", add2, "In");
    g.Link(add2, "Then", rem, "In");
    const std::uint32_t stock = g.Node("Variable.Get", "Stock");
    const std::uint32_t find  = g.Node("Map.Find");
    g.Link(stock, "Value", find, "Map");
    CHECK(g.g.FindNode(find)->param == "map:string:int");
    g.Set(find, "Key", std::string("b"));
    const std::uint32_t len = g.Node("Map.Length"), keys = g.Node("Map.Keys");
    g.Link(stock, "Value", len, "Map");
    g.Link(stock, "Value", keys, "Map");
    const std::uint32_t p2 = Print(g), p3 = Print(g), p4 = Print(g);
    g.Link(rem, "Then", p2, "In");
    g.Link(find, "Value", p2, "Text");
    g.Link(p2, "Then", p3, "In");
    g.Link(len, "Length", p3, "Text");
    g.Link(p3, "Then", p4, "In");
    g.Link(keys, "Keys", p4, "Text");
    CHECK(g.Valid());

    Runner r;
    r.Add("Stocker", "types.ugraph", g.g);
    r.scripts.Begin(r.scene);
    CHECK(r.Printed("7 apple Green") && r.Printed("is green") && !r.Printed("not green"));
    CHECK(r.Printed("2") && r.Printed("1") && r.Printed("[b]") && r.scripts.Stats().errors == 0);
    r.scripts.End(r.scene);

    // Unknown struct / enum params and map nodes on non-maps are errors.
    Graph b;
    b.Node("Struct.Make", "Nope");
    b.Node("Enum.Literal", "Color.Purple");
    b.g.variables.push_back({"NotAMap", PinType::Int, std::int32_t{0}});
    b.Node("Map.Add", "NotAMap");
    CHECK(HasError(b.g, "Unknown struct") && HasError(b.g, "Unknown enum value") && HasError(b.g, "is not a map"));
    ScriptRegistry::Clear();
}

TEST_CASE(Blueprint2_SwitchSelectMultiGateDelayAndPureCache)
{
    Graph g;
    const std::uint32_t begin = g.Node("Event.BeginPlay");
    // Switch on Int (2) and String ("b").
    const std::uint32_t si = g.Node("Flow.SwitchInt", "0, 1, 2"), ss = g.Node("Flow.SwitchString", "a, b");
    g.Set(si, "Selection", std::int32_t{2});
    g.Set(ss, "Selection", std::string("b"));
    const std::uint32_t pi = Print(g, "int two"), pd = Print(g, "int default"), ps = Print(g, "string b");
    g.Link(begin, "Out", si, "In");
    g.Link(si, "2", pi, "In");
    g.Link(si, "Default", pd, "In");
    g.Link(pi, "Then", ss, "In");
    g.Link(ss, "b", ps, "In");
    // Select (index 1 of three strings) -> print.
    const std::uint32_t sel = g.Node("Utility.Select", "string:3");
    g.Set(sel, "Index", std::int32_t{1});
    g.Set(sel, "Option 0", std::string("zero"));
    g.Set(sel, "Option 1", std::string("one"));
    const std::uint32_t psel = Print(g);
    g.Link(ps, "Then", psel, "In");
    g.Link(sel, "Result", psel, "Text");
    // Pure cache: one Random read twice by one Append -> both halves equal.
    const std::uint32_t rnd = g.Node("Math.RandomInt"), app = g.Node("String.Append");
    g.Set(rnd, "Max", std::int32_t{1000000});
    g.Link(rnd, "Result", app, "A");
    g.Link(rnd, "Result", app, "B");
    g.g.variables.push_back({"Pair", PinType::String, std::string()});
    const std::uint32_t setPair = g.Node("Variable.Set", "Pair");
    g.Link(psel, "Then", setPair, "In");
    g.Link(app, "Result", setPair, "Value");
    // Multi Gate: Custom "Gate" fires it; outputs print 0 / 1 / 2.
    const std::uint32_t ev = g.Node("Event.Custom", "Gate"), mg = g.Node("Flow.MultiGate", "3");
    g.Link(ev, "Out", mg, "In");
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t p = Print(g, ("gate " + std::to_string(i)).c_str());
        g.Link(mg, ("Out " + std::to_string(i)).c_str(), p, "In");
    }
    // Retriggerable Delay on "Poke": completes once, 1 s after the last poke.
    const std::uint32_t poke = g.Node("Event.Custom", "Poke"), rd = g.Node("Flow.RetriggerableDelay");
    g.g.variables.push_back({"Done", PinType::Int, std::int32_t{0}});
    const std::uint32_t getDone = g.Node("Variable.Get", "Done"), inc = g.Node("Math.AddInt"), setDone = g.Node("Variable.Set", "Done");
    g.Set(inc, "B", std::int32_t{1});
    g.Link(poke, "Out", rd, "In");
    g.Link(rd, "Completed", setDone, "In");
    g.Link(getDone, "Value", inc, "A");
    g.Link(inc, "Result", setDone, "Value");
    CHECK(g.Valid());

    Runner       r;
    const Entity e = r.Add("Flow", "flow.ugraph", g.g);
    r.scripts.Begin(r.scene);
    CHECK(r.Printed("int two") && !r.Printed("int default") && r.Printed("string b") && r.Printed("one"));
    const std::string pair = std::get<std::string>(r.Var("flow.ugraph", e, "Pair"));
    CHECK(pair.size() % 2 == 0 && pair.substr(0, pair.size() / 2) == pair.substr(pair.size() / 2));

    // Multi gate: 0, 1, 2 in order, then nothing (no loop).
    Graph trigger; // calls Gate 4 times and Poke at 0 and 0.5 s
    {
        const std::uint32_t b2 = trigger.Node("Event.BeginPlay");
        const std::uint32_t loop = trigger.Node("Flow.ForLoop");
        trigger.Set(loop, "First Index", std::int32_t{1});
        trigger.Set(loop, "Last Index", std::int32_t{4});
        const std::uint32_t callGate = trigger.Node("Script.CallEvent");
        trigger.Set(callGate, "Event", std::string("Gate"));
        const std::uint32_t find = trigger.Node("Entity.FindByName");
        trigger.Set(find, "Name", std::string("Flow"));
        trigger.Link(b2, "Out", loop, "In");
        trigger.Link(loop, "Loop Body", callGate, "In");
        trigger.Link(find, "Entity", callGate, "Target");
        const std::uint32_t poke1 = trigger.Node("Script.CallEvent");
        trigger.Set(poke1, "Event", std::string("Poke"));
        trigger.Link(find, "Entity", poke1, "Target");
        trigger.Link(loop, "Completed", poke1, "In");
        const std::uint32_t delay = trigger.Node("Flow.Delay"), poke2 = trigger.Node("Script.CallEvent");
        trigger.Set(delay, "Duration", 0.5f);
        trigger.Set(poke2, "Event", std::string("Poke"));
        trigger.Link(find, "Entity", poke2, "Target");
        trigger.Link(poke1, "Then", delay, "In");
        trigger.Link(delay, "Completed", poke2, "In");
        CHECK(trigger.Valid());
    }
    r.scripts.End(r.scene);
    r.scripts.Provide("trigger.ugraph", trigger.g);
    r.scene.GetRegistry().Emplace<ScriptComponent>(r.scene.CreateEntity("Trigger"), ScriptComponent{"trigger.ugraph"});
    r.scripts.Begin(r.scene);
    const auto count = [&](const char* text) {
        return std::ranges::count_if(r.scripts.Messages(), [&](const ScriptMessage& m) { return m.text == text; });
    };
    CHECK(count("gate 0") == 1 && count("gate 1") == 1 && count("gate 2") == 1);
    r.Run(1.2f); // poke at 0 and 0.5: done once at 1.5
    CHECK(ValuesEqual(r.Var("flow.ugraph", e, "Done"), std::int32_t{0}));
    r.Run(0.6f);
    CHECK(ValuesEqual(r.Var("flow.ugraph", e, "Done"), std::int32_t{1}));
    r.Run(2.0f);
    CHECK(ValuesEqual(r.Var("flow.ugraph", e, "Done"), std::int32_t{1}) && r.scripts.Stats().errors == 0);
    r.scripts.End(r.scene);
}
