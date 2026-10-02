#include "Test.h"
#include "BlueprintTestUtil.h"

#include "Engine/Core/Input.h"
#include "Engine/Core/Project.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptCondition.h"
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

namespace {

// Sets variable `name` from (node, pin) after `after` fired; returns the Set node.
std::uint32_t SetVar(Graph& g, std::uint32_t after, const char* afterPin, const char* name, std::uint32_t from,
                     const char* fromPin, std::string function = {})
{
    const std::uint32_t set = g.Node("Variable.Set", name, std::move(function));
    g.Link(after, afterPin, set, "In");
    g.Link(from, fromPin, set, "Value");
    return set;
}

float FloatOf(const ScriptValue& v) { return std::get<float>(Convert(v, PinType::Float)); }
std::int32_t IntOf(const ScriptValue& v) { return std::get<std::int32_t>(Convert(v, PinType::Int)); }

// Library "MathLib": Triple(X) -> Y = 3 X (pure), Report(Msg) prints it, macro TwiceLib(In, V) ->
// (Out, R = Triple(V)) calling the library's own function.
void RegisterMathLib(bool broken = false)
{
    Graph l;
    l.g.library = true;
    CHECK(l.g.AddFunction("Triple"));
    ScriptFunction* t = l.g.FindFunction("Triple");
    t->inputs  = {{"X", PinType::Float}};
    t->outputs = {{"Y", PinType::Float}};
    l.g.SetFunctionPure("Triple", true);
    const std::uint32_t mul = l.Node("Math.MultiplyFloat", {}, "Triple");
    l.Set(mul, "B", 3.0f);
    l.Link(l.Find("Function.Entry", "Triple"), "X", mul, "A");
    l.Link(mul, "Result", l.Find("Function.Return", "Triple"), "Y");
    CHECK(l.g.AddFunction("Report"));
    l.g.FindFunction("Report")->inputs = {{"Msg", PinType::String}};
    l.g.FunctionSignatureChanged("Report");
    const std::uint32_t print = Print(l, "", "Report");
    l.Link(l.Find("Function.Entry", "Report"), "Then", print, "In");
    l.Link(l.Find("Function.Entry", "Report"), "Msg", print, "Text");
    l.Link(print, "Then", l.Find("Function.Return", "Report"), "In");
    CHECK(l.g.AddMacro("TwiceLib"));
    ScriptMacro* m = l.g.FindMacro("TwiceLib");
    m->inputs.push_back({"V", PinType::Float});
    m->outputs.push_back({"R", PinType::Float});
    const std::uint32_t triple = l.Node("Function.CallPure", "Triple", "TwiceLib");
    l.Link(l.Find("Macro.Inputs", "TwiceLib"), "V", triple, "X");
    l.Link(triple, "Y", l.Find("Macro.Outputs", "TwiceLib"), "R");
    if (broken)
        l.Node("Variable.Get", "Missing", "Report");
    CHECK(broken || l.Valid());
    ScriptRegistry::AddLibrary({"MathLib", l.g, {}});
}

} // namespace

TEST_CASE(Blueprint2_MacrosAndLibraries)
{
    ScriptRegistry::Clear();
    RegisterMathLib();
    Graph g;
    for (const char* v : {"r1", "r2", "r3", "r4", "r5"})
        g.g.variables.push_back({v, PinType::Float, 0.0f, false});
    g.g.variables.push_back({"counter", PinType::Int, std::int32_t{0}, false});
    g.g.variables.push_back({"n", PinType::Int, std::int32_t{5}, false});
    // Twice(In, Value) -> (Out, Result = Value + Value); counts its runs in a graph variable.
    CHECK(g.g.AddMacro("Twice") && !g.g.AddMacro("Twice") && !g.g.AddMacro("bad.name"));
    CHECK(!g.g.AddFunction("Twice"));
    ScriptMacro* m = g.g.FindMacro("Twice");
    m->inputs.push_back({"Value", PinType::Float});
    m->outputs.push_back({"Result", PinType::Float});
    const std::uint32_t in = g.Find("Macro.Inputs", "Twice"), out = g.Find("Macro.Outputs", "Twice");
    const std::uint32_t sum = g.Node("Math.AddFloat", {}, "Twice");
    g.Link(in, "Value", sum, "A");
    g.Link(in, "Value", sum, "B");
    g.Link(sum, "Result", out, "Result");
    g.g.Disconnect(in, "In", true);
    const std::uint32_t count = g.Node("Variable.Get", "counter", "Twice"), inc = g.Node("Math.AddInt", {}, "Twice");
    g.Set(inc, "B", std::int32_t{1});
    g.Link(count, "Value", inc, "A");
    const std::uint32_t bump = SetVar(g, in, "In", "counter", inc, "Result", "Twice");
    g.Link(bump, "Then", out, "Out");

    // BeginPlay -> Twice(1.5) -> r1 -> Twice(r1) -> r2 -> Twice(n: int, through a float pin) -> r3
    //   -> MathLib.Report("lib hello") -> MathLib.TwiceLib(2) -> r4 -> r5 = MathLib.Triple(5).
    const std::uint32_t begin = g.Node("Event.BeginPlay");
    const std::uint32_t u1 = g.Node("Macro.Use", "Twice"), u2 = g.Node("Macro.Use", "Twice"), u3 = g.Node("Macro.Use", "Twice");
    g.Set(u1, "Value", 1.5f);
    g.Link(begin, "Out", u1, "In");
    const std::uint32_t s1 = SetVar(g, u1, "Out", "r1", u1, "Result");
    g.Link(s1, "Then", u2, "In");
    g.Link(g.Node("Variable.Get", "r1"), "Value", u2, "Value");
    const std::uint32_t s2 = SetVar(g, u2, "Out", "r2", u2, "Result");
    g.Link(s2, "Then", u3, "In");
    g.Link(g.Node("Variable.Get", "n"), "Value", u3, "Value");
    const std::uint32_t s3     = SetVar(g, u3, "Out", "r3", u3, "Result");
    const std::uint32_t report = g.Node("Library.Call", "MathLib.Report");
    g.Set(report, "Msg", std::string("lib hello"));
    g.Link(s3, "Then", report, "In");
    const std::uint32_t lib = g.Node("Macro.Use", "MathLib.TwiceLib");
    g.Set(lib, "V", 2.0f);
    g.Link(report, "Then", lib, "In");
    const std::uint32_t s4     = SetVar(g, lib, "Out", "r4", lib, "R");
    const std::uint32_t triple = g.Node("Library.CallPure", "MathLib.Triple");
    g.Set(triple, "X", 5.0f);
    SetVar(g, s4, "Then", "r5", triple, "Y");
    CHECK(g.Valid());
    CHECK(ScriptGraphFromJson(ScriptGraphToJson(g.g)).macros.size() == 1);

    {
        Runner r;
        const Entity e = r.Add("M", "macros.ugraph", g.g);
        r.scripts.Begin(r.scene);
        CHECK(FloatOf(r.Var("macros.ugraph", e, "r1")) == 3.0f && FloatOf(r.Var("macros.ugraph", e, "r2")) == 6.0f &&
              FloatOf(r.Var("macros.ugraph", e, "r3")) == 10.0f && IntOf(r.Var("macros.ugraph", e, "counter")) == 3);
        CHECK(FloatOf(r.Var("macros.ugraph", e, "r4")) == 6.0f && FloatOf(r.Var("macros.ugraph", e, "r5")) == 15.0f);
        CHECK(r.Printed("lib hello"));
        // The debugger sees the Macro nodes and the macro's own nodes run (a macro without impure
        // nodes, like TwiceLib, has nothing that runs); library nodes under the library.
        const ScriptDebugInfo* info = r.scripts.Debug("macros.ugraph");
        CHECK(info && info->nodeTimes.contains(u1) && info->nodeTimes.contains(u3) && info->nodeTimes.contains(bump) &&
              info->nodeTimes.contains(report) && !info->nodeTimes.contains(count));
        const ScriptDebugInfo* libInfo = r.scripts.Debug("library:MathLib");
        CHECK(libInfo && !libInfo->nodeTimes.empty());
        r.scripts.End(r.scene);
    }

    // Validation.
    {
        Graph bad = g;
        bad.Node("Macro.Use", "Nope");
        CHECK(HasError(bad.g, "Unknown macro"));
    }
    {
        Graph bad = g; // a macro using itself
        bad.Node("Macro.Use", "Twice", "Twice");
        CHECK(HasError(bad.g, "uses itself"));
    }
    {
        Graph bad = g; // latent macro inside a function
        CHECK(bad.g.AddMacro("Wait"));
        const std::uint32_t delay = bad.Node("Flow.Delay", {}, "Wait");
        bad.Link(bad.Find("Macro.Inputs", "Wait"), "In", delay, "In");
        CHECK(bad.g.AddFunction("F"));
        const std::uint32_t use = bad.Node("Macro.Use", "Wait", "F");
        bad.Link(bad.Find("Function.Entry", "F"), "Then", use, "In");
        CHECK(HasError(bad.g, "contains latent nodes"));
        CHECK(!HasError(g.g, "contains latent nodes"));
    }
    {
        Graph bad = g;
        bad.g.FindNode(triple)->type = "Library.Call"; // pure function with exec pins
        CHECK(HasError(bad.g, "is pure"));
        bad.g.FindNode(triple)->param = "MathLib.Nope";
        CHECK(HasError(bad.g, "Unknown library function"));
    }
    {
        Graph bad = g; // a second Inputs node
        bad.Node("Macro.Inputs", "Twice", "Twice");
        CHECK(HasError(bad.g, "exactly one"));
    }
    {
        ScriptGraph library;
        library.library = true;
        library.variables.push_back({"x", PinType::Int, std::int32_t{0}, false});
        library.AddNode("Event.BeginPlay", {});
        CHECK(HasError(library, "only functions and macros") && HasError(library, "no event graph"));
    }
    // A broken library: its users do not run.
    RegisterMathLib(true);
    CHECK(HasError(g.g, "Library 'MathLib' has errors"));
    CHECK(!ScriptRegistry::Validate().empty());
    ScriptRegistry::Clear();
    CHECK(HasError(g.g, "Unknown library function") && HasError(g.g, "Unknown macro"));
}

TEST_CASE(Blueprint2_InterfacesAndDispatchers)
{
    ScriptRegistry::Clear();
    ScriptRegistry::AddInterface({"Damageable", {{"TakeDamage", {{"Amount", PinType::Float}}, {{"Remaining", PinType::Float}}}}, {}});
    CHECK(ScriptRegistry::Validate().empty());

    // Target: hp, implements TakeDamage (hp -= Amount, calls OnDamaged(Amount)).
    Graph t;
    t.g.variables.push_back({"hp", PinType::Float, 100.0f, false});
    t.g.interfaces.push_back("Damageable");
    t.g.dispatchers.push_back({"OnDamaged", {{"Amount", PinType::Float}}});
    CHECK(HasError(t.g, "is not implemented"));
    CHECK(t.g.AddFunction("TakeDamage"));
    ScriptFunction* f = t.g.FindFunction("TakeDamage");
    f->inputs  = {{"Amount", PinType::Float}};
    f->outputs = {{"Remaining", PinType::Int}};
    t.g.FunctionSignatureChanged("TakeDamage");
    CHECK(HasError(t.g, "differ from the interface"));
    f->outputs = {{"Remaining", PinType::Float}};
    t.g.FunctionSignatureChanged("TakeDamage");
    const std::uint32_t entry = t.Find("Function.Entry", "TakeDamage"), ret = t.Find("Function.Return", "TakeDamage");
    const std::uint32_t sub = t.Node("Math.SubtractFloat", {}, "TakeDamage");
    t.Link(t.Node("Variable.Get", "hp", "TakeDamage"), "Value", sub, "A");
    t.Link(entry, "Amount", sub, "B");
    const std::uint32_t setHp = SetVar(t, entry, "Then", "hp", sub, "Result", "TakeDamage");
    const std::uint32_t call  = t.Node("Dispatcher.Call", "OnDamaged", "TakeDamage");
    t.Link(setHp, "Then", call, "In");
    t.Link(entry, "Amount", call, "Amount");
    t.Link(call, "Then", ret, "In");
    t.Link(setHp, "Value", ret, "Remaining");
    CHECK(t.Valid());

    // Attacker: binds Hurt(Amount) to the target's OnDamaged, damages it twice (unbinding in
    // between), asks whether target / itself implement Damageable, calls Hurt(5) itself.
    Graph a;
    a.g.variables.push_back({"Target", PinType::Entity, NullEntity, true});
    for (const char* v : {"remaining", "lastHurt"})
        a.g.variables.push_back({v, PinType::Float, 0.0f, false});
    a.g.variables.push_back({"hurtCount", PinType::Int, std::int32_t{0}, false});
    a.g.variables.push_back({"targetImplements", PinType::Bool, false, false});
    a.g.variables.push_back({"selfImplements", PinType::Bool, true, false});
    a.g.events.push_back({"Hurt", {{"Amount", PinType::Float}}});
    const std::uint32_t hurt = a.Node("Event.Custom", "Hurt");
    const std::uint32_t last = SetVar(a, hurt, "Out", "lastHurt", hurt, "Amount");
    const std::uint32_t inc  = a.Node("Math.AddInt");
    a.Set(inc, "B", std::int32_t{1});
    a.Link(a.Node("Variable.Get", "hurtCount"), "Value", inc, "A");
    SetVar(a, last, "Then", "hurtCount", inc, "Result");

    const std::uint32_t begin = a.Node("Event.BeginPlay"), target = a.Node("Variable.Get", "Target");
    const std::uint32_t bind = a.Node("Dispatcher.Bind", "OnDamaged");
    a.Set(bind, "Event", std::string("Hurt"));
    a.Link(begin, "Out", bind, "In");
    a.Link(target, "Value", bind, "Target");
    const std::uint32_t hit1 = a.Node("Interface.Call", "Damageable.TakeDamage");
    a.Set(hit1, "Amount", 30.0f);
    a.Link(bind, "Then", hit1, "In");
    a.Link(target, "Value", hit1, "Target");
    const std::uint32_t unbind = a.Node("Dispatcher.Unbind", "OnDamaged");
    a.Set(unbind, "Event", std::string("Hurt"));
    a.Link(hit1, "Then", unbind, "In");
    a.Link(target, "Value", unbind, "Target");
    const std::uint32_t hit2 = a.Node("Interface.Call", "Damageable.TakeDamage");
    a.Set(hit2, "Amount", 20.0f);
    a.Link(unbind, "Then", hit2, "In");
    a.Link(target, "Value", hit2, "Target");
    const std::uint32_t s1 = SetVar(a, hit2, "Then", "remaining", hit2, "Remaining");
    const std::uint32_t impl = a.Node("Interface.Implements", "Damageable"), self = a.Node("Interface.Implements", "Damageable");
    a.Link(target, "Value", impl, "Target");
    const std::uint32_t s2 = SetVar(a, s1, "Then", "targetImplements", impl, "Result");
    const std::uint32_t s3 = SetVar(a, s2, "Then", "selfImplements", self, "Result");
    const std::uint32_t own = a.Node("Interface.Call", "Damageable.TakeDamage"); // not implemented here: nothing
    a.Link(s3, "Then", own, "In");
    const std::uint32_t callHurt = a.Node("Flow.CallEvent", "Hurt");
    a.Set(callHurt, "Amount", 5.0f);
    a.Link(own, "Then", callHurt, "In");
    CHECK(a.Valid());
    {
        Graph bad = a;
        bad.Node("Dispatcher.Call", "Nope");
        CHECK(HasError(bad.g, "Unknown dispatcher"));
        bad.g.interfaces.push_back("Unknown");
        CHECK(HasError(bad.g, "Unknown interface 'Unknown'"));
    }
    const ScriptGraph back = ScriptGraphFromJson(ScriptGraphToJson(t.g));
    CHECK(back.interfaces == t.g.interfaces && back.dispatchers.size() == 1 && back.dispatchers[0].params.size() == 1);

    Runner       r;
    const Entity victim = r.Add("Victim", "target.ugraph", t.g);
    const Entity hero   = r.Add("Hero", "attacker.ugraph", a.g);
    r.scene.GetRegistry().Get<ScriptComponent>(hero).variables["Target"] = {NullEntity, r.scene.GetRegistry().Get<Uuid>(victim).value};
    r.scripts.Begin(r.scene);
    r.Run(0.2f);
    CHECK(FloatOf(r.Var("target.ugraph", victim, "hp")) == 50.0f);
    CHECK(FloatOf(r.Var("attacker.ugraph", hero, "remaining")) == 50.0f);
    CHECK(IntOf(r.Var("attacker.ugraph", hero, "hurtCount")) == 2); // bound once (30), then called directly (5)
    CHECK(FloatOf(r.Var("attacker.ugraph", hero, "lastHurt")) == 5.0f);
    CHECK(std::get<bool>(r.Var("attacker.ugraph", hero, "targetImplements")) &&
          !std::get<bool>(r.Var("attacker.ugraph", hero, "selfImplements")));
    CHECK(r.scripts.Stats().errors == 0);
    r.scripts.End(r.scene);
}

TEST_CASE(Blueprint2_TimelinesAndTweens)
{
    ScriptRegistry::Clear();
    CHECK(ScriptEase("Quad In", 0.5f) == 0.25f && ScriptEase("Linear", 0.3f) == 0.3f && ScriptEase("Bounce Out", 1.0f) == 1.0f &&
          ScriptEase("Sine In Out", 0.0f) == 0.0f && std::abs(ScriptEase("Back Out", 1.0f) - 1.0f) < 1e-5f);
    ScriptTimelineTrack linear{"Alpha", ScriptTrackKind::Float, {{0.0f, glm::vec3(0.0f)}, {1.0f, glm::vec3(10.0f)}}};
    CHECK(EvaluateTrack(linear, 0.25f).x == 2.5f && EvaluateTrack(linear, -1.0f).x == 0.0f && EvaluateTrack(linear, 3.0f).x == 10.0f);
    linear.keys[0].interp = ScriptInterp::Constant;
    CHECK(EvaluateTrack(linear, 0.9f).x == 0.0f);
    linear.keys[0].interp = ScriptInterp::Linear;

    Graph g;
    for (const char* v : {"alpha", "x", "tf"})
        g.g.variables.push_back({v, PinType::Float, 0.0f, false});
    for (const char* v : {"pings", "finished"})
        g.g.variables.push_back({v, PinType::Int, std::int32_t{0}, false});
    g.g.variables.push_back({"moved", PinType::Bool, false, false});
    g.g.variables.push_back({"tfDone", PinType::Bool, false, false});
    g.g.variables.push_back({"pos", PinType::Vec3, glm::vec3(0.0f), false});
    ScriptTimeline fade{"Fade", 1.0f, false, false, {}};
    fade.tracks.push_back(linear);
    fade.tracks.push_back({"Ping", ScriptTrackKind::Event, {{0.5f, glm::vec3(0.0f)}}});
    fade.tracks.push_back({"Pos", ScriptTrackKind::Vector, {{0.0f, glm::vec3(0.0f)}, {1.0f, glm::vec3(0.0f, 4.0f, 0.0f)}}});
    g.g.timelines.push_back(fade);
    const ScriptGraph back = ScriptGraphFromJson(ScriptGraphToJson(g.g));
    CHECK(back.timelines.size() == 1 && back.timelines[0].tracks.size() == 3 && back.timelines[0].tracks[2].keys[1].value.y == 4.0f &&
          back.timelines[0].tracks[1].kind == ScriptTrackKind::Event);

    const auto increment = [&](std::uint32_t after, const char* afterPin, const char* var) {
        const std::uint32_t add = g.Node("Math.AddInt");
        g.Set(add, "B", std::int32_t{1});
        g.Link(g.Node("Variable.Get", var), "Value", add, "A");
        return SetVar(g, after, afterPin, var, add, "Result");
    };
    // BeginPlay -> Fade from start: Update -> alpha, pos; Ping -> pings++; Finished -> finished++.
    const std::uint32_t begin = g.Node("Event.BeginPlay"), tl = g.Node("Timeline.Play", "Fade");
    g.Link(begin, "Out", tl, "Play from Start");
    const std::uint32_t setA = SetVar(g, tl, "Update", "alpha", tl, "Alpha");
    SetVar(g, setA, "Then", "pos", tl, "Pos");
    increment(tl, "Ping", "pings");
    increment(tl, "Finished", "finished");
    // Custom event Back: reverse from the end.
    g.Link(g.Node("Event.Custom", "Back"), "Out", tl, "Reverse from End");
    // Tweens: move self to (10, 0, 0) linearly in 1 s; Tween Float 0 -> 1 in 0.5 s.
    const std::uint32_t begin2 = g.Node("Event.BeginPlay"), move = g.Node("Tween.MoveTo", "Linear");
    g.Set(move, "Location", glm::vec3(10.0f, 0.0f, 0.0f));
    g.Link(begin2, "Out", move, "In");
    const std::uint32_t moved = g.Node("Variable.Set", "moved");
    g.Set(moved, "Value", true);
    g.Link(move, "Completed", moved, "In");
    const std::uint32_t tween = g.Node("Tween.Float", "Linear");
    g.Set(tween, "Duration", 0.5f);
    g.Link(moved, "Then", tween, "In");
    const std::uint32_t begin3 = g.Node("Event.BeginPlay"), tween2 = g.Node("Tween.Float", "Linear");
    g.Set(tween2, "Duration", 0.5f);
    g.Link(begin3, "Out", tween2, "In");
    SetVar(g, tween2, "Update", "tf", tween2, "Value");
    const std::uint32_t done = g.Node("Variable.Set", "tfDone");
    g.Set(done, "Value", true);
    g.Link(tween2, "Completed", done, "In");
    CHECK(g.Valid());
    {
        Graph bad = g; // timelines / tweens are latent: not in functions
        CHECK(bad.g.AddFunction("F"));
        bad.Node("Tween.MoveTo", "Linear", "F");
        CHECK(HasError(bad.g, "latent"));
        bad.g.timelines[0].tracks[0].name = "Update";
        CHECK(HasError(bad.g, "track names"));
    }

    Runner       r;
    const Entity e = r.Add("Mover", "tl.ugraph", g.g);
    r.scripts.Begin(r.scene);
    CHECK(r.scripts.Stats().ticking == 3);
    r.Run(0.5f);
    CHECK(std::abs(FloatOf(r.Var("tl.ugraph", e, "alpha")) - 5.0f) < 1e-3f && IntOf(r.Var("tl.ugraph", e, "pings")) == 1);
    CHECK(std::abs(r.scene.GetRegistry().Get<Transform>(e).position.x - 5.0f) < 1e-3f);
    CHECK(std::abs(FloatOf(r.Var("tl.ugraph", e, "tf")) - 1.0f) < 1e-4f && std::get<bool>(r.Var("tl.ugraph", e, "tfDone")));
    r.Run(0.6f);
    CHECK(FloatOf(r.Var("tl.ugraph", e, "alpha")) == 10.0f && IntOf(r.Var("tl.ugraph", e, "finished")) == 1 &&
          IntOf(r.Var("tl.ugraph", e, "pings")) == 1);
    CHECK(std::get<glm::vec3>(r.Var("tl.ugraph", e, "pos")).y == 4.0f);
    CHECK(r.scene.GetRegistry().Get<Transform>(e).position.x == 10.0f && std::get<bool>(r.Var("tl.ugraph", e, "moved")));
    CHECK(r.scripts.Stats().ticking == 1); // the second Tween Float runs now
    // Reverse from the end: back to 0, Ping again (passed backwards), Finished again.
    {
        Graph caller;
        const std::uint32_t b = caller.Node("Event.BeginPlay"), call = caller.Node("Script.CallEvent");
        caller.Set(call, "Event", std::string("Back"));
        caller.Link(b, "Out", call, "In");
        const std::uint32_t find = caller.Node("Entity.FindByName");
        caller.Set(find, "Name", std::string("Mover"));
        caller.Link(find, "Entity", call, "Target");
        CHECK(caller.Valid());
        r.Add("Caller", "caller.ugraph", caller.g);
    }
    r.Run(1.2f);
    CHECK(FloatOf(r.Var("tl.ugraph", e, "alpha")) == 0.0f && IntOf(r.Var("tl.ugraph", e, "finished")) == 2 &&
          IntOf(r.Var("tl.ugraph", e, "pings")) == 2);
    CHECK(r.scripts.Stats().ticking == 0 && r.scripts.Stats().errors == 0);
    r.scripts.End(r.scene);
}

TEST_CASE(Blueprint2_InputActionsSaveGameAndLevels)
{
    ScriptRegistry::Clear();
    // Input actions: Jump = Space or MouseLeft, axis MoveForward = W (+1) / S (-1).
    InputMap map;
    map.actions.push_back({"Jump", {"Space", "MouseLeft"}});
    map.axes.push_back({"MoveForward", {{"W", 1.0f}, {"S", -1.0f}}});
    Graph g;
    for (const char* v : {"jumps", "releases"})
        g.g.variables.push_back({v, PinType::Int, std::int32_t{0}, false});
    g.g.variables.push_back({"axis", PinType::Float, 0.0f, false});
    g.g.variables.push_back({"down", PinType::Bool, false, false});
    const auto increment = [&](std::uint32_t after, const char* afterPin, const char* var) {
        const std::uint32_t add = g.Node("Math.AddInt");
        g.Set(add, "B", std::int32_t{1});
        g.Link(g.Node("Variable.Get", var), "Value", add, "A");
        return SetVar(g, after, afterPin, var, add, "Result");
    };
    increment(g.Node("Event.InputActionPressed", "Jump"), "Out", "jumps");
    increment(g.Node("Event.InputActionReleased", "Jump"), "Out", "releases");
    const std::uint32_t tick = g.Node("Event.Tick");
    const std::uint32_t s1   = SetVar(g, tick, "Out", "axis", g.Node("Input.GetAxis", "MoveForward"), "Value");
    SetVar(g, s1, "Then", "down", g.Node("Input.IsActionDown", "Jump"), "Down");
    CHECK(g.Valid());

    EventBus     bus;
    Scene        scene;
    Input        input{bus};
    ScriptSystem scripts{bus, &input};
    scripts.SetInputMap(map);
    const Entity e = scene.CreateEntity("Player");
    scene.GetRegistry().Emplace<ScriptComponent>(e, ScriptComponent{"input.ugraph"});
    scripts.Provide("input.ugraph", g.g);
    scripts.Begin(scene);
    // Keys (codes >= 0) and the left mouse button (-1) this frame.
    const auto frame = [&](std::initializer_list<std::pair<int, InputAction>> keys) {
        input.NewFrame();
        for (const auto& [key, action] : keys)
            if (key < 0)
                bus.Publish(MouseButtonEvent{0, action, 0});
            else
                bus.Publish(KeyEvent{key, 0, action, 0});
        scripts.Update(scene, 0.016f);
    };
    const auto var = [&](const char* name) {
        const auto watch = scripts.Watch("input.ugraph", e); // keep the copy alive while iterating
        if (watch)
            for (const auto& [n, v] : watch->variables)
                if (n == name)
                    return v;
        return ScriptValue(false);
    };
    constexpr int kSpace = 32, kW = 87, kS = 83;
    frame({{kSpace, InputAction::Press}, {kW, InputAction::Press}});
    CHECK(IntOf(var("jumps")) == 1 && FloatOf(var("axis")) == 1.0f && std::get<bool>(var("down")));
    frame({{kS, InputAction::Press}});
    CHECK(FloatOf(var("axis")) == 0.0f && IntOf(var("jumps")) == 1);
    frame({{-1, InputAction::Press}}); // a second bound key: pressed again
    CHECK(IntOf(var("jumps")) == 2);
    frame({{kSpace, InputAction::Release}});
    CHECK(IntOf(var("releases")) == 0 && std::get<bool>(var("down"))); // the mouse button still holds it
    frame({{-1, InputAction::Release}, {kW, InputAction::Release}});
    CHECK(IntOf(var("releases")) == 1 && !std::get<bool>(var("down")) && FloatOf(var("axis")) == -1.0f);
    scripts.End(scene);

    // Save games: values + variables to a slot file, read by a new script system.
    const fs::path dir = TempDir("ungine_saves");
    Graph s;
    s.g.variables.push_back({"score", PinType::Int, std::int32_t{7}, false});
    s.g.variables.push_back({"name", PinType::String, std::string("Ada"), false});
    s.g.variables.push_back({"loaded", PinType::Int, std::int32_t{0}, false});
    s.g.variables.push_back({"found", PinType::Bool, false, false});
    s.g.variables.push_back({"exists", PinType::Bool, false, false});
    s.g.variables.push_back({"level", PinType::String, std::string(), false});
    s.g.events.push_back({"Load", {}});
    s.g.events.push_back({"Bad", {}});
    {
        const std::uint32_t begin = s.Node("Event.BeginPlay"), set = s.Node("SaveGame.SetValue", "int");
        s.Set(set, "Key", std::string("best"));
        s.Set(set, "Value", std::int32_t{42});
        s.Link(begin, "Out", set, "In");
        const std::uint32_t vars = s.Node("SaveGame.SaveVariables");
        s.Set(vars, "Prefix", std::string("v."));
        s.Link(set, "Then", vars, "In");
        const std::uint32_t lvl = SetVar(s, vars, "Then", "level", s.Node("Game.CurrentLevel"), "Scene");
        const std::uint32_t open = s.Node("Game.OpenLevel");
        s.Set(open, "Scene", std::string("Content/Next.uscene"));
        s.Link(lvl, "Then", open, "In");
        // Load: variables back, best -> loaded, exists.
        const std::uint32_t load = s.Node("Event.Custom", "Load"), lv = s.Node("SaveGame.LoadVariables");
        s.Set(lv, "Prefix", std::string("v."));
        s.Link(load, "Out", lv, "In");
        const std::uint32_t get = s.Node("SaveGame.GetValue", "int");
        s.Set(get, "Key", std::string("best"));
        const std::uint32_t a = SetVar(s, lv, "Then", "loaded", get, "Value");
        const std::uint32_t b = SetVar(s, a, "Then", "found", get, "Found");
        const std::uint32_t c = SetVar(s, b, "Then", "exists", s.Node("SaveGame.Exists"), "Exists");
        const std::uint32_t quit = s.Node("Game.Quit");
        s.Link(c, "Then", quit, "In");
        const std::uint32_t bad = s.Node("SaveGame.Save");
        s.Set(bad, "Slot", std::string("../escape"));
        s.Link(s.Node("Event.Custom", "Bad"), "Out", bad, "In");
    }
    CHECK(s.Valid());
    {
        Runner r;
        r.scripts.SetSaveDirectory(dir);
        r.scripts.SetCurrentLevel("Content/Main.uscene");
        r.Add("Saver", "save.ugraph", s.g);
        r.scripts.Begin(r.scene);
        const auto request = r.scripts.TakeLevelRequest();
        CHECK(request && !request->quit && request->scene == "Content/Next.uscene" && !r.scripts.TakeLevelRequest());
        CHECK(fs::exists(dir / "Save1.sav"));
        r.scripts.End(r.scene);
    }
    {
        Runner r;
        r.scripts.SetSaveDirectory(dir);
        Graph changed = s; // different start values: Load Variables restores the saved ones
        changed.g.variables[0].value = std::int32_t{0};
        changed.g.variables[1].value = std::string("Bob");
        changed.g.nodes.erase(std::ranges::find(changed.g.nodes, std::string("Event.BeginPlay"), &ScriptNode::type));
        std::erase_if(changed.g.links, [&](const ScriptLink& l) { return !changed.g.FindNode(l.fromNode); });
        CHECK(changed.Valid());
        const Entity loader = r.Add("Loader", "load.ugraph", changed.g);
        Graph trigger;
        const std::uint32_t b = trigger.Node("Event.BeginPlay"), call = trigger.Node("Script.CallEvent");
        trigger.Set(call, "Event", std::string("Load"));
        trigger.Link(b, "Out", call, "In");
        const std::uint32_t find = trigger.Node("Entity.FindByName");
        trigger.Set(find, "Name", std::string("Loader"));
        trigger.Link(find, "Entity", call, "Target");
        const std::uint32_t call2 = trigger.Node("Script.CallEvent");
        trigger.Set(call2, "Event", std::string("Bad"));
        trigger.Link(call, "Then", call2, "In");
        trigger.Link(find, "Entity", call2, "Target");
        r.Add("Trigger", "trigger.ugraph", trigger.g);
        r.scripts.Begin(r.scene);
        CHECK(IntOf(r.Var("load.ugraph", loader, "score")) == 7 &&
              std::get<std::string>(r.Var("load.ugraph", loader, "name")) == "Ada");
        CHECK(IntOf(r.Var("load.ugraph", loader, "loaded")) == 42 && std::get<bool>(r.Var("load.ugraph", loader, "found")) &&
              std::get<bool>(r.Var("load.ugraph", loader, "exists")));
        const auto request = r.scripts.TakeLevelRequest();
        CHECK(request && request->quit);
        CHECK(r.scripts.Stats().errors == 1 && !fs::exists(dir.parent_path() / "escape.sav")); // invalid slot name
        r.scripts.End(r.scene);
    }
    fs::remove_all(dir);

    // Project input settings round trip.
    const fs::path pdir = TempDir("ungine_input");
    {
        auto project = Project::Create(pdir, "InputGame", ProjectTemplates().front());
        CHECK(project.has_value());
        project->settings.input = map;
        CHECK(project->Save());
        const auto loaded = Project::Load(project->File());
        CHECK(loaded && loaded->settings.input == map);
    }
    fs::remove_all(pdir);
}

TEST_CASE(Blueprint2_ConditionsSteppingAndConstruction)
{
    ScriptRegistry::Clear();
    // Conditions.
    const ScriptValue health = 7.0f, name = std::string("Bob"), dead = false, count = std::int32_t{6};
    const auto vars = [&](std::string_view n) -> const ScriptValue* {
        return n == "health" ? &health : n == "name" ? &name : n == "dead" ? &dead : n == "count" ? &count : nullptr;
    };
    const auto eval = [&](const char* e) { return EvaluateScriptCondition(e, vars); };
    CHECK(eval("health < 10 && !dead") == true && eval("health >= 10 || dead") == false);
    CHECK(eval("name == \"Bob\"") == true && eval("name != \"Bob\"") == false && eval("count % 3 == 0") == true);
    CHECK(eval("(count + 1) * 2 == 14") == true && eval("-health < 0") == true && eval("count / 4 > 1.4") == true);
    std::string error;
    CHECK(!EvaluateScriptCondition("missing > 1", vars, &error) && error.find("missing") != std::string::npos);
    CHECK(CheckScriptCondition("a < (b") != "" && CheckScriptCondition("a <= b && \"x\" == c").empty() &&
          CheckScriptCondition("a b") != "");

    // Stepping: BeginPlay -> A -> Call F -> B; F: F1 -> F2 -> Return. Loop body with a conditional
    // breakpoint (counter == 3) and one with a hit count (2nd hit).
    Graph g;
    g.g.variables.push_back({"counter", PinType::Int, std::int32_t{0}, false});
    CHECK(g.g.AddFunction("F"));
    const std::uint32_t f1 = Print(g, "f1", "F"), f2 = Print(g, "f2", "F");
    g.Link(g.Find("Function.Entry", "F"), "Then", f1, "In");
    g.Link(f1, "Then", f2, "In");
    g.Link(f2, "Then", g.Find("Function.Return", "F"), "In");
    const std::uint32_t begin = g.Node("Event.BeginPlay"), a = Print(g, "a"), call = g.Node("Function.Call", "F"), b = Print(g, "b");
    g.Link(begin, "Out", a, "In");
    g.Link(a, "Then", call, "In");
    g.Link(call, "Then", b, "In");
    const std::uint32_t loop = g.Node("Flow.ForLoop");
    g.Set(loop, "First Index", std::int32_t{1});
    g.Set(loop, "Last Index", std::int32_t{5});
    g.Link(b, "Then", loop, "In");
    const std::uint32_t add = g.Node("Math.AddInt");
    g.Set(add, "B", std::int32_t{1});
    g.Link(g.Node("Variable.Get", "counter"), "Value", add, "A");
    const std::uint32_t set = SetVar(g, loop, "Loop Body", "counter", add, "Result");
    const std::uint32_t inLoop = Print(g, "loop");
    g.Link(set, "Then", inLoop, "In");
    const std::uint32_t hit = Print(g, "hit");
    g.Link(inLoop, "Then", hit, "In");
    g.g.SetBreakpoint(call, true);
    g.g.SetBreakpoint(inLoop, true);
    g.g.breakpointOptions[inLoop] = {"counter == 3", 0};
    g.g.SetBreakpoint(hit, true);
    g.g.breakpointOptions[hit] = {"", 2};
    CHECK(g.Valid());
    CHECK(ScriptGraphFromJson(ScriptGraphToJson(g.g)).breakpointOptions == g.g.breakpointOptions);

    const auto counter = [](Runner& r, Entity e) { return IntOf(r.Var("step.ugraph", e, "counter")); };
    {
        Runner       r;
        const Entity e = r.Add("S", "step.ugraph", g.g);
        r.scripts.Begin(r.scene);
        CHECK(r.scripts.DebugPaused() && r.scripts.PausedAt()->node == call);
        r.scripts.DebugStepOver(r.scene); // the function runs through
        CHECK(r.scripts.PausedAt() && r.scripts.PausedAt()->node == b && r.Printed("f2") && !r.Printed("b"));
        r.scripts.DebugContinue(r.scene); // hit count 2: the first loop round passes "hit"
        auto at = r.scripts.PausedAt();
        CHECK(at && at->node == hit && counter(r, e) == 2);
        r.scripts.DebugContinue(r.scene); // condition counter == 3
        at = r.scripts.PausedAt();
        CHECK(at && at->node == inLoop && counter(r, e) == 3);
        r.scripts.DebugContinue(r.scene); // "hit" (3rd hit) then nothing more
        CHECK(r.scripts.PausedAt() && r.scripts.PausedAt()->node == hit);
        r.scripts.SetBreakpoints("step.ugraph", {});
        r.scripts.DebugContinue(r.scene);
        CHECK(!r.scripts.DebugPaused() && counter(r, e) == 5);
        r.scripts.End(r.scene);
    }
    {
        Runner r;
        r.Add("S", "step.ugraph", g.g);
        r.scripts.Begin(r.scene);
        r.scripts.DebugStep(r.scene); // into F
        auto at = r.scripts.PausedAt();
        CHECK(at && at->node == f1 && at->function == "F" && at->callers.size() == 1 && at->callers[0].node == call);
        r.scripts.DebugStepOver(r.scene); // next node in F
        CHECK(r.scripts.PausedAt() && r.scripts.PausedAt()->node == f2);
        r.scripts.DebugStepOut(r.scene); // back in the event graph after the call
        at = r.scripts.PausedAt();
        CHECK(at && at->node == b && at->callers.empty());
        // A broken condition stops (and reports) instead of being ignored.
        r.scripts.SetBreakpointList("step.ugraph", {{inLoop, {"nope > 1", 0}}});
        r.scripts.DebugContinue(r.scene);
        CHECK(r.scripts.PausedAt() && r.scripts.PausedAt()->node == inLoop && r.scripts.Stats().errors == 1);
        r.scripts.SetBreakpoints("step.ugraph", {});
        r.scripts.DebugContinue(r.scene);
        r.scripts.End(r.scene);
    }

    // Construction script: Count posts (exposed) as children, rebuilt on every run, not saved.
    Graph c;
    c.g.variables.push_back({"Count", PinType::Int, std::int32_t{3}, true});
    const std::uint32_t cons = c.Node("Event.Construction"), floop = c.Node("Flow.ForLoop");
    c.Link(cons, "Out", floop, "In");
    const std::uint32_t last = c.Node("Math.SubtractInt");
    c.Set(last, "B", std::int32_t{1});
    c.Link(c.Node("Variable.Get", "Count"), "Value", last, "A");
    c.Link(last, "Result", floop, "Last Index");
    const std::uint32_t spawn = c.Node("Entity.SpawnEmpty"), toFloat = c.Node("Convert.IntToFloat"), vec = c.Node("Vector.Make");
    c.Set(spawn, "Name", std::string("Post"));
    c.Link(floop, "Loop Body", spawn, "In");
    c.Link(floop, "Index", toFloat, "Value");
    c.Link(toFloat, "Result", vec, "X");
    c.Link(vec, "Vector", spawn, "Location");
    c.Link(c.Node("Entity.Self"), "Self", spawn, "Parent");
    const std::uint32_t delay = c.Node("Flow.Delay"); // reported, does not run
    c.Link(floop, "Completed", delay, "In");
    CHECK(c.Valid());
    {
        Runner           r;
        const Entity     fence = r.Add("Fence", "fence.ugraph", c.g);
        const std::uint64_t fenceUuid = r.scene.GetRegistry().Get<Uuid>(fence).value;
        Registry&        reg   = r.scene.GetRegistry();
        const auto posts = [&] {
            std::size_t n = 0;
            reg.ViewOf<ConstructionOwned>().Each([&](Entity, ConstructionOwned& o) { n += o.owner == fenceUuid ? 1 : 0; });
            return n;
        };
        CHECK(r.scripts.RunConstruction(r.scene, fence) && posts() == 3);
        CHECK(reg.Get<Hierarchy>(fence).children.size() == 3 && r.scripts.Stats().errors == 1); // the Delay
        r.scene.GetRegistry().Get<ScriptComponent>(fence).variables["Count"] = {std::int32_t{5}, 0};
        CHECK(r.scripts.RunAllConstruction(r.scene) == 1 && posts() == 5);
        // Saved without the posts; a snapshot (undo / play) keeps them marked.
        const fs::path dir = TempDir("ungine_construct");
        SaveSceneFile(dir / "s.uscene", r.scene, nullptr);
        Scene loaded;
        (void)LoadSceneFile(dir / "s.uscene", loaded, nullptr);
        std::size_t entities = 0;
        loaded.GetRegistry().ViewOf<Uuid>().Each([&](Entity, Uuid&) { ++entities; });
        CHECK(entities == 1);
        const std::vector<Entity> roots{fence};
        const std::string         snapshot = SnapshotEntities(r.scene, roots);
        r.scene.Clear();
        (void)RestoreEntities(r.scene, snapshot, RestoreMode::Original);
        CHECK(posts() == 5);
        // Play: rebuilt before BeginPlay (no duplicates).
        r.scripts.Begin(r.scene);
        CHECK(posts() == 5);
        r.scripts.End(r.scene);
        // The owner gone: its posts go with the next run.
        const Entity owner = r.scene.FindByUuid(fenceUuid);
        r.scene.GetRegistry().Remove<ScriptComponent>(owner);
        CHECK(r.scripts.RunAllConstruction(r.scene) == 0);
        std::size_t left = 0;
        reg.ViewOf<ConstructionOwned>().Each([&](Entity, ConstructionOwned&) { ++left; });
        CHECK(left == 0);
        fs::remove_all(dir);
    }
}

TEST_CASE(Blueprint2_CollapseToFunctionAndMacro)
{
    ScriptRegistry::Clear();
    // BeginPlay -> Set x = y * 2 -> Print x -> Delay 0.2 -> Print "late".
    Graph g;
    g.g.variables.push_back({"x", PinType::Float, 0.0f, false});
    g.g.variables.push_back({"y", PinType::Int, std::int32_t{21}, false});
    const std::uint32_t begin = g.Node("Event.BeginPlay"), mul = g.Node("Math.MultiplyFloat"), gety = g.Node("Variable.Get", "y");
    g.Set(mul, "B", 2.0f);
    g.Link(gety, "Value", mul, "A");
    const std::uint32_t setx = SetVar(g, begin, "Out", "x", mul, "Result");
    const std::uint32_t print = Print(g);
    g.Link(setx, "Then", print, "In");
    g.Link(setx, "Value", print, "Text");
    const std::uint32_t delay = g.Node("Flow.Delay"), late = Print(g, "late");
    g.Set(delay, "Duration", 0.2f);
    g.Link(print, "Then", delay, "In");
    g.Link(delay, "Completed", late, "In");
    CHECK(g.Valid());

    Graph f = g;
    CHECK(!f.g.Collapse({begin}, "Bad", false, {}).empty());            // events stay
    CHECK(!f.g.Collapse({delay}, "Bad", false, {}).empty());            // latent: macro only
    CHECK(!f.g.Collapse({mul}, "bad.name", false, {}).empty());
    CHECK(f.g.Collapse({mul, setx}, "Double", false, {100.0f, 0.0f}).empty());
    const ScriptFunction* fn = f.g.FindFunction("Double");
    CHECK(fn && !fn->pure && fn->inputs.size() == 1 && fn->inputs[0].type == PinType::Int && fn->outputs.size() == 1 &&
          fn->outputs[0].type == PinType::Float && f.g.FindNode(mul)->function == "Double");
    CHECK(f.g.Collapse({print}, "Show", false, {}).empty()); // an impure node with exec in / out
    CHECK(f.g.Collapse({delay, late}, "Wait", true, {}).empty());
    CHECK(f.g.FindMacro("Wait") && f.g.FindMacro("Wait")->inputs.size() == 1 && f.g.FindMacro("Wait")->outputs.empty());
    CHECK(f.Valid());
    {
        Graph p = g; // pure: just the multiply
        CHECK(p.g.Collapse({mul}, "Twice", false, {}).empty() && p.g.FindFunction("Twice")->pure);
        CHECK(p.Valid());
        Runner r;
        const Entity e = r.Add("P", "pure.ugraph", p.g);
        r.scripts.Begin(r.scene);
        CHECK(FloatOf(r.Var("pure.ugraph", e, "x")) == 42.0f);
        r.scripts.End(r.scene);
    }
    Runner       r;
    const Entity e = r.Add("C", "collapsed.ugraph", f.g);
    r.scripts.Begin(r.scene);
    CHECK(FloatOf(r.Var("collapsed.ugraph", e, "x")) == 42.0f && r.Printed("42") && !r.Printed("late"));
    r.Run(0.3f);
    CHECK(r.Printed("late") && r.scripts.Stats().errors == 0);
    r.scripts.End(r.scene);
}
