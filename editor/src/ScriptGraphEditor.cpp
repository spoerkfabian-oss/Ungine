#include "Editor/ScriptGraphEditor.h"
#include "FileDialog.h"

#include "Engine/Core/Log.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Script/ScriptCondition.h"
#include "Engine/Script/ScriptNodes.h"
#include "Engine/Script/ScriptRegistry.h"
#include "Engine/Script/ScriptSystem.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <deque>
#include <map>
#include <unordered_set>
#include <set>

namespace Engine {

namespace {

constexpr float kHeader    = 24.0f; // canvas units
constexpr float kRow       = 22.0f;
constexpr float kPad       = 8.0f;
constexpr float kPinRadius = 5.0f;
constexpr float kPinHit    = 9.0f;
constexpr float kMinZoom   = 0.2f;
constexpr float kMaxZoom   = 2.0f;
constexpr std::size_t kMaxUndo = 200;

ImU32 PinColor(PinType type, float alpha = 1.0f)
{
    const auto c = [&](int r, int g, int b) { return IM_COL32(r, g, b, static_cast<int>(alpha * 255.0f)); };
    switch (type.kind) { // containers: the element's color (drawn as a square / diamond pin)
    case PinKind::Exec: return c(235, 235, 235);
    case PinKind::Bool: return c(210, 50, 50);
    case PinKind::Int: return c(40, 205, 160);
    case PinKind::Float: return c(150, 225, 70);
    case PinKind::Vec3: return c(245, 195, 45);
    case PinKind::String: return c(235, 90, 210);
    case PinKind::Entity: return c(70, 150, 255);
    case PinKind::Enum: return c(20, 120, 90);
    case PinKind::Struct: return c(40, 80, 200);
    }
    return c(255, 255, 255);
}

bool IsReroute(const ScriptNode& node) { return node.type == "Utility.Reroute" || node.type == "Utility.RerouteExec"; }
bool IsFunctionFrame(const ScriptNode& node)
{
    return node.type == "Function.Entry" || node.type == "Function.Return" || node.type == "Macro.Inputs" || node.type == "Macro.Outputs";
}

// Every value base type: built-ins, then the registered enums and structs.
std::vector<PinType> BaseTypes(bool keysOnly)
{
    std::vector<PinType> types;
    for (PinType t : {PinType::Bool, PinType::Int, PinType::Float, PinType::Vec3, PinType::String, PinType::Entity})
        if (!keysOnly || IsKeyType(t))
            types.push_back(t);
    for (const std::string& e : ScriptRegistry::EnumNames())
        types.push_back(PinType::Enum(e));
    if (!keysOnly)
        for (const std::string& st : ScriptRegistry::StructNames())
            types.push_back(PinType::Struct(st));
    return types;
}

// Type picker: container (single / array / map), value type, map key type. True when changed.
bool TypeCombo(const char* id, PinType& type, float width)
{
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(PinColor(type)));
    const bool open = ImGui::BeginCombo(id, DisplayName(type).c_str(), ImGuiComboFlags_HeightLarge);
    ImGui::PopStyleColor();
    if (!open)
        return false;
    int container = static_cast<int>(type.container);
    bool pickContainer = ImGui::RadioButton("Single", &container, 0); // all three drawn (no short circuit)
    ImGui::SameLine();
    pickContainer |= ImGui::RadioButton("Array", &container, 1);
    ImGui::SameLine();
    pickContainer |= ImGui::RadioButton("Map", &container, 2);
    if (pickContainer) {
        const PinType value = ElementType(type);
        type    = container == 1 ? ArrayOf(value) : container == 2 ? PinType::Map(PinType::String, value) : value;
        changed = true;
    }
    if (IsMap(type)) {
        ImGui::SeparatorText("Key");
        for (PinType key : BaseTypes(true)) {
            ImGui::PushID(ToString(key).c_str());
            if (ImGui::Selectable(DisplayName(key).c_str(), KeyType(type) == key, ImGuiSelectableFlags_NoAutoClosePopups)) {
                type    = PinType::Map(key, ElementType(type));
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::SeparatorText("Value");
    }
    for (PinType base : BaseTypes(false)) {
        ImGui::PushID(ToString(base).c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(PinColor(base)));
        if (ImGui::Selectable(DisplayName(base).c_str(), ElementType(type) == base)) {
            type    = IsArray(type) ? ArrayOf(base) : IsMap(type) ? PinType::Map(KeyType(type), base) : base;
            changed = true;
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndCombo();
    return changed;
}

ImU32 HeaderColor(const NodeDesc* desc, const ScriptGraph& graph, const ScriptNode& node)
{
    if (!desc)
        return IM_COL32(120, 30, 30, 255);
    if (desc->kind == NodeKind::Event)
        return IM_COL32(140, 30, 30, 255);
    if (desc->category == "Flow")
        return IM_COL32(75, 75, 80, 255);
    if (desc->category == "Functions")
        return desc->kind == NodeKind::Pure ? IM_COL32(60, 95, 60, 255) : IM_COL32(95, 55, 140, 255);
    if (desc->category == "Variables") {
        const ScriptVariable* v = graph.FindVariableInScope(node.function, node.param);
        const ImVec4          c = ImGui::ColorConvertU32ToFloat4(PinColor(v ? v->type : PinType::Float));
        return ImGui::GetColorU32(ImVec4(c.x * 0.45f, c.y * 0.45f, c.z * 0.45f, 1.0f));
    }
    if (desc->kind == NodeKind::Pure)
        return IM_COL32(40, 95, 55, 255);
    return IM_COL32(35, 75, 130, 255);
}

std::string Lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string NodeTitle(const NodeDesc* desc, const ScriptNode& node)
{
    if (!desc)
        return "Unknown: " + node.type;
    switch (desc->paramKind) {
    case ParamKind::Variable: return desc->title + " " + node.param;
    case ParamKind::Key:
    case ParamKind::Choice:
    case ParamKind::ElementType:
    case ParamKind::TypeAndCount:
    case ParamKind::PinType: return desc->title + " (" + node.param + ")";
    case ParamKind::Text: return desc->title + ": " + node.param;
    case ParamKind::Function:
    case ParamKind::StructField: return desc->title + " " + node.param;
    case ParamKind::StructType:
    case ParamKind::EnumType: return desc->title + " (" + node.param + ")";
    case ParamKind::EnumValue: return node.param.empty() ? desc->title : node.param;
    case ParamKind::Macro:
        return node.type == "Macro.Use" ? (node.param.empty() ? desc->title : node.param) : desc->title;
    case ParamKind::LibraryFunction: return node.param.empty() ? desc->title : node.param;
    case ParamKind::InterfaceFunction: return node.param.empty() ? desc->title : node.param + " (Message)";
    case ParamKind::Interface:
    case ParamKind::Dispatcher:
    case ParamKind::Timeline:
    case ParamKind::InputAction:
    case ParamKind::InputAxis: return desc->title + " " + node.param;
    default: return desc->title;
    }
}

float WidgetWidth(PinType type)
{
    if (IsContainer(type) || type.kind == PinKind::Struct)
        return 44.0f; // a button with a popup editor
    switch (type.kind) {
    case PinKind::Bool: return 20.0f;
    case PinKind::Int: return 50.0f;
    case PinKind::Float: return 56.0f;
    case PinKind::Vec3: return 156.0f;
    case PinKind::String: return 96.0f;
    case PinKind::Entity: return 30.0f;
    case PinKind::Enum: return 90.0f;
    default: return 0.0f;
    }
}

struct NodeLayout {
    const ScriptNode*      node = nullptr;
    const NodeDesc*        desc = nullptr;
    std::string            title;
    glm::vec2              pos{0.0f}, size{0.0f};
    std::vector<PinInfo>   pins;
    std::vector<glm::vec2> pinPos;    // canvas
    std::vector<bool>      connected;
    std::vector<float>     labelWidth;

    [[nodiscard]] bool Contains(glm::vec2 p) const
    {
        return p.x >= pos.x && p.y >= pos.y && p.x <= pos.x + size.x && p.y <= pos.y + size.y;
    }
    [[nodiscard]] int PinIndex(const std::string& name, bool output) const
    {
        for (std::size_t i = 0; i < pins.size(); ++i)
            if (pins[i].output == output && pins[i].name == name)
                return static_cast<int>(i);
        return -1;
    }
};

// Text widths are measured at the window's font size (= canvas units at zoom 1).
NodeLayout Layout(const ScriptGraph& graph, const ScriptNode& node)
{
    NodeLayout l;
    l.node  = &node;
    l.desc  = FindScriptNodeType(node.type);
    l.title = NodeTitle(l.desc, node);
    l.pos   = node.position;
    l.pins  = NodePins(graph, node);
    if (IsReroute(node)) { // a knot: pins on both sides of a small dot, no header
        l.size = {32.0f, 20.0f};
        l.pinPos.resize(l.pins.size());
        l.connected.resize(l.pins.size(), false);
        l.labelWidth.resize(l.pins.size(), 0.0f);
        for (std::size_t i = 0; i < l.pins.size(); ++i)
            l.pinPos[i] = {l.pins[i].output ? l.pos.x + l.size.x : l.pos.x, l.pos.y + l.size.y * 0.5f};
        for (const ScriptLink& link : graph.links)
            for (std::size_t i = 0; i < l.pins.size(); ++i)
                if ((l.pins[i].output && link.fromNode == node.id) || (!l.pins[i].output && link.toNode == node.id))
                    l.connected[i] = true;
        return l;
    }
    l.pinPos.resize(l.pins.size());
    l.connected.resize(l.pins.size(), false);
    l.labelWidth.resize(l.pins.size(), 0.0f);
    for (const ScriptLink& link : graph.links) {
        if (link.fromNode == node.id)
            if (const int i = l.PinIndex(link.fromPin, true); i >= 0)
                l.connected[static_cast<std::size_t>(i)] = true;
        if (link.toNode == node.id)
            if (const int i = l.PinIndex(link.toPin, false); i >= 0)
                l.connected[static_cast<std::size_t>(i)] = true;
    }
    float inWidth = 0.0f, outWidth = 0.0f;
    int   inRows = 0, outRows = 0;
    for (std::size_t i = 0; i < l.pins.size(); ++i) {
        const PinInfo& p     = l.pins[i];
        const bool     named = !(p.type == PinType::Exec && (p.name == "In" || p.name == "Then" || p.name == "Out"));
        l.labelWidth[i]      = named ? ImGui::CalcTextSize(p.name.c_str()).x : 0.0f;
        float width          = l.labelWidth[i];
        if (!p.output && p.type != PinType::Exec && !l.connected[i])
            width += WidgetWidth(p.type) + 4.0f;
        if (p.output) {
            outWidth = std::max(outWidth, width);
            ++outRows;
        } else {
            inWidth = std::max(inWidth, width);
            ++inRows;
        }
    }
    const float titleWidth = ImGui::CalcTextSize(l.title.c_str()).x + 2.0f * kPad;
    l.size.x = std::max({titleWidth, inWidth + outWidth + 4.0f * kPad + 16.0f, 110.0f});
    l.size.y = kHeader + static_cast<float>(std::max({inRows, outRows, 1})) * kRow + kPad * 0.5f;
    int in = 0, out = 0;
    for (std::size_t i = 0; i < l.pins.size(); ++i) {
        const int row = l.pins[i].output ? out++ : in++;
        l.pinPos[i]   = {l.pins[i].output ? l.pos.x + l.size.x : l.pos.x, l.pos.y + kHeader + (static_cast<float>(row) + 0.5f) * kRow};
    }
    return l;
}

// A name not used by graph variables or the scope's locals (or functions when `functions`).
std::string UniqueName(const ScriptGraph& graph, const std::string& scope, const std::string& base, bool functions = false)
{
    std::string name = base;
    for (int i = 1; functions ? graph.HasScope(name) : graph.FindVariableInScope(scope, name) != nullptr; ++i)
        name = base + std::to_string(i);
    return name;
}

bool ValueWidget(const char* id, ScriptValue& value, PinType type, float width);

// Arrays: a "[n]" button opening a list editor.
bool ArrayWidget(const char* id, ScriptValue& value, PinType type, float width)
{
    bool              changed = false;
    const std::size_t count   = ArrayItems(value).items.size();
    ImGui::PushID(id);
    if (ImGui::Button(std::format("[{}]", count).c_str(), ImVec2(width, 0.0f)))
        ImGui::OpenPopup("items");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", ToDisplayString(value).c_str());
    if (ImGui::BeginPopup("items")) {
        const PinType element = ElementType(type);
        ImGui::TextDisabled("%s, %zu item(s)", DisplayName(type).c_str(), count);
        std::optional<std::size_t> remove;
        for (std::size_t i = 0; i < ArrayItems(value).items.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%zu", i);
            ImGui::SameLine(36.0f);
            ScriptValue item = ArrayItems(value).items[i];
            if (ValueWidget("##item", item, element, 180.0f)) {
                MutableArray(value, element).items[i] = std::move(item);
                changed                               = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                remove = i;
            ImGui::PopID();
        }
        if (remove) {
            auto& items = MutableArray(value, element).items;
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(*remove));
            changed = true;
        }
        if (ImGui::SmallButton("+ Item")) {
            MutableArray(value, element).items.push_back(DefaultValue(element));
            changed = true;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// Structs: a "{..}" button opening the field editors (types from the definition).
bool StructWidget(const char* id, ScriptValue& value, PinType type, float width)
{
    bool               changed = false;
    const std::string& name    = UserTypeName(type);
    ImGui::PushID(id);
    if (ImGui::Button("{..}", ImVec2(width, 0.0f)))
        ImGui::OpenPopup("fields");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s %s", name.c_str(), ToDisplayString(value).c_str());
    if (ImGui::BeginPopup("fields")) {
        const ScriptStructDef* def = ScriptRegistry::FindStruct(name);
        ImGui::TextDisabled("%s", name.c_str());
        if (!def)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Unknown struct");
        else
            for (const ScriptStructField& f : def->fields) {
                ImGui::PushID(f.name.c_str());
                ImGui::TextUnformatted(f.name.c_str());
                ImGui::SameLine(110.0f);
                const ScriptValue* current = StructField(value, f.name);
                ScriptValue        field   = current ? *current : f.value;
                if (ValueWidget("##field", field, f.type, 180.0f)) {
                    SetStructField(value, name, f.name, std::move(field));
                    changed = true;
                }
                ImGui::PopID();
            }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// Maps: a "{n}" button opening the entry list (key + value, add / remove).
bool MapWidget(const char* id, ScriptValue& value, PinType type, float width)
{
    bool              changed = false;
    const PinType     key     = KeyType(type), element = ElementType(type);
    const std::size_t count   = MapOf(value).items.size();
    ImGui::PushID(id);
    if (ImGui::Button(std::format("{{{}}}", count).c_str(), ImVec2(width, 0.0f)))
        ImGui::OpenPopup("entries");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", ToDisplayString(value).c_str());
    if (ImGui::BeginPopup("entries")) {
        ImGui::TextDisabled("%s, %zu entr%s", DisplayName(type).c_str(), count, count == 1 ? "y" : "ies");
        std::optional<ScriptValue>                      remove;
        std::optional<std::pair<ScriptValue, ScriptValue>> rekey;
        int                                             i = 0;
        for (const auto& [k, v] : MapOf(value).items) {
            ImGui::PushID(i++);
            ScriptValue newKey = k;
            if (ValueWidget("##key", newKey, key, 120.0f) && !ValuesEqual(newKey, k))
                rekey = std::pair{k, newKey};
            ImGui::SameLine();
            ScriptValue item = v;
            if (ValueWidget("##value", item, element, 160.0f)) {
                MutableMap(value, key, element).items[k] = std::move(item);
                changed                                  = true;
                ImGui::PopID();
                break; // iterators changed
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                remove = k;
            ImGui::PopID();
        }
        if (remove) {
            MutableMap(value, key, element).items.erase(*remove);
            changed = true;
        } else if (rekey && !MapOf(value).items.contains(rekey->second)) {
            auto& items           = MutableMap(value, key, element).items;
            ScriptValue moved     = items[rekey->first];
            items.erase(rekey->first);
            items[rekey->second] = std::move(moved);
            changed              = true;
        }
        if (ImGui::SmallButton("+ Entry")) { // a key not used yet
            auto&       items = MutableMap(value, key, element).items;
            ScriptValue k     = DefaultValue(key);
            for (std::int32_t n = 1; items.contains(k) && n < 10000; ++n)
                k = key.kind == PinKind::String ? ScriptValue(std::string("key") + std::to_string(n))
                    : key.kind == PinKind::Bool ? ScriptValue(true)
                                                : ScriptValue(n);
            if (!items.contains(k)) {
                items[k] = DefaultValue(element);
                changed  = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// Value editor shared by inline pins, variables, details and the inspector. Returns true when changed.
bool ValueWidget(const char* id, ScriptValue& value, PinType type, float width)
{
    if (!ValueFits(value, type))
        value = Convert(value, type);
    if (IsArray(type))
        return ArrayWidget(id, value, type, width);
    if (IsMap(type))
        return MapWidget(id, value, type, width);
    ImGui::SetNextItemWidth(width);
    switch (type.kind) {
    case PinKind::Bool: return ImGui::Checkbox(id, &std::get<bool>(value));
    case PinKind::Int: return ImGui::DragInt(id, &std::get<std::int32_t>(value), 0.2f);
    case PinKind::Float: return ImGui::DragFloat(id, &std::get<float>(value), 0.01f, 0.0f, 0.0f, "%.3g");
    case PinKind::Vec3: return ImGui::DragFloat3(id, &std::get<glm::vec3>(value).x, 0.01f, 0.0f, 0.0f, "%.3g");
    case PinKind::String: return ImGui::InputText(id, &std::get<std::string>(value));
    case PinKind::Entity: ImGui::TextDisabled("self"); return false;
    case PinKind::Enum: {
        const ScriptEnum* e       = ScriptRegistry::FindEnum(UserTypeName(type));
        std::int32_t&     current = std::get<std::int32_t>(value);
        const std::string preview = ScriptRegistry::EnumValueName(UserTypeName(type), current);
        bool              changed = false;
        if (ImGui::BeginCombo(id, preview.empty() ? std::to_string(current).c_str() : preview.c_str())) {
            for (std::size_t i = 0; e && i < e->values.size(); ++i)
                if (ImGui::Selectable(e->values[i].c_str(), static_cast<std::size_t>(current) == i)) {
                    current = static_cast<std::int32_t>(i);
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    }
    case PinKind::Struct: return StructWidget(id, value, type, width);
    case PinKind::Exec: break;
    }
    return false;
}

// Distance from p to a link's bezier (sampled), for hit tests.
float BezierDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b)
{
    const float dx   = std::max(std::abs(b.x - a.x) * 0.5f, 40.0f);
    const glm::vec2 c1 = a + glm::vec2(dx, 0.0f), c2 = b - glm::vec2(dx, 0.0f);
    float           best = std::numeric_limits<float>::max();
    glm::vec2       prev = a;
    for (int i = 1; i <= 24; ++i) {
        const float     t  = static_cast<float>(i) / 24.0f, u = 1.0f - t;
        const glm::vec2 q  = u * u * u * a + 3.0f * u * u * t * c1 + 3.0f * u * t * t * c2 + t * t * t * b;
        const glm::vec2 ab = q - prev;
        const float     h  = std::clamp(glm::dot(p - prev, ab) / std::max(glm::dot(ab, ab), 1e-6f), 0.0f, 1.0f);
        best               = std::min(best, glm::distance(p, prev + ab * h));
        prev               = q;
    }
    return best;
}

} // namespace

// Parameter list editor (macros, events, dispatchers): the edited list, and the old -> new name
// of a renamed entry (links follow it).
struct ParamEdit {
    std::vector<ScriptParam> params;
    std::string              from, to;
};
std::optional<ParamEdit> ParamListWidget(const std::vector<ScriptParam>& params, bool allowExec, const char* addLabel, int idBase)
{
    std::optional<ParamEdit> result;
    for (std::size_t i = 0; i < params.size() && !result; ++i) {
        ImGui::PushID(idBase + static_cast<int>(i));
        std::string name = params[i].name;
        ImGui::SetNextItemWidth(90.0f);
        ImGui::InputText("##pname", &name);
        if (ImGui::IsItemDeactivatedAfterEdit() && name != params[i].name && IsValidScriptName(name) &&
            std::ranges::none_of(params, [&](const ScriptParam& p) { return p.name == name; })) {
            result               = ParamEdit{params, params[i].name, name};
            result->params[i].name = name;
        }
        ImGui::SameLine();
        PinType type = params[i].type;
        bool    exec = type.kind == PinKind::Exec;
        if (allowExec && ImGui::Checkbox("##exec", &exec)) {
            result                 = ParamEdit{params, {}, {}};
            result->params[i].type = exec ? PinType::Exec : PinType::Float;
        }
        if (allowExec && ImGui::IsItemHovered())
            ImGui::SetTooltip("Exec pin");
        if (allowExec)
            ImGui::SameLine();
        if (exec) {
            ImGui::TextDisabled("exec");
        } else if (TypeCombo("##ptype", type, 90.0f)) {
            result                 = ParamEdit{params, {}, {}};
            result->params[i].type = type;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            result = ParamEdit{params, {}, {}};
            result->params.erase(result->params.begin() + static_cast<std::ptrdiff_t>(i));
        }
        ImGui::PopID();
    }
    ImGui::PushID(idBase + 999);
    if (!result && ImGui::SmallButton(addLabel)) {
        std::string name = "Value";
        for (int n = 1; std::ranges::any_of(params, [&](const ScriptParam& p) { return p.name == name; }); ++n)
            name = "Value" + std::to_string(n);
        result = ParamEdit{params, {}, {}};
        result->params.push_back({name, PinType::Float});
    }
    ImGui::PopID();
    return result;
}

// Renames parameter pins: side(node) = 1 if the parameters are outputs of the node, 2 if inputs.
void RenamePins(ScriptGraph& g, const std::function<int(const ScriptNode&)>& side, const std::string& from, const std::string& to)
{
    for (ScriptLink& l : g.links) {
        const ScriptNode* a = g.FindNode(l.fromNode);
        const ScriptNode* b = g.FindNode(l.toNode);
        if (a && side(*a) == 1 && l.fromPin == from)
            l.fromPin = to;
        if (b && side(*b) == 2 && l.toPin == from)
            l.toPin = to;
    }
    for (ScriptNode& n : g.nodes)
        if (side(n) == 2)
            if (const auto it = n.defaults.find(from); it != n.defaults.end()) {
                ScriptValue v = it->second;
                n.defaults.erase(it);
                n.defaults[to] = std::move(v);
            }
}

struct ScriptGraphEditor::Document {
    std::filesystem::path path;
    ScriptGraph           graph;
    std::vector<std::pair<std::string, std::uint64_t>> undo, redo; // graph JSON + revision
    std::uint64_t revision = 0, savedRevision = 0;
    glm::vec2     scroll{0.0f};
    float         zoom   = 1.0f;
    bool          framed = false; // view fitted once
    std::vector<std::uint32_t> selected;
    std::uint32_t              selectedComment = 0;
    std::vector<ScriptDiagnostic> diagnostics;
    std::uint64_t                 diagnosed = ~std::uint64_t{0};
    bool                          selectTab = false;
    glm::vec2                     viewSize{800.0f, 500.0f}; // canvas size last frame (framing)
    std::string                   scope;  // the function shown on the canvas; empty: the event graph
    std::vector<std::uint32_t>    syncedBreakpoints;        // last sent to the script system
    std::map<std::uint32_t, ScriptBreakpointOptions> syncedOptions;
    Entity                        watchEntity = NullEntity; // instance whose values the debugger shows
    bool                          showMinimap = true;
};

struct ScriptGraphEditor::DragState {
    enum class Kind { None, Pan, Nodes, Comment, Resize, Link, Box } kind = Kind::None;
    int           button = 0;
    glm::vec2     start{0.0f}, last{0.0f}; // canvas
    std::string   before;
    std::uint64_t beforeRevision = 0;
    bool          moved          = false;
    // Link from a pin.
    std::uint32_t node   = 0;
    std::string   pin;
    bool          output = false;
    PinType       type   = PinType::Exec;
    // Comment drag: the nodes inside it move along.
    std::uint32_t              comment = 0;
    std::vector<std::uint32_t> carried;
    // Create menu.
    struct Pending {
        std::uint32_t node;
        std::string   pin;
        bool          output;
        PinType       type;
    };
    std::optional<Pending> pending;
    glm::vec2              menuPos{0.0f};
    std::string            search;
    bool                   openCreate = false, openNode = false, focusSearch = false;
    std::uint32_t          menuNode    = 0;
    std::optional<std::size_t> confirmClose;
    // Debugger: the pause last brought into view (node id + entity).
    std::uint32_t pausedNode   = 0;
    Entity        pausedEntity = NullEntity;
    std::uint32_t minimapDrag  = 0; // 1 while dragging the minimap view
};

ScriptGraphEditor::ScriptGraphEditor() : m_Drag(std::make_unique<DragState>()), m_Dialog(std::make_unique<FileDialog>()) {}
ScriptGraphEditor::~ScriptGraphEditor() = default;

// --- Documents ----------------------------------------------------------------------------------

ScriptGraphEditor::Document* ScriptGraphEditor::ActiveDoc()
{
    return m_Active < m_Docs.size() ? m_Docs[m_Active].get() : nullptr;
}
const ScriptGraphEditor::Document* ScriptGraphEditor::ActiveDoc() const
{
    return m_Active < m_Docs.size() ? m_Docs[m_Active].get() : nullptr;
}

bool ScriptGraphEditor::Open(const std::filesystem::path& file)
{
    const std::string key = ScriptSystem::Key(file);
    for (std::size_t i = 0; i < m_Docs.size(); ++i)
        if (ScriptSystem::Key(m_Docs[i]->path) == key) {
            Activate(i);
            return true;
        }
    auto doc = std::make_unique<Document>();
    try {
        doc->graph = LoadScriptGraph(file);
    } catch (const std::exception& e) {
        m_LastError = e.what();
        ENGINE_ERROR("Blueprint: {}", m_LastError);
        return false;
    }
    std::error_code ec;
    doc->path     = std::filesystem::absolute(file, ec).lexically_normal();
    doc->revision = doc->savedRevision = ++m_RevisionCounter;
    m_Docs.push_back(std::move(doc));
    Activate(m_Docs.size() - 1);
    return true;
}

bool ScriptGraphEditor::New(const std::filesystem::path& file)
{
    ScriptGraph graph;
    const std::uint32_t begin = graph.AddNode("Event.BeginPlay", {0.0f, 0.0f});
    const std::uint32_t tick  = graph.AddNode("Event.Tick", {0.0f, 140.0f});
    const std::uint32_t print = graph.AddNode("Debug.Print", {260.0f, 0.0f});
    graph.FindNode(print)->defaults["Text"] = std::string("Hello from ") + file.stem().string();
    graph.Connect(begin, "Out", print, "In");
    (void)tick;
    try {
        SaveScriptGraph(file, graph);
    } catch (const std::exception& e) {
        m_LastError = e.what();
        ENGINE_ERROR("Blueprint: {}", m_LastError);
        return false;
    }
    return Open(file);
}

bool ScriptGraphEditor::Save()
{
    Document* doc = ActiveDoc();
    if (!doc)
        return false;
    try {
        SaveScriptGraph(doc->path, doc->graph);
    } catch (const std::exception& e) {
        m_LastError = e.what();
        ENGINE_ERROR("Blueprint: {}", m_LastError);
        return false;
    }
    doc->savedRevision = doc->revision;
    return true;
}

bool ScriptGraphEditor::SaveAs(const std::filesystem::path& file)
{
    Document* doc = ActiveDoc();
    if (!doc)
        return false;
    std::error_code ec;
    doc->path = std::filesystem::absolute(file, ec).lexically_normal();
    return Save();
}

bool ScriptGraphEditor::SaveAll()
{
    bool              ok     = true;
    const std::size_t active = m_Active;
    for (std::size_t i = 0; i < m_Docs.size(); ++i)
        if (m_Docs[i]->revision != m_Docs[i]->savedRevision) {
            m_Active = i;
            ok       = Save() && ok;
        }
    m_Active = active;
    return ok;
}

void ScriptGraphEditor::Close(std::size_t index)
{
    if (index >= m_Docs.size())
        return;
    m_Docs.erase(m_Docs.begin() + static_cast<std::ptrdiff_t>(index));
    if (m_Active >= m_Docs.size())
        m_Active = m_Docs.empty() ? 0 : m_Docs.size() - 1;
    m_WidgetSession.reset();
    *m_Drag = DragState{};
}

std::size_t                  ScriptGraphEditor::Count() const { return m_Docs.size(); }
std::optional<std::size_t>   ScriptGraphEditor::Active() const { return m_Docs.empty() ? std::nullopt : std::optional(m_Active); }
ScriptGraph*                 ScriptGraphEditor::Graph() { return ActiveDoc() ? &ActiveDoc()->graph : nullptr; }
const ScriptGraph* ScriptGraphEditor::Find(const std::filesystem::path& file) const
{
    const std::string key = ScriptSystem::Key(file);
    for (const auto& doc : m_Docs)
        if (ScriptSystem::Key(doc->path) == key)
            return &doc->graph;
    return nullptr;
}
const std::filesystem::path* ScriptGraphEditor::Path() const { return ActiveDoc() ? &ActiveDoc()->path : nullptr; }
bool ScriptGraphEditor::Dirty() const { return ActiveDoc() && ActiveDoc()->revision != ActiveDoc()->savedRevision; }
bool ScriptGraphEditor::AnyDirty() const
{
    return std::ranges::any_of(m_Docs, [](const auto& d) { return d->revision != d->savedRevision; });
}

void ScriptGraphEditor::Activate(std::size_t index)
{
    if (index >= m_Docs.size())
        return;
    if (index != m_Active) {
        m_WidgetSession.reset();
        *m_Drag = DragState{};
    }
    m_Active                  = index;
    m_Docs[index]->selectTab  = true;
}

// --- Undo ---------------------------------------------------------------------------------------

void ScriptGraphEditor::Changed(Document& doc) { doc.revision = ++m_RevisionCounter; }

void ScriptGraphEditor::PushUndo(Document& doc, std::string before, std::uint64_t revision)
{
    doc.undo.emplace_back(std::move(before), revision);
    if (doc.undo.size() > kMaxUndo)
        doc.undo.erase(doc.undo.begin());
    doc.redo.clear();
}

void ScriptGraphEditor::Edit(const char* /*label*/, const std::function<void(ScriptGraph&)>& change)
{
    Document* doc = ActiveDoc();
    if (!doc)
        return;
    std::string         before   = ScriptGraphToJson(doc->graph);
    const std::uint64_t revision = doc->revision;
    change(doc->graph);
    if (ScriptGraphToJson(doc->graph) == before)
        return; // nothing happened (e.g. a rejected connection)
    PushUndo(*doc, std::move(before), revision);
    Changed(*doc);
}

bool ScriptGraphEditor::Undo()
{
    Document* doc = ActiveDoc();
    if (!doc || doc->undo.empty())
        return false;
    m_WidgetSession.reset();
    doc->redo.emplace_back(ScriptGraphToJson(doc->graph), doc->revision);
    auto [json, revision] = std::move(doc->undo.back());
    doc->undo.pop_back();
    doc->graph    = ScriptGraphFromJson(json);
    doc->revision = revision;
    std::erase_if(doc->selected, [&](std::uint32_t id) { return !doc->graph.FindNode(id); });
    return true;
}

bool ScriptGraphEditor::Redo()
{
    Document* doc = ActiveDoc();
    if (!doc || doc->redo.empty())
        return false;
    m_WidgetSession.reset();
    doc->undo.emplace_back(ScriptGraphToJson(doc->graph), doc->revision);
    auto [json, revision] = std::move(doc->redo.back());
    doc->redo.pop_back();
    doc->graph    = ScriptGraphFromJson(json);
    doc->revision = revision;
    std::erase_if(doc->selected, [&](std::uint32_t id) { return !doc->graph.FindNode(id); });
    return true;
}

bool ScriptGraphEditor::CanUndo() const { return ActiveDoc() && !ActiveDoc()->undo.empty(); }
bool ScriptGraphEditor::CanRedo() const { return ActiveDoc() && !ActiveDoc()->redo.empty(); }

const std::vector<ScriptDiagnostic>& ScriptGraphEditor::Diagnostics()
{
    static const std::vector<ScriptDiagnostic> none;
    Document* doc = ActiveDoc();
    if (!doc)
        return none;
    if (doc->diagnosed != doc->revision) {
        doc->diagnostics = ValidateScriptGraph(doc->graph);
        doc->diagnosed   = doc->revision;
    }
    return doc->diagnostics;
}

// --- Selection operations -----------------------------------------------------------------------

const std::vector<std::uint32_t>& ScriptGraphEditor::SelectedNodes() const
{
    static const std::vector<std::uint32_t> none;
    return ActiveDoc() ? ActiveDoc()->selected : none;
}

void ScriptGraphEditor::Select(std::vector<std::uint32_t> nodes)
{
    if (Document* doc = ActiveDoc()) {
        doc->selected        = std::move(nodes);
        doc->selectedComment = 0;
    }
}

void ScriptGraphEditor::DeleteSelection()
{
    Document* doc = ActiveDoc();
    if (!doc || (doc->selected.empty() && !doc->selectedComment))
        return;
    const std::vector<std::uint32_t> nodes   = doc->selected;
    const std::uint32_t              comment = doc->selectedComment;
    Edit("Delete", [&](ScriptGraph& g) {
        for (std::uint32_t id : nodes)
            if (const ScriptNode* n = g.FindNode(id); n && n->type != "Function.Entry" && n->type != "Macro.Inputs") // starts stay
                g.RemoveNode(id);
        if (comment)
            g.RemoveComment(comment);
    });
    doc->selected.clear();
    doc->selectedComment = 0;
}

std::string ScriptGraphEditor::CopySelection() const
{
    const Document* doc = ActiveDoc();
    if (!doc || doc->selected.empty())
        return {};
    ScriptGraph part;
    for (std::uint32_t id : doc->selected)
        if (const ScriptNode* n = doc->graph.FindNode(id))
            part.nodes.push_back(*n);
    for (const ScriptLink& l : doc->graph.links)
        if (part.FindNode(l.fromNode) && part.FindNode(l.toNode))
            part.links.push_back(l);
    part.nextId = doc->graph.nextId;
    return ScriptGraphToJson(part);
}

void ScriptGraphEditor::Paste(const std::string& clipboard, glm::vec2 at)
{
    Document* doc = ActiveDoc();
    if (!doc || clipboard.empty())
        return;
    ScriptGraph part;
    try {
        part = ScriptGraphFromJson(clipboard);
    } catch (const std::exception&) {
        return; // not a graph
    }
    if (part.nodes.empty())
        return;
    glm::vec2 minPos(std::numeric_limits<float>::max());
    for (const ScriptNode& n : part.nodes)
        minPos = glm::min(minPos, n.position);
    std::vector<std::uint32_t> pasted;
    const std::string          scope = doc->scope;
    Edit("Paste", [&](ScriptGraph& g) {
        std::map<std::uint32_t, std::uint32_t> ids;
        for (const ScriptNode& n : part.nodes) {
            if (IsFunctionFrame(n)) // one Entry / Return per function
                continue;
            const std::uint32_t id = g.AddNode(n.type, n.position - minPos + at, n.param, scope);
            g.FindNode(id)->defaults = n.defaults;
            ids[n.id]                = id;
            pasted.push_back(id);
        }
        for (const ScriptLink& l : part.links)
            if (ids.contains(l.fromNode) && ids.contains(l.toNode))
                (void)g.Connect(ids[l.fromNode], l.fromPin, ids[l.toNode], l.toPin);
    });
    doc->selected        = std::move(pasted);
    doc->selectedComment = 0;
}

void ScriptGraphEditor::DuplicateSelection()
{
    Document* doc = ActiveDoc();
    if (!doc || doc->selected.empty())
        return;
    glm::vec2 minPos(std::numeric_limits<float>::max());
    for (std::uint32_t id : doc->selected)
        if (const ScriptNode* n = doc->graph.FindNode(id))
            minPos = glm::min(minPos, n->position);
    Paste(CopySelection(), minPos + glm::vec2(30.0f, 30.0f));
}

void ScriptGraphEditor::CommentSelection()
{
    Document* doc = ActiveDoc();
    if (!doc)
        return;
    glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (std::uint32_t id : doc->selected)
        if (const ScriptNode* n = doc->graph.FindNode(id)) {
            const NodeLayout l = Layout(doc->graph, *n);
            lo                 = glm::min(lo, l.pos);
            hi                 = glm::max(hi, l.pos + l.size);
        }
    if (lo.x > hi.x) { // nothing selected: a comment at the view center
        lo = -doc->scroll + glm::vec2(40.0f);
        hi = lo + glm::vec2(300.0f, 160.0f);
    }
    std::uint32_t     id    = 0;
    const std::string scope = doc->scope;
    Edit("Comment", [&](ScriptGraph& g) {
        id = g.AddComment(lo - glm::vec2(20.0f, 44.0f), hi - lo + glm::vec2(40.0f, 64.0f), "Comment", scope);
    });
    doc->selectedComment = id;
}

bool ScriptGraphEditor::EditType(const char* id, PinType& type, float width) { return TypeCombo(id, type, width); }

bool ScriptGraphEditor::EditValue(const char* id, ScriptValue& value, PinType type, float width)
{
    return ValueWidget(id, value, type, width);
}

void ScriptGraphEditor::AlignSelection(Align how)
{
    Document* doc = ActiveDoc();
    if (!doc || doc->selected.size() < 2)
        return;
    struct Box {
        std::uint32_t id;
        glm::vec2     pos, size;
    };
    std::vector<Box> boxes;
    for (std::uint32_t id : doc->selected)
        if (const ScriptNode* n = doc->graph.FindNode(id)) {
            const NodeLayout l = Layout(doc->graph, *n);
            boxes.push_back({id, l.pos, l.size});
        }
    if (boxes.size() < 2)
        return;
    glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (const Box& b : boxes) {
        lo = glm::min(lo, b.pos);
        hi = glm::max(hi, b.pos + b.size);
    }
    const auto distribute = [&](int axis) {
        std::ranges::sort(boxes, [&](const Box& a, const Box& b) { return a.pos[axis] < b.pos[axis]; });
        float total = 0.0f;
        for (const Box& b : boxes)
            total += b.size[axis];
        const float gap = (hi[axis] - lo[axis] - total) / static_cast<float>(boxes.size() - 1);
        float       at  = lo[axis];
        for (Box& b : boxes) {
            b.pos[axis] = at;
            at += b.size[axis] + std::max(gap, 16.0f);
        }
    };
    switch (how) {
    case Align::Left: for (Box& b : boxes) b.pos.x = lo.x; break;
    case Align::Right: for (Box& b : boxes) b.pos.x = hi.x - b.size.x; break;
    case Align::Top: for (Box& b : boxes) b.pos.y = lo.y; break;
    case Align::Bottom: for (Box& b : boxes) b.pos.y = hi.y - b.size.y; break;
    case Align::CenterX: for (Box& b : boxes) b.pos.x = (lo.x + hi.x - b.size.x) * 0.5f; break;
    case Align::CenterY: for (Box& b : boxes) b.pos.y = (lo.y + hi.y - b.size.y) * 0.5f; break;
    case Align::DistributeX: distribute(0); break;
    case Align::DistributeY: distribute(1); break;
    }
    Edit("Align", [&](ScriptGraph& g) {
        for (const Box& b : boxes)
            if (ScriptNode* n = g.FindNode(b.id))
                n->position = glm::round(b.pos);
    });
}

void ScriptGraphEditor::ToggleBreakpoints()
{
    Document* doc = ActiveDoc();
    if (!doc || doc->selected.empty())
        return;
    const std::vector<std::uint32_t> nodes = doc->selected;
    const bool enable = std::ranges::any_of(nodes, [&](std::uint32_t id) { return !doc->graph.HasBreakpoint(id); });
    Edit("Breakpoint", [&](ScriptGraph& g) {
        for (std::uint32_t id : nodes)
            if (const ScriptNode* n = g.FindNode(id)) {
                const NodeDesc* desc = FindScriptNodeType(n->type);
                if (!enable || (desc && desc->kind == NodeKind::Impure)) // exec nodes only
                    g.SetBreakpoint(id, enable);
            }
    });
}

void ScriptGraphEditor::OpenScope(const std::string& function)
{
    if (Document* doc = ActiveDoc(); doc && doc->scope != function) {
        doc->scope = function;
        doc->selected.clear();
        doc->selectedComment = 0;
        doc->framed          = false; // fit the new scope once
        *m_Drag              = DragState{};
    }
}

const std::string* ScriptGraphEditor::Scope() const { return ActiveDoc() ? &ActiveDoc()->scope : nullptr; }

void ScriptGraphEditor::ProvideTo(ScriptSystem& scripts) const
{
    std::unordered_set<std::string> open;
    for (const auto& doc : m_Docs) {
        scripts.Provide(doc->path, doc->graph);
        open.insert(ScriptSystem::Key(doc->path));
    }
    for (const auto& [key, path] : m_Provided) // closed since: their files count again
        if (!open.contains(key))
            scripts.Provide(path, std::nullopt);
    m_Provided.clear();
    for (const auto& doc : m_Docs)
        m_Provided.emplace(ScriptSystem::Key(doc->path), doc->path);
}

// --- Window -------------------------------------------------------------------------------------

void ScriptGraphEditor::SyncDebugger(ScriptSystem* debug)
{
    DragState& drag = *m_Drag;
    if (!debug || !debug->Running()) {
        // Play compiles the graphs with their saved breakpoints.
        for (auto& d : m_Docs) {
            d->syncedBreakpoints = d->graph.breakpoints;
            d->syncedOptions     = d->graph.breakpointOptions;
        }
        drag.pausedNode   = 0;
        drag.pausedEntity = NullEntity;
        m_Watch.reset();
        return;
    }
    for (auto& d : m_Docs) // toggled / changed while playing
        if (d->syncedBreakpoints != d->graph.breakpoints || d->syncedOptions != d->graph.breakpointOptions) {
            std::vector<ScriptBreakpoint> list;
            for (std::uint32_t node : d->graph.breakpoints) {
                const auto o = d->graph.breakpointOptions.find(node);
                list.push_back({node, o != d->graph.breakpointOptions.end() ? o->second : ScriptBreakpointOptions{}});
            }
            debug->SetBreakpointList(d->path, list);
            d->syncedBreakpoints = d->graph.breakpoints;
            d->syncedOptions     = d->graph.breakpointOptions;
        }
    const std::optional<ScriptDebugFrame> at = debug->PausedAt();
    if (!at) {
        drag.pausedNode   = 0;
        drag.pausedEntity = NullEntity;
    } else if (at->node != drag.pausedNode || at->entity != drag.pausedEntity) {
        // A new stop: open the graph on the paused node.
        drag.pausedNode   = at->node;
        drag.pausedEntity = at->entity;
        if (Open(std::filesystem::path(at->file))) {
            Document& doc = *ActiveDoc();
            OpenScope(at->function);
            doc.selected    = {at->node};
            doc.watchEntity = at->entity;
            FrameNodes(doc, true);
            doc.framed       = true;
            m_FocusRequested = true;
        }
    }
    m_Watch.reset();
    if (const Document* doc = ActiveDoc()) {
        const std::vector<Entity> instances = debug->InstancesOf(doc->path);
        if (!instances.empty()) {
            Entity watched = doc->watchEntity;
            if (std::ranges::find(instances, watched) == instances.end())
                watched = instances.front();
            m_Watch = debug->Watch(doc->path, watched);
        }
    }
}

void ScriptGraphEditor::Draw(bool* open, ScriptSystem* debug, Scene* scene)
{
    m_Scene = scene;
    SyncDebugger(debug);
    DrawSearch(); // its own window, also without open graphs
    if (m_FocusRequested) {
        ImGui::SetNextWindowFocus();
        m_FocusRequested = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    const bool visible = ImGui::Begin("Blueprint", open);
    ImGui::PopStyleVar();
    if (!visible) {
        m_Focused = false;
        ImGui::End();
        DrawDialog();
        return;
    }
    m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    DrawToolbar(debug, scene);

    if (m_Docs.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No blueprint open. Create one with New... or open a .ugraph file,");
        ImGui::TextDisabled("or use \"Edit\" on a Script component in the Inspector.");
        ImGui::End();
        DrawDialog();
        return;
    }

    // Tabs, one per open graph.
    std::optional<std::size_t> close;
    if (ImGui::BeginTabBar("graphs", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (std::size_t i = 0; i < m_Docs.size(); ++i) {
            Document&         d     = *m_Docs[i];
            bool              keep  = true;
            const std::string label = d.path.filename().string() + "###" + d.path.string();
            ImGuiTabItemFlags flags = d.revision != d.savedRevision ? ImGuiTabItemFlags_UnsavedDocument : 0;
            if (std::exchange(d.selectTab, false))
                flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem(label.c_str(), &keep, flags)) {
                if (m_Active != i) {
                    m_WidgetSession.reset();
                    *m_Drag = DragState{};
                }
                m_Active = i;
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", d.path.string().c_str());
            if (!keep)
                close = i;
        }
        ImGui::EndTabBar();
    }
    if (close) {
        if (m_Docs[*close]->revision != m_Docs[*close]->savedRevision)
            m_Drag->confirmClose = close;
        else
            Close(*close);
    }
    if (m_Docs.empty()) {
        ImGui::End();
        return;
    }

    Document& doc = *m_Docs[m_Active];
    m_FrameJson   = ScriptGraphToJson(doc.graph); // widget edits this frame undo to this
    m_WidgetTouched = false;

    if (ImGui::BeginChild("sidebar", ImVec2(260.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) {
        DrawFunctions(doc);
        DrawMembers(doc);
        DrawSidebar(doc);
        DrawDetails(doc);
        DrawWatch(doc, debug);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("main", ImVec2(0.0f, 0.0f))) {
        DrawCanvas(doc, debug);
        DrawResults(doc, debug);
    }
    ImGui::EndChild();
    DrawTimeline(doc);

    if (m_Focused)
        HandleKeys(doc, debug, scene);

    // Widget edits: one undo step per interaction (session until no item is active).
    if (m_WidgetTouched) {
        if (!m_WidgetSession)
            m_WidgetSession.emplace(m_FrameJson, doc.revision);
        Changed(doc);
    }
    if (m_WidgetSession && !ImGui::IsAnyItemActive()) {
        PushUndo(doc, std::move(m_WidgetSession->first), m_WidgetSession->second);
        m_WidgetSession.reset();
    }

    // Closing an unsaved graph.
    if (m_Drag->confirmClose)
        ImGui::OpenPopup("Close blueprint?");
    if (ImGui::BeginPopupModal("Close blueprint?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::size_t index = m_Drag->confirmClose.value_or(0);
        ImGui::Text("'%s' has unsaved changes.", index < m_Docs.size() ? m_Docs[index]->path.filename().string().c_str() : "");
        if (ImGui::Button("Save and close")) {
            m_Active = index;
            if (Save())
                Close(index);
            m_Drag->confirmClose.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            Close(index);
            m_Drag->confirmClose.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            m_Drag->confirmClose.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
    DrawDialog();
}

void ScriptGraphEditor::DrawToolbar(ScriptSystem* debug, Scene* scene)
{
    std::error_code ec;
    const std::filesystem::path dir = ActiveDoc() ? ActiveDoc()->path.parent_path() : std::filesystem::current_path(ec);
    if (ImGui::Button("New...")) {
        m_DialogPurpose = DialogPurpose::New;
        m_Dialog->Open("New blueprint", FileDialog::Mode::Save, dir, {".ugraph"}, "NewBlueprint.ugraph");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open...")) {
        m_DialogPurpose = DialogPurpose::Open;
        m_Dialog->Open("Open blueprint", FileDialog::Mode::Open, dir, {".ugraph"});
    }
    ImGui::BeginDisabled(!ActiveDoc());
    ImGui::SameLine();
    if (ImGui::Button("Save"))
        Save();
    ImGui::SameLine();
    if (ImGui::Button("Save As...")) {
        m_DialogPurpose = DialogPurpose::SaveAs;
        m_Dialog->Open("Save blueprint as", FileDialog::Mode::Save, dir, {".ugraph"},
                       ActiveDoc() ? ActiveDoc()->path.filename().string() : std::string());
    }
    ImGui::SameLine();
    const std::size_t errors = ActiveDoc() ? static_cast<std::size_t>(std::ranges::count_if(
                                                 Diagnostics(), [](const ScriptDiagnostic& d) { return d.error; }))
                                           : 0;
    ImGui::PushStyleColor(ImGuiCol_Button, errors ? ImVec4(0.6f, 0.15f, 0.15f, 1.0f) : ImVec4(0.15f, 0.45f, 0.2f, 1.0f));
    if (ImGui::Button(errors ? "Compile (errors)" : "Compile (ok)") && ActiveDoc())
        ActiveDoc()->diagnosed = ~std::uint64_t{0}; // validate again now
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanUndo());
    if (ImGui::Button("Undo"))
        Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!CanRedo());
    if (ImGui::Button("Redo"))
        Redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Frame") && ActiveDoc())
        FrameNodes(*ActiveDoc(), false);
    ImGui::EndDisabled();
    if (Document* doc = ActiveDoc()) {
        ImGui::SameLine();
        ImGui::Checkbox("Map", &doc->showMinimap);
        ImGui::SameLine();
        ImGui::TextDisabled("%d%%  %zu nodes", static_cast<int>(std::round(doc->zoom * 100.0f)), doc->graph.nodes.size());
    }
    if (debug && debug->Running()) {
        ImGui::SameLine();
        if (const auto at = debug->PausedAt()) {
            const ScriptNode* node = nullptr;
            for (const auto& d : m_Docs)
                if (ScriptSystem::Key(d->path) == at->file)
                    node = d->graph.FindNode(at->node);
            const NodeDesc* desc = node ? FindScriptNodeType(node->type) : nullptr;
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "  Paused at %s", node ? NodeTitle(desc, *node).c_str() : "?");
            ImGui::SameLine();
            ImGui::BeginDisabled(!scene);
            if (ImGui::Button("Continue (F5)") && scene)
                debug->DebugContinue(*scene);
            ImGui::SameLine();
            if (ImGui::Button("Over (F10)") && scene)
                debug->DebugStepOver(*scene);
            ImGui::SameLine();
            if (ImGui::Button("Into (F11)") && scene)
                debug->DebugStep(*scene);
            ImGui::SameLine();
            if (ImGui::Button("Out (Shift+F11)") && scene)
                debug->DebugStepOut(*scene);
            ImGui::EndDisabled();
        } else {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "  Playing - execution highlighted, F9 = breakpoint");
        }
    }
}

void ScriptGraphEditor::DrawDialog()
{
    const std::optional<std::filesystem::path> file = m_Dialog->Draw();
    if (!file)
        return;
    switch (m_DialogPurpose) {
    case DialogPurpose::Open: Open(*file); break;
    case DialogPurpose::New: New(*file); break;
    case DialogPurpose::SaveAs: SaveAs(*file); break;
    case DialogPurpose::None: break;
    }
    m_DialogPurpose = DialogPurpose::None;
}

// --- Sidebar: variables + details ---------------------------------------------------------------

void ScriptGraphEditor::DrawFunctions(Document& doc)
{
    ScriptGraph& g = doc.graph;
    ImGui::SeparatorText(g.library ? "Library" : "Graphs");
    if (g.library)
        ImGui::TextDisabled("Functions and macros for every blueprint\n(<file name>.<name>); no event graph.");
    else if (ImGui::Selectable("Event Graph", doc.scope.empty()))
        OpenScope({});
    std::optional<std::string> removeFunction;
    for (const ScriptFunction& f : g.functions) {
        ImGui::PushID(f.name.c_str());
        const std::string label = (f.pure ? "f(x) " : "f() ") + f.name;
        if (ImGui::Selectable(label.c_str(), doc.scope == f.name))
            OpenScope(f.name);
        if (ImGui::IsItemHovered() && !f.description.empty())
            ImGui::SetTooltip("%s", f.description.c_str());
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Open"))
                OpenScope(f.name);
            if (ImGui::MenuItem("Find references"))
                OpenSearch(f.name, true);
            if (ImGui::MenuItem("Delete function"))
                removeFunction = f.name;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (removeFunction) {
        const std::string name = *removeFunction;
        if (doc.scope == name)
            OpenScope({});
        Edit("Remove function", [&](ScriptGraph& graph) { graph.RemoveFunction(name); });
        std::erase_if(doc.selected, [&](std::uint32_t id) { return !doc.graph.FindNode(id); });
    }
    if (ImGui::SmallButton("+ Function")) {
        const std::string name = UniqueName(g, {}, "NewFunction", true);
        Edit("Add function", [&](ScriptGraph& graph) { graph.AddFunction(name, {0.0f, 0.0f}); });
        OpenScope(name);
    }
    // Macros: copied into the graph wherever used (latent nodes and several exec pins allowed).
    std::optional<std::string> removeMacro;
    for (const ScriptMacro& m : g.macros) {
        ImGui::PushID(("macro:" + m.name).c_str());
        if (ImGui::Selectable(("[M] " + m.name).c_str(), doc.scope == m.name))
            OpenScope(m.name);
        if (ImGui::IsItemHovered() && !m.description.empty())
            ImGui::SetTooltip("%s", m.description.c_str());
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Find references"))
                OpenSearch(m.name, true);
            if (ImGui::MenuItem("Delete macro"))
                removeMacro = m.name;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (removeMacro) {
        const std::string name = *removeMacro;
        if (doc.scope == name)
            OpenScope({});
        Edit("Remove macro", [&](ScriptGraph& graph) { graph.RemoveMacro(name); });
        std::erase_if(doc.selected, [&](std::uint32_t id) { return !doc.graph.FindNode(id); });
    }
    if (ImGui::SmallButton("+ Macro")) {
        const std::string name = UniqueName(g, {}, "NewMacro", true);
        Edit("Add macro", [&](ScriptGraph& graph) { graph.AddMacro(name, {0.0f, 0.0f}); });
        OpenScope(name);
    }
    if (ScriptMacro* m = doc.scope.empty() ? nullptr : g.FindMacro(doc.scope)) {
        DrawMacro(doc, *m);
        return;
    }

    ScriptFunction* f = doc.scope.empty() ? nullptr : g.FindFunction(doc.scope);
    if (!f)
        return;
    ImGui::SeparatorText("Function");
    std::string name = f->name;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##fname", &name);
    if (ImGui::IsItemDeactivatedAfterEdit() && name != f->name) {
        if (!IsValidScriptName(name) || g.FindFunction(name)) {
            m_LastError = "Function names must be unique (letters, digits, '_', ' ')";
        } else {
            const std::string from = f->name;
            Edit("Rename function", [&](ScriptGraph& graph) { graph.RenameFunction(from, name); });
            doc.scope = name;
        }
        return; // f may be stale
    }
    bool pure = f->pure;
    if (ImGui::Checkbox("Pure (no exec pins)", &pure)) {
        const std::string fn = f->name;
        Edit("Function purity", [&](ScriptGraph& graph) { graph.SetFunctionPure(fn, pure); });
        return;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    m_WidgetTouched |= ImGui::InputTextWithHint("##desc", "Description (tooltip)", &f->description);

    // Inputs / outputs: renaming keeps the links, type changes drop the ones that no longer fit.
    const auto paramList = [&](const char* title, bool outputs) {
        ImGui::SeparatorText(title);
        std::vector<ScriptParam>& params = outputs ? f->outputs : f->inputs;
        std::optional<std::size_t> remove;
        for (std::size_t i = 0; i < params.size(); ++i) {
            ImGui::PushID(static_cast<int>(i) + (outputs ? 1000 : 0));
            std::string pname = params[i].name;
            ImGui::SetNextItemWidth(100.0f);
            ImGui::InputText("##pname", &pname);
            if (ImGui::IsItemDeactivatedAfterEdit() && pname != params[i].name && IsValidScriptName(pname) &&
                std::ranges::none_of(params, [&](const ScriptParam& p) { return p.name == pname; })) {
                const std::string fn = f->name, from = params[i].name;
                Edit("Rename parameter", [&](ScriptGraph& graph) {
                    ScriptFunction* fun = graph.FindFunction(fn);
                    for (ScriptParam& p : outputs ? fun->outputs : fun->inputs)
                        if (p.name == from)
                            p.name = pname;
                    for (ScriptLink& l : graph.links) { // Entry / Call / Return pins of this function
                        const ScriptNode* a = graph.FindNode(l.fromNode);
                        const ScriptNode* b = graph.FindNode(l.toNode);
                        if (!outputs && a && a->type == "Function.Entry" && a->function == fn && l.fromPin == from)
                            l.fromPin = pname;
                        if (!outputs && b && b->type.starts_with("Function.Call") && b->param == fn && l.toPin == from)
                            l.toPin = pname;
                        if (outputs && b && b->type == "Function.Return" && b->function == fn && l.toPin == from)
                            l.toPin = pname;
                        if (outputs && a && a->type.starts_with("Function.Call") && a->param == fn && l.fromPin == from)
                            l.fromPin = pname;
                    }
                    for (ScriptNode& n : graph.nodes) // unconnected values of Call / Return inputs
                        if (((!outputs && n.type.starts_with("Function.Call") && n.param == fn) ||
                             (outputs && n.type == "Function.Return" && n.function == fn)))
                            if (auto it = n.defaults.find(from); it != n.defaults.end()) {
                                n.defaults[pname] = it->second;
                                n.defaults.erase(from);
                            }
                    graph.FunctionSignatureChanged(fn);
                });
                ImGui::PopID();
                return true;
            }
            ImGui::SameLine();
            PinType type = params[i].type;
            if (TypeCombo("##ptype", type, 96.0f)) {
                const std::string fn = f->name;
                Edit("Parameter type", [&](ScriptGraph& graph) {
                    ScriptFunction* fun                          = graph.FindFunction(fn);
                    (outputs ? fun->outputs : fun->inputs)[i].type = type;
                    graph.FunctionSignatureChanged(fn);
                });
                ImGui::PopID();
                return true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                remove = i;
            ImGui::PopID();
        }
        if (remove) {
            const std::string fn = f->name;
            Edit("Remove parameter", [&](ScriptGraph& graph) {
                ScriptFunction* fun = graph.FindFunction(fn);
                auto&           ps  = outputs ? fun->outputs : fun->inputs;
                ps.erase(ps.begin() + static_cast<std::ptrdiff_t>(*remove));
                graph.FunctionSignatureChanged(fn);
            });
            return true;
        }
        if (ImGui::SmallButton(outputs ? "+ Output" : "+ Input")) {
            std::string pname = outputs ? "Result" : "Value";
            for (int n = 1; std::ranges::any_of(params, [&](const ScriptParam& p) { return p.name == pname; }); ++n)
                pname = (outputs ? "Result" : "Value") + std::to_string(n);
            const std::string fn = f->name;
            Edit("Add parameter", [&](ScriptGraph& graph) {
                ScriptFunction* fun = graph.FindFunction(fn);
                (outputs ? fun->outputs : fun->inputs).push_back({pname, PinType::Float});
                graph.FunctionSignatureChanged(fn);
            });
            return true;
        }
        return false;
    };
    if (paramList("Inputs", false) || paramList("Outputs", true))
        return;

    // Locals: reset at every call.
    ImGui::SeparatorText("Local variables");
    const glm::vec2            center = -doc.scroll + glm::vec2(80.0f, 80.0f);
    std::optional<std::string> removeLocal;
    for (std::size_t i = 0; i < f->locals.size(); ++i) {
        ScriptVariable& v = f->locals[i];
        ImGui::PushID(static_cast<int>(i) + 5000);
        std::string lname = v.name;
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputText("##lname", &lname);
        if (ImGui::IsItemDeactivatedAfterEdit() && lname != v.name) {
            const std::string fn = f->name, from = v.name;
            Edit("Rename local", [&](ScriptGraph& graph) { graph.RenameVariable(from, lname, fn); });
            ImGui::PopID();
            return;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Get")) {
            const std::string var = v.name, fn = f->name;
            Edit("Add Get", [&](ScriptGraph& graph) { doc.selected = {graph.AddNode("Variable.Get", center, var, fn)}; });
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Set")) {
            const std::string var = v.name, fn = f->name;
            Edit("Add Set", [&](ScriptGraph& graph) { doc.selected = {graph.AddNode("Variable.Set", center, var, fn)}; });
        }
        ImGui::Indent();
        PinType type = v.type;
        if (TypeCombo("##ltype", type, 96.0f)) {
            const std::string fn = f->name, var = v.name;
            Edit("Local type", [&](ScriptGraph& graph) { graph.SetVariableType(var, type, fn); });
            ImGui::Unindent();
            ImGui::PopID();
            return;
        }
        ImGui::SameLine();
        if (v.type != PinType::Entity)
            m_WidgetTouched |= ValueWidget("##lvalue", v.value, v.type, -24.0f);
        else
            ImGui::TextDisabled("none");
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            removeLocal = v.name;
        ImGui::Unindent();
        ImGui::PopID();
    }
    if (removeLocal) {
        const std::string fn = f->name, var = *removeLocal;
        Edit("Remove local", [&](ScriptGraph& graph) {
            ScriptFunction* fun = graph.FindFunction(fn);
            std::erase_if(fun->locals, [&](const ScriptVariable& v) { return v.name == var; });
            std::vector<std::uint32_t> users;
            for (const ScriptNode& n : graph.nodes)
                if (const NodeDesc* d = FindScriptNodeType(n.type);
                    n.function == fn && d && d->paramKind == ParamKind::Variable && n.param == var && !graph.FindVariable(var))
                    users.push_back(n.id);
            for (std::uint32_t id : users)
                graph.RemoveNode(id);
            graph.RemoveDanglingLinks();
        });
        std::erase_if(doc.selected, [&](std::uint32_t id) { return !doc.graph.FindNode(id); });
    }
    if (ImGui::SmallButton("+ Local")) {
        const std::string fn = f->name, lname = UniqueName(g, f->name, "Local");
        Edit("Add local", [&](ScriptGraph& graph) { graph.FindFunction(fn)->locals.push_back({lname, PinType::Float, 0.0f}); });
    }
}

void ScriptGraphEditor::DrawSidebar(Document& doc)
{
    ImGui::SeparatorText("Variables");
    ScriptGraph& g = doc.graph;
    if (ImGui::SmallButton("+ Variable")) {
        const std::string name = UniqueName(g, {}, "NewVar");
        Edit("Add variable", [&](ScriptGraph& graph) { graph.variables.push_back({name, PinType::Float, 0.0f}); });
    }
    std::optional<std::string> remove;
    const glm::vec2            center = -doc.scroll + glm::vec2(80.0f, 80.0f);
    const std::string          scope  = doc.scope;
    for (std::size_t i = 0; i < g.variables.size(); ++i) {
        ScriptVariable& v = g.variables[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(PinColor(v.type)));
        ImGui::Bullet();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        std::string name = v.name;
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputText("##name", &name);
        if (ImGui::IsItemDeactivatedAfterEdit() && name != v.name) {
            const std::string from = v.name;
            Edit("Rename variable", [&](ScriptGraph& graph) { graph.RenameVariable(from, name); });
            ImGui::PopID();
            break; // references changed
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Get")) {
            const std::string var = v.name;
            Edit("Add Get", [&](ScriptGraph& graph) { doc.selected = {graph.AddNode("Variable.Get", center, var, scope)}; });
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Set")) {
            const std::string var = v.name;
            Edit("Add Set", [&](ScriptGraph& graph) { doc.selected = {graph.AddNode("Variable.Set", center, var, scope)}; });
        }
        // Type, initial value, instance editable.
        ImGui::Indent();
        PinType type = v.type;
        if (TypeCombo("##type", type, 96.0f)) {
            const std::string var = v.name;
            Edit("Variable type", [&](ScriptGraph& graph) { graph.SetVariableType(var, type); });
            ImGui::Unindent();
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (v.type != PinType::Entity)
            m_WidgetTouched |= ValueWidget("##value", v.value, v.type, -50.0f);
        else
            ImGui::TextDisabled("none");
        ImGui::SameLine();
        m_WidgetTouched |= ImGui::Checkbox("##exposed", &v.exposed);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Instance editable: set per entity in the Inspector");
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            remove = v.name;
        ImGui::Unindent();
        ImGui::PopID();
    }
    if (remove) {
        const std::string var = *remove;
        Edit("Remove variable", [&](ScriptGraph& graph) {
            std::erase_if(graph.variables, [&](const ScriptVariable& v) { return v.name == var; });
            std::vector<std::uint32_t> users; // nodes naming it (not a function's local of the same name)
            for (const ScriptNode& n : graph.nodes)
                if (const NodeDesc* d = FindScriptNodeType(n.type);
                    d && d->paramKind == ParamKind::Variable && n.param == var && !graph.FindVariableInScope(n.function, var))
                    users.push_back(n.id);
            for (std::uint32_t id : users)
                graph.RemoveNode(id);
        });
        std::erase_if(doc.selected, [&](std::uint32_t id) { return !doc.graph.FindNode(id); });
    }
}

void ScriptGraphEditor::DrawWatch(Document& doc, const ScriptSystem* debug)
{
    if (!debug || !debug->Running())
        return;
    ImGui::SeparatorText("Debug");
    const std::vector<Entity> instances = debug->InstancesOf(doc.path);
    if (instances.empty()) {
        ImGui::TextDisabled("No entity runs this graph");
        return;
    }
    const auto label = [&](Entity e) {
        const Name* name = m_Scene ? m_Scene->GetRegistry().TryGet<Name>(e) : nullptr;
        return name ? name->value : std::format("entity {}", EntityIndex(e));
    };
    if (std::ranges::find(instances, doc.watchEntity) == instances.end())
        doc.watchEntity = instances.front();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##instance", label(doc.watchEntity).c_str())) {
        for (Entity e : instances)
            if (ImGui::Selectable(std::format("{}##{}", label(e), static_cast<std::uint64_t>(e)).c_str(), e == doc.watchEntity))
                doc.watchEntity = e;
        ImGui::EndCombo();
    }
    if (!m_Watch)
        return;
    const auto table = [](const char* id, const std::vector<std::pair<std::string, ScriptValue>>& values) {
        if (values.empty() || !ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
            return;
        for (const auto& [name, value] : values) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(PinColor(TypeOf(value))));
            ImGui::TextUnformatted(name.c_str());
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ToDisplayString(value).c_str());
        }
        ImGui::EndTable();
    };
    if (const auto at = debug->PausedAt(); at && at->entity == m_Watch->entity) {
        ImGui::TextDisabled("Call stack");
        std::vector<ScriptDebugFrame> frames = at->callers;
        frames.push_back(*at);
        for (std::size_t i = frames.size(); i-- > 0;) { // innermost first
            const ScriptDebugFrame& f = frames[i];
            const std::string frameLabel = std::format("{} {}##frame{}", f.function.empty() ? "Event Graph" : f.function,
                                                       i + 1 == frames.size() ? "(paused)" : "(call)", i);
            if (ImGui::Selectable(frameLabel.c_str()))
                ShowHit({std::filesystem::path(f.file), f.function, f.node, {}});
        }
    }
    table("vars", m_Watch->variables);
    if (!m_Watch->locals.empty()) {
        ImGui::TextDisabled("Locals of %s", m_Watch->function.c_str());
        table("locals", m_Watch->locals);
    }
    ImGui::TextDisabled("Hover a pin for its last value");
}

void ScriptGraphEditor::DrawDetails(Document& doc)
{
    ImGui::SeparatorText("Details");
    ScriptGraph& g = doc.graph;
    if (ScriptComment* c = doc.selectedComment ? g.FindComment(doc.selectedComment) : nullptr) {
        ImGui::TextUnformatted("Comment");
        ImGui::SetNextItemWidth(-FLT_MIN);
        m_WidgetTouched |= ImGui::InputTextMultiline("##text", &c->text, ImVec2(-FLT_MIN, 60.0f));
        m_WidgetTouched |= ImGui::ColorEdit3("Color", &c->color.x, ImGuiColorEditFlags_NoInputs);
        static constexpr glm::vec3 kPresets[] = {{0.3f, 0.45f, 0.7f}, {0.25f, 0.6f, 0.35f}, {0.75f, 0.55f, 0.15f},
                                                 {0.7f, 0.25f, 0.25f}, {0.5f, 0.3f, 0.7f},  {0.4f, 0.4f, 0.45f}};
        for (std::size_t i = 0; i < std::size(kPresets); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (i)
                ImGui::SameLine();
            const glm::vec3 p = kPresets[i];
            if (ImGui::ColorButton("##preset", ImVec4(p.r, p.g, p.b, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f, 18.0f))) {
                c->color        = p;
                m_WidgetTouched = true;
            }
            ImGui::PopID();
        }
        ImGui::TextDisabled("Dragging the title moves the nodes inside.");
        return;
    }
    if (doc.selected.size() != 1) {
        ImGui::TextDisabled(doc.selected.empty() ? "Select a node" : "%zu nodes selected", doc.selected.size());
        return;
    }
    ScriptNode* node = g.FindNode(doc.selected.front());
    if (!node)
        return;
    const NodeDesc* desc = FindScriptNodeType(node->type);
    ImGui::TextUnformatted(desc ? desc->title.c_str() : node->type.c_str());
    ImGui::TextDisabled("%s  (id %u)", node->type.c_str(), node->id);
    if (desc && !desc->tooltip.empty())
        ImGui::TextWrapped("%s", desc->tooltip.c_str());
    if (!desc)
        return;
    if (g.HasBreakpoint(node->id)) { // condition / hit count
        ScriptBreakpointOptions options = g.breakpointOptions.contains(node->id) ? g.breakpointOptions[node->id] : ScriptBreakpointOptions{};
        bool changed = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::InputTextWithHint("##condition", "Break condition, e.g. health < 10", &options.condition);
        if (const std::string error = options.condition.empty() ? std::string() : CheckScriptCondition(options.condition); !error.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error.c_str());
        int hits = static_cast<int>(options.hitCount);
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputInt("Break from hit", &hits)) {
            options.hitCount = static_cast<std::uint32_t>(std::max(hits, 0));
            changed          = true;
        }
        if (changed) {
            if (options == ScriptBreakpointOptions{})
                g.breakpointOptions.erase(node->id);
            else
                g.breakpointOptions[node->id] = options;
            m_WidgetTouched = true;
        }
    }
    if (desc->kind == NodeKind::Impure) {
        bool breakpoint = g.HasBreakpoint(node->id);
        if (ImGui::Checkbox("Breakpoint (F9)", &breakpoint)) {
            const std::uint32_t id = node->id;
            Edit("Breakpoint", [&](ScriptGraph& graph) { graph.SetBreakpoint(id, breakpoint); });
            return;
        }
    }

    // Param: may change the pins -> structural edit.
    std::string param = node->param;
    bool        commit = false;
    switch (desc->paramKind) {
    case ParamKind::None: break;
    case ParamKind::Text:
    case ParamKind::InputAction:
    case ParamKind::InputAxis:
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##param", &param);
        commit = ImGui::IsItemDeactivatedAfterEdit();
        break;
    case ParamKind::Variable:
        if (ImGui::BeginCombo(desc->paramLabel.c_str(), param.c_str())) {
            const auto offer = [&](const ScriptVariable& v) {
                if (desc->arrayVariable && !IsArray(v.type))
                    return;
                if (ImGui::Selectable(v.name.c_str(), v.name == param)) {
                    param  = v.name;
                    commit = true;
                }
            };
            if (const ScriptFunction* f = g.FindFunction(node->function))
                for (const ScriptVariable& v : f->locals)
                    offer(v);
            for (const ScriptVariable& v : g.variables)
                offer(v);
            ImGui::EndCombo();
        }
        break;
    case ParamKind::Function:
        if (node->type == "Function.Entry" || node->type == "Function.Return") {
            ImGui::TextDisabled("Function %s: edit its signature in the sidebar", node->function.c_str());
            break;
        }
        if (ImGui::BeginCombo(desc->paramLabel.c_str(), param.c_str())) {
            for (const ScriptFunction& f : g.functions) // calls of the same purity, never the function itself
                if (f.pure == (desc->kind == NodeKind::Pure) && f.name != node->function &&
                    ImGui::Selectable(f.name.c_str(), f.name == param)) {
                    param  = f.name;
                    commit = true;
                }
            ImGui::EndCombo();
        }
        break;
    case ParamKind::Macro:
    case ParamKind::LibraryFunction:
    case ParamKind::Interface:
    case ParamKind::InterfaceFunction:
    case ParamKind::Dispatcher:
    case ParamKind::Timeline: {
        if (node->type == "Macro.Inputs" || node->type == "Macro.Outputs") {
            ImGui::TextDisabled("Macro %s: edit its pins in the sidebar", node->function.c_str());
            break;
        }
        std::vector<std::string> names;
        switch (desc->paramKind) {
        case ParamKind::Macro:
            for (const ScriptMacro& m : g.macros)
                if (m.name != node->function)
                    names.push_back(m.name);
            for (const std::string& lib : ScriptRegistry::LibraryNames())
                for (const ScriptMacro& m : ScriptRegistry::FindLibrary(lib)->graph.macros)
                    names.push_back(lib + "." + m.name);
            break;
        case ParamKind::LibraryFunction:
            for (const std::string& lib : ScriptRegistry::LibraryNames())
                for (const ScriptFunction& f : ScriptRegistry::FindLibrary(lib)->graph.functions)
                    if (f.pure == (desc->kind == NodeKind::Pure))
                        names.push_back(lib + "." + f.name);
            break;
        case ParamKind::Interface: names = ScriptRegistry::InterfaceNames(); break;
        case ParamKind::Timeline:
            for (const ScriptTimeline& t : g.timelines)
                names.push_back(t.name);
            break;
        case ParamKind::InterfaceFunction:
            for (const std::string& i : ScriptRegistry::InterfaceNames())
                for (const ScriptInterfaceFunction& f : ScriptRegistry::FindInterface(i)->functions)
                    names.push_back(i + "." + f.name);
            break;
        default:
            for (const ScriptEventDecl& d : g.dispatchers)
                names.push_back(d.name);
            break;
        }
        if (ImGui::BeginCombo(desc->paramLabel.c_str(), param.c_str())) {
            for (const std::string& name : names)
                if (ImGui::Selectable(name.c_str(), name == param)) {
                    param  = name;
                    commit = true;
                }
            ImGui::EndCombo();
        }
        break;
    }
    case ParamKind::ElementType:
        if (ImGui::BeginCombo(desc->paramLabel.c_str(), param.c_str())) {
            for (const std::string& t : ElementTypeNames())
                if (ImGui::Selectable(t.c_str(), t == param)) {
                    param  = t;
                    commit = true;
                }
            ImGui::EndCombo();
        }
        break;
    case ParamKind::TypeAndCount: { // "float:3"
        const std::size_t colon   = param.find(':');
        std::string       element = param.substr(0, colon);
        int count = colon == std::string::npos ? 2 : std::clamp(std::atoi(param.c_str() + colon + 1), 1, 16);
        if (ImGui::BeginCombo("Element", element.c_str())) {
            for (const std::string& t : ElementTypeNames())
                if (ImGui::Selectable(t.c_str(), t == element)) {
                    element = t;
                    commit  = true;
                }
            ImGui::EndCombo();
        }
        commit |= ImGui::SliderInt("Items", &count, 1, 16);
        param = element + ":" + std::to_string(count);
        break;
    }
    case ParamKind::PinType: {
        PinType type = PinTypeFromString(param).value_or(PinType::Float);
        if (TypeCombo(desc->paramLabel.c_str(), type, 140.0f)) {
            param  = ToString(type);
            commit = true;
        }
        break;
    }
    case ParamKind::StructType:
    case ParamKind::EnumType: {
        const bool structs = desc->paramKind == ParamKind::StructType;
        if (ImGui::BeginCombo(desc->paramLabel.c_str(), param.c_str())) {
            for (const std::string& name : structs ? ScriptRegistry::StructNames() : ScriptRegistry::EnumNames())
                if (ImGui::Selectable(name.c_str(), name == param)) {
                    param  = name;
                    commit = true;
                }
            ImGui::EndCombo();
        }
        break;
    }
    case ParamKind::EnumValue:
    case ParamKind::StructField: { // "<type>.<member>": two combos
        const bool        isEnum = desc->paramKind == ParamKind::EnumValue;
        const std::size_t dot    = param.find('.');
        std::string       type   = param.substr(0, dot), member = dot == std::string::npos ? "" : param.substr(dot + 1);
        if (ImGui::BeginCombo(isEnum ? "Enum" : "Struct", type.c_str())) {
            for (const std::string& name : isEnum ? ScriptRegistry::EnumNames() : ScriptRegistry::StructNames())
                if (ImGui::Selectable(name.c_str(), name == type)) {
                    type   = name;
                    member.clear();
                    if (isEnum) {
                        if (const ScriptEnum* e = ScriptRegistry::FindEnum(name); e && !e->values.empty())
                            member = e->values.front();
                    } else if (const ScriptStructDef* st = ScriptRegistry::FindStruct(name); st && !st->fields.empty()) {
                        member = st->fields.front().name;
                    }
                    commit = true;
                }
            ImGui::EndCombo();
        }
        if (ImGui::BeginCombo(isEnum ? "Value" : "Field", member.c_str())) {
            std::vector<std::string> members;
            if (isEnum) {
                if (const ScriptEnum* e = ScriptRegistry::FindEnum(type))
                    members = e->values;
            } else if (const ScriptStructDef* st = ScriptRegistry::FindStruct(type)) {
                for (const ScriptStructField& f : st->fields)
                    members.push_back(f.name);
            }
            for (const std::string& m : members)
                if (ImGui::Selectable(m.c_str(), m == member)) {
                    member = m;
                    commit = true;
                }
            ImGui::EndCombo();
        }
        param = type + "." + member;
        break;
    }
    case ParamKind::Cases:
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##cases", "comma separated, e.g. 0, 1, 2", &param);
        commit = ImGui::IsItemDeactivatedAfterEdit();
        break;
    case ParamKind::Key:
        if (ImGui::BeginCombo(desc->paramLabel.c_str(), param.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (const std::string& k : KeyNames())
                if (ImGui::Selectable(k.c_str(), k == param)) {
                    param  = k;
                    commit = true;
                }
            ImGui::EndCombo();
        }
        break;
    case ParamKind::Count: {
        int count = std::clamp(std::atoi(param.c_str()), 2, 16);
        if (ImGui::SliderInt(desc->paramLabel.c_str(), &count, 2, 16)) {
            param  = std::to_string(count);
            commit = true;
        }
        break;
    }
    case ParamKind::Choice:
        if (ImGui::BeginCombo(desc->paramLabel.c_str(), param.c_str())) {
            for (const std::string& c : desc->paramChoices)
                if (ImGui::Selectable(c.c_str(), c == param)) {
                    param  = c;
                    commit = true;
                }
            ImGui::EndCombo();
        }
        break;
    }
    if (commit && param != node->param) {
        const std::uint32_t id = node->id;
        Edit("Change parameter", [&](ScriptGraph& graph) {
            graph.FindNode(id)->param = param;
            graph.RemoveDanglingLinks();
        });
        return;
    }

    // Unconnected inputs (same values as inline on the canvas).
    const NodeLayout l = Layout(g, *node);
    bool             header = false;
    for (std::size_t i = 0; i < l.pins.size(); ++i) {
        const PinInfo& p = l.pins[i];
        if (p.output || p.type == PinType::Exec || p.type == PinType::Entity || l.connected[i])
            continue;
        if (!std::exchange(header, true))
            ImGui::SeparatorText("Inputs");
        auto it = node->defaults.find(p.name);
        if (it == node->defaults.end())
            it = node->defaults.emplace(p.name, PinDefault(*node, desc, p)).first;
        ImGui::PushID(p.name.c_str());
        m_WidgetTouched |= ValueWidget(p.name.c_str(), it->second, p.type, 140.0f);
        ImGui::PopID();
    }
}

// --- Canvas -------------------------------------------------------------------------------------

void ScriptGraphEditor::DrawCanvas(Document& doc, const ScriptSystem* debug)
{
    constexpr float kResultsHeight = 110.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(28, 28, 32, 255));
    const bool shown = ImGui::BeginChild("canvas", ImVec2(0.0f, -kResultsHeight), ImGuiChildFlags_Borders,
                                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                             ImGuiWindowFlags_NoMove);
    ImGui::PopStyleColor();
    if (!shown) {
        ImGui::EndChild();
        return;
    }
    ScriptGraph&  g      = doc.graph;
    ImGuiIO&      io     = ImGui::GetIO();
    const ImVec2  origin = ImGui::GetCursorScreenPos();
    const ImVec2  avail  = ImGui::GetContentRegionAvail();
    const glm::vec2 size(std::max(avail.x, 50.0f), std::max(avail.y, 50.0f));
    doc.viewSize = size;
    m_CanvasRect = {origin.x, origin.y, size.x, size.y};
    if (!doc.framed) {
        doc.framed = true;
        doc.scroll = glm::vec2(60.0f, 60.0f);
        if (!g.nodes.empty())
            FrameNodes(doc, false);
    }
    float& zoom = doc.zoom;
    const auto toScreen = [&](glm::vec2 p) {
        return ImVec2(origin.x + (p.x + doc.scroll.x) * zoom, origin.y + (p.y + doc.scroll.y) * zoom);
    };
    const auto toCanvas = [&](ImVec2 s) {
        return glm::vec2((s.x - origin.x) / zoom - doc.scroll.x, (s.y - origin.y) / zoom - doc.scroll.y);
    };

    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##canvas", ImVec2(size.x, size.y),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool      hovered = ImGui::IsItemHovered();
    const glm::vec2 mouse   = toCanvas(io.MousePos);
    ImDrawList*     draw    = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    // Grid.
    const float grid = 32.0f * zoom;
    if (grid > 6.0f) {
        const float ox = std::fmod(doc.scroll.x * zoom, grid), oy = std::fmod(doc.scroll.y * zoom, grid);
        for (float x = ox; x < size.x; x += grid)
            draw->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), IM_COL32(45, 45, 52, 255));
        for (float y = oy; y < size.y; y += grid)
            draw->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), IM_COL32(45, 45, 52, 255));
    }

    ImFont*     font     = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize() * zoom;
    const bool  text     = zoom > 0.35f;
    const double now     = debug ? debug->Time() : 0.0;
    const ScriptDebugInfo* info = debug && debug->Running() ? debug->Debug(doc.path) : nullptr;
    const auto glow = [&](double t) { return info ? std::clamp(1.0 - (now - t) / 0.5, 0.0, 1.0) : 0.0; };

    // Comments (behind everything).
    for (const ScriptComment& c : g.comments) {
        if (c.function != doc.scope)
            continue;
        const ImVec2 a = toScreen(c.position), b = toScreen(c.position + c.size);
        const ImU32  fill = ImGui::GetColorU32(ImVec4(c.color.r, c.color.g, c.color.b, 0.18f));
        const ImU32  head = ImGui::GetColorU32(ImVec4(c.color.r, c.color.g, c.color.b, 0.7f));
        draw->AddRectFilled(a, b, fill, 4.0f * zoom);
        draw->AddRectFilled(a, ImVec2(b.x, a.y + kHeader * zoom), head, 4.0f * zoom, ImDrawFlags_RoundCornersTop);
        draw->AddRect(a, b, doc.selectedComment == c.id ? IM_COL32(255, 200, 60, 255) : head, 4.0f * zoom,
                      doc.selectedComment == c.id ? 2.0f : 1.0f);
        draw->AddTriangleFilled(ImVec2(b.x - 12.0f * zoom, b.y), b, ImVec2(b.x, b.y - 12.0f * zoom), head);
        if (text)
            draw->AddText(font, fontSize * 1.1f, ImVec2(a.x + 6.0f * zoom, a.y + 3.0f * zoom), IM_COL32(255, 255, 255, 230),
                          c.text.c_str());
    }

    // Layouts (nodes are drawn in order: later = on top).
    std::vector<NodeLayout>             layouts;
    std::map<std::uint32_t, std::size_t> layoutOf;
    layouts.reserve(g.nodes.size());
    for (const ScriptNode& n : g.nodes) {
        if (n.function != doc.scope)
            continue;
        layoutOf[n.id] = layouts.size();
        layouts.push_back(Layout(g, n));
    }

    // Links.
    const auto bezier = [&](ImVec2 p0, ImVec2 p3, ImU32 color, float thickness) {
        const float dx = std::max(std::abs(p3.x - p0.x) * 0.5f, 40.0f * zoom);
        draw->AddBezierCubic(p0, ImVec2(p0.x + dx, p0.y), ImVec2(p3.x - dx, p3.y), p3, color, thickness);
    };
    for (const ScriptLink& link : g.links) {
        const auto a = layoutOf.find(link.fromNode), b = layoutOf.find(link.toNode);
        if (a == layoutOf.end() || b == layoutOf.end())
            continue;
        const NodeLayout& la = layouts[a->second];
        const NodeLayout& lb = layouts[b->second];
        const int         pa = la.PinIndex(link.fromPin, true), pb = lb.PinIndex(link.toPin, false);
        if (pa < 0 || pb < 0)
            continue;
        const PinType type = la.pins[static_cast<std::size_t>(pa)].type;
        double        lit  = 0.0;
        if (info && type == PinType::Exec)
            if (const auto t = info->linkTimes.find({link.fromNode, link.fromPin}); t != info->linkTimes.end())
                lit = glow(t->second);
        const float thickness = (type == PinType::Exec ? 3.0f : 2.0f) * std::max(zoom, 0.5f) * (1.0f + static_cast<float>(lit));
        const ImU32 color     = lit > 0.0 ? ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.2f, 1.0f)) : PinColor(type, 0.85f);
        bezier(toScreen(la.pinPos[static_cast<std::size_t>(pa)]), toScreen(lb.pinPos[static_cast<std::size_t>(pb)]), color, thickness);
    }

    // Hover: topmost pin / node / comment under the mouse.
    struct PinHit {
        std::uint32_t node = 0;
        int           pin  = -1;
    } pinHit;
    std::uint32_t nodeHit = 0;
    for (auto it = layouts.rbegin(); it != layouts.rend() && hovered; ++it) {
        for (std::size_t p = 0; p < it->pins.size(); ++p)
            if (glm::distance(mouse, it->pinPos[p]) <= kPinHit) {
                pinHit = {it->node->id, static_cast<int>(p)};
                break;
            }
        if (pinHit.pin >= 0)
            break;
        if (it->Contains(mouse)) {
            nodeHit = it->node->id;
            break;
        }
    }
    std::uint32_t commentHeader = 0, commentResize = 0;
    if (hovered && pinHit.pin < 0 && !nodeHit)
        for (auto it = g.comments.rbegin(); it != g.comments.rend(); ++it) {
            if (it->function != doc.scope)
                continue;
            const glm::vec2 corner = it->position + it->size;
            if (mouse.x >= corner.x - 14.0f && mouse.y >= corner.y - 14.0f && mouse.x <= corner.x && mouse.y <= corner.y) {
                commentResize = it->id;
                break;
            }
            if (mouse.x >= it->position.x && mouse.x <= corner.x && mouse.y >= it->position.y &&
                mouse.y <= it->position.y + kHeader) {
                commentHeader = it->id;
                break;
            }
        }

    // Nodes.
    const std::set<std::uint32_t> selected(doc.selected.begin(), doc.selected.end());
    std::set<std::uint32_t>       errorNodes;
    for (const ScriptDiagnostic& d : Diagnostics())
        if (d.error && d.node)
            errorNodes.insert(d.node);
    if (info)
        for (const ScriptDiagnostic& d : info->diagnostics)
            if (d.error && d.node)
                errorNodes.insert(d.node);
    const float rounding = 6.0f * zoom;
    std::uint32_t pausedHere = 0;
    if (debug && debug->Running())
        if (const auto at = debug->PausedAt(); at && at->file == ScriptSystem::Key(doc.path))
            pausedHere = at->node;
    for (NodeLayout& l : layouts) {
        const ScriptNode& n = *l.node;
        const ImVec2      a = toScreen(l.pos), b = toScreen(l.pos + l.size);
        if (b.x < origin.x || b.y < origin.y || a.x > origin.x + size.x || a.y > origin.y + size.y)
            continue; // off screen
        double lit = 0.0;
        if (info)
            if (const auto t = info->nodeTimes.find(n.id); t != info->nodeTimes.end())
                lit = glow(t->second);
        if (pausedHere == n.id) // the debugger stopped before this node
            draw->AddRect(ImVec2(a.x - 5.0f, a.y - 5.0f), ImVec2(b.x + 5.0f, b.y + 5.0f), IM_COL32(255, 220, 40, 255),
                          rounding + 4.0f, 5.0f);
        else if (lit > 0.0)
            draw->AddRect(ImVec2(a.x - 4.0f, a.y - 4.0f), ImVec2(b.x + 4.0f, b.y + 4.0f),
                          ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.2f, static_cast<float>(lit))), rounding + 3.0f, 4.0f);
        if (IsReroute(n)) { // knot
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            const PinType type = l.pins.empty() ? PinType::Exec : l.pins.front().type;
            draw->AddLine(toScreen(l.pinPos.front()), toScreen(l.pinPos.back()), PinColor(type, 0.85f), 2.0f * std::max(zoom, 0.5f));
            draw->AddCircleFilled(c, 6.0f * zoom, PinColor(type));
            for (const glm::vec2& pinPos : l.pinPos) // grab points
                draw->AddCircle(toScreen(pinPos), 3.0f * zoom, PinColor(type, 0.7f));
            if (selected.contains(n.id))
                draw->AddCircle(c, 8.0f * zoom, IM_COL32(255, 200, 60, 255), 0, 2.0f);
            continue;
        }
        draw->AddRectFilled(a, b, IM_COL32(22, 22, 26, 235), rounding);
        draw->AddRectFilled(a, ImVec2(b.x, a.y + kHeader * zoom), HeaderColor(l.desc, g, n), rounding, ImDrawFlags_RoundCornersTop);
        const bool isSelected = selected.contains(n.id);
        const bool isError    = errorNodes.contains(n.id);
        draw->AddRect(a, b,
                      isError      ? IM_COL32(255, 60, 60, 255)
                      : isSelected ? IM_COL32(255, 200, 60, 255)
                                   : IM_COL32(70, 70, 80, 255),
                      rounding, isSelected || isError ? 2.5f : 1.0f);
        if (text)
            draw->AddText(font, fontSize, ImVec2(a.x + kPad * zoom, a.y + 4.0f * zoom), IM_COL32(255, 255, 255, 255),
                          l.title.c_str());
        if (g.HasBreakpoint(n.id)) // red dot on the header's left edge
            draw->AddCircleFilled(ImVec2(a.x, a.y), 6.0f * std::max(zoom, 0.6f), IM_COL32(230, 40, 40, 255));

        for (std::size_t p = 0; p < l.pins.size(); ++p) {
            const PinInfo& pin   = l.pins[p];
            const ImVec2   c     = toScreen(l.pinPos[p]);
            const ImU32    color = PinColor(pin.type);
            const float    r     = kPinRadius * zoom;
            const bool     hot   = pinHit.node == n.id && pinHit.pin == static_cast<int>(p);
            if (pin.type == PinType::Exec) {
                const ImVec2 t0(c.x - r, c.y - r), t1(c.x - r, c.y + r), t2(c.x + r, c.y);
                if (l.connected[p] || hot)
                    draw->AddTriangleFilled(t0, t1, t2, color);
                else
                    draw->AddTriangle(t0, t1, t2, color, 1.5f);
            } else if (IsArray(pin.type)) { // arrays: a square
                const ImVec2 lo(c.x - r, c.y - r), hi(c.x + r, c.y + r);
                if (l.connected[p] || hot)
                    draw->AddRectFilled(lo, hi, color);
                else
                    draw->AddRect(lo, hi, color, 0.0f, 1.5f);
            } else {
                if (l.connected[p] || hot)
                    draw->AddCircleFilled(c, r, color);
                else
                    draw->AddCircle(c, r, color, 0, 1.5f);
            }
            if (!text || l.labelWidth[p] <= 0.0f)
                continue;
            const float ty = c.y - fontSize * 0.5f;
            if (pin.output)
                draw->AddText(font, fontSize, ImVec2(c.x - (kPad + 6.0f + l.labelWidth[p]) * zoom, ty),
                              IM_COL32(220, 220, 220, 255), pin.name.c_str());
            else
                draw->AddText(font, fontSize, ImVec2(c.x + (kPad + 6.0f) * zoom, ty), IM_COL32(220, 220, 220, 255),
                              pin.name.c_str());
        }

        // Inline values of unconnected data inputs.
        if (zoom < 0.5f || IsReroute(n))
            continue;
        ScriptNode& editable = *g.FindNode(n.id);
        ImGui::PushID(static_cast<int>(n.id));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * zoom);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f * zoom, 1.0f * zoom));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(2.0f * zoom, 2.0f * zoom));
        for (std::size_t p = 0; p < l.pins.size(); ++p) {
            const PinInfo& pin = l.pins[p];
            if (pin.output || pin.type == PinType::Exec || l.connected[p])
                continue;
            const ImVec2 c = toScreen(l.pinPos[p]);
            ImGui::SetCursorScreenPos(ImVec2(c.x + (kPad + 10.0f + l.labelWidth[p]) * zoom, c.y - ImGui::GetFrameHeight() * 0.5f));
            auto it = editable.defaults.find(pin.name);
            if (it == editable.defaults.end()) {
                ScriptValue value = PinDefault(editable, l.desc, pin);
                ImGui::PushID(pin.name.c_str());
                if (ValueWidget("##v", value, pin.type, WidgetWidth(pin.type) * zoom)) {
                    editable.defaults[pin.name] = std::move(value);
                    m_WidgetTouched             = true;
                }
                ImGui::PopID();
            } else {
                ImGui::PushID(pin.name.c_str());
                m_WidgetTouched |= ValueWidget("##v", it->second, pin.type, WidgetWidth(pin.type) * zoom);
                ImGui::PopID();
            }
        }
        ImGui::PopStyleVar(2);
        ImGui::PopFont();
        ImGui::PopID();
    }

    // --- Interaction ---
    DragState&    drag         = *m_Drag;
    std::uint32_t bringToFront = 0; // clicked node: drawn last from the next frame on
    if (hovered && io.MouseWheel != 0.0f && drag.kind == DragState::Kind::None) {
        const glm::vec2 before = mouse;
        zoom                   = std::clamp(zoom * std::pow(1.15f, io.MouseWheel), kMinZoom, kMaxZoom);
        doc.scroll = glm::vec2((io.MousePos.x - origin.x) / zoom, (io.MousePos.y - origin.y) / zoom) - before;
    }
    // Minimap (bottom right): the scope's nodes and the view; click / drag moves the view.
    bool inMinimap = false;
    if (doc.showMinimap && !layouts.empty() && size.x > 300.0f && size.y > 200.0f) {
        const glm::vec2 mapSize(180.0f, 120.0f);
        const glm::vec2 mapMin(origin.x + size.x - mapSize.x - 8.0f, origin.y + size.y - mapSize.y - 8.0f);
        const glm::vec2 viewLo = -doc.scroll, viewHi = -doc.scroll + size / zoom;
        glm::vec2       lo = viewLo, hi = viewHi;
        for (const NodeLayout& l : layouts) {
            lo = glm::min(lo, l.pos);
            hi = glm::max(hi, l.pos + l.size);
        }
        const float     scale  = std::min(mapSize.x / (hi.x - lo.x), mapSize.y / (hi.y - lo.y));
        const glm::vec2 offset = mapMin + (mapSize - (hi - lo) * scale) * 0.5f;
        const auto      toMap  = [&](glm::vec2 p) { const glm::vec2 m = offset + (p - lo) * scale; return ImVec2(m.x, m.y); };
        draw->AddRectFilled(ImVec2(mapMin.x, mapMin.y), ImVec2(mapMin.x + mapSize.x, mapMin.y + mapSize.y), IM_COL32(15, 15, 18, 220), 4.0f);
        for (const NodeLayout& l : layouts)
            draw->AddRectFilled(toMap(l.pos), toMap(l.pos + l.size),
                                selected.contains(l.node->id) ? IM_COL32(255, 200, 60, 255) : HeaderColor(l.desc, g, *l.node));
        draw->AddRect(toMap(viewLo), toMap(viewHi), IM_COL32(255, 255, 255, 200), 0.0f, 1.5f);
        draw->AddRect(ImVec2(mapMin.x, mapMin.y), ImVec2(mapMin.x + mapSize.x, mapMin.y + mapSize.y), IM_COL32(80, 80, 90, 255), 4.0f);
        const glm::vec2 m(io.MousePos.x, io.MousePos.y);
        inMinimap = m.x >= mapMin.x && m.y >= mapMin.y && m.x <= mapMin.x + mapSize.x && m.y <= mapMin.y + mapSize.y;
        if (hovered && inMinimap && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_Drag->kind == DragState::Kind::None)
            m_Drag->minimapDrag = 1;
        if (m_Drag->minimapDrag) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                doc.scroll = size / (2.0f * zoom) - (lo + (m - offset) / scale);
            else
                m_Drag->minimapDrag = 0;
            inMinimap = true;
        }
    }

    const auto beginDrag = [&](DragState::Kind kind, int button) {
        drag.kind           = kind;
        drag.button         = button;
        drag.start          = mouse;
        drag.last           = mouse;
        drag.before         = m_FrameJson;
        drag.beforeRevision = doc.revision;
        drag.moved          = false;
    };
    // Double click on a link: a reroute knot there.
    const ScriptLink* linkHit = nullptr;
    if (hovered && !inMinimap && drag.kind == DragState::Kind::None && pinHit.pin < 0 && !nodeHit) {
        float best = 8.0f / zoom;
        for (const ScriptLink& link : g.links) {
            const auto a = layoutOf.find(link.fromNode), b = layoutOf.find(link.toNode);
            if (a == layoutOf.end() || b == layoutOf.end())
                continue;
            const int pa = layouts[a->second].PinIndex(link.fromPin, true), pb = layouts[b->second].PinIndex(link.toPin, false);
            if (pa < 0 || pb < 0)
                continue;
            const float d = BezierDistance(mouse, layouts[a->second].pinPos[static_cast<std::size_t>(pa)],
                                           layouts[b->second].pinPos[static_cast<std::size_t>(pb)]);
            if (d < best) {
                best    = d;
                linkHit = &link;
            }
        }
    }
    if (linkHit && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const ScriptLink link  = *linkHit;
        const auto       from  = FindPin(g, *g.FindNode(link.fromNode), link.fromPin, true);
        const PinType    type  = from ? from->type : PinType::Float;
        const std::string scope = doc.scope;
        std::uint32_t     knot  = 0;
        Edit("Reroute", [&](ScriptGraph& graph) {
            const bool exec = type == PinType::Exec;
            knot = graph.AddNode(exec ? "Utility.RerouteExec" : "Utility.Reroute", mouse - glm::vec2(16.0f, 10.0f),
                                 exec ? std::string() : std::string(ToString(type)), scope);
            std::erase(graph.links, link);
            (void)graph.Connect(link.fromNode, link.fromPin, knot, "In");
            (void)graph.Connect(knot, "Out", link.toNode, link.toPin);
        });
        doc.selected = {knot};
    } else if (hovered && !inMinimap && drag.kind == DragState::Kind::None) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (pinHit.pin >= 0) {
                const NodeLayout& l   = layouts[layoutOf[pinHit.node]];
                const PinInfo&    pin = l.pins[static_cast<std::size_t>(pinHit.pin)];
                if (io.KeyAlt) {
                    const std::uint32_t id = pinHit.node;
                    Edit("Break links", [&](ScriptGraph& graph) { graph.Disconnect(id, pin.name, pin.output); });
                } else {
                    beginDrag(DragState::Kind::Link, 0);
                    drag.node   = pinHit.node;
                    drag.pin    = pin.name;
                    drag.output = pin.output;
                    drag.type   = pin.type;
                }
            } else if (nodeHit) {
                const bool isSelected = selected.contains(nodeHit);
                if (io.KeyCtrl || io.KeyShift) {
                    if (isSelected)
                        std::erase(doc.selected, nodeHit);
                    else
                        doc.selected.push_back(nodeHit);
                } else if (!isSelected) {
                    doc.selected = {nodeHit};
                }
                doc.selectedComment = 0;
                bringToFront        = nodeHit;
                beginDrag(DragState::Kind::Nodes, 0);
            } else if (commentResize) {
                doc.selectedComment = commentResize;
                doc.selected.clear();
                beginDrag(DragState::Kind::Resize, 0);
                drag.comment = commentResize;
            } else if (commentHeader) {
                doc.selectedComment = commentHeader;
                doc.selected.clear();
                beginDrag(DragState::Kind::Comment, 0);
                drag.comment = commentHeader;
                drag.carried.clear();
                if (const ScriptComment* c = g.FindComment(commentHeader))
                    for (const NodeLayout& l : layouts)
                        if (l.pos.x >= c->position.x && l.pos.y >= c->position.y &&
                            l.pos.x + l.size.x <= c->position.x + c->size.x && l.pos.y + l.size.y <= c->position.y + c->size.y)
                            drag.carried.push_back(l.node->id);
            } else {
                if (!io.KeyCtrl && !io.KeyShift)
                    doc.selected.clear();
                doc.selectedComment = 0;
                beginDrag(DragState::Kind::Box, 0);
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            beginDrag(DragState::Kind::Pan, 1);
            drag.menuNode = nodeHit;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            beginDrag(DragState::Kind::Pan, 2);
        }
    }

    switch (drag.kind) {
    case DragState::Kind::None: break;
    case DragState::Kind::Pan:
        if (ImGui::IsMouseDown(drag.button)) {
            doc.scroll += glm::vec2(io.MouseDelta.x, io.MouseDelta.y) / zoom;
            if (const ImVec2 d = ImGui::GetMouseDragDelta(drag.button); d.x * d.x + d.y * d.y > 16.0f)
                drag.moved = true;
        } else {
            if (drag.button == 1 && !drag.moved && hovered) { // right click: menus
                drag.menuPos = mouse;
                if (drag.menuNode) {
                    if (!selected.contains(drag.menuNode))
                        doc.selected = {drag.menuNode};
                    drag.openNode = true;
                } else {
                    drag.pending.reset();
                    drag.openCreate = true;
                }
            }
            drag.kind = DragState::Kind::None;
        }
        break;
    case DragState::Kind::Nodes:
    case DragState::Kind::Comment:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            glm::vec2 delta = mouse - drag.last;
            drag.last       = mouse;
            if (delta != glm::vec2(0.0f)) {
                drag.moved = true;
                const std::vector<std::uint32_t>& ids = drag.kind == DragState::Kind::Nodes ? doc.selected : drag.carried;
                for (std::uint32_t id : ids)
                    if (ScriptNode* n = g.FindNode(id))
                        n->position += delta;
                if (drag.kind == DragState::Kind::Comment)
                    if (ScriptComment* c = g.FindComment(drag.comment))
                        c->position += delta;
            }
        } else {
            if (drag.moved) {
                // Snap to a 8-unit grid on release.
                const auto snap = [](glm::vec2 p) { return glm::round(p / 8.0f) * 8.0f; };
                for (std::uint32_t id : drag.kind == DragState::Kind::Nodes ? doc.selected : drag.carried)
                    if (ScriptNode* n = g.FindNode(id))
                        n->position = snap(n->position);
                PushUndo(doc, std::move(drag.before), drag.beforeRevision);
                Changed(doc);
            }
            drag.kind = DragState::Kind::None;
        }
        break;
    case DragState::Kind::Resize:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (ScriptComment* c = g.FindComment(drag.comment)) {
                c->size    = glm::max(mouse - c->position, glm::vec2(120.0f, 60.0f));
                drag.moved = true;
            }
        } else {
            if (drag.moved) {
                PushUndo(doc, std::move(drag.before), drag.beforeRevision);
                Changed(doc);
            }
            drag.kind = DragState::Kind::None;
        }
        break;
    case DragState::Kind::Link: {
        const auto from = layoutOf.find(drag.node);
        if (from == layoutOf.end()) {
            drag.kind = DragState::Kind::None;
            break;
        }
        const NodeLayout& l   = layouts[from->second];
        const int         pin = l.PinIndex(drag.pin, drag.output);
        if (pin < 0) {
            drag.kind = DragState::Kind::None;
            break;
        }
        const ImVec2 p = toScreen(l.pinPos[static_cast<std::size_t>(pin)]);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Compatible target under the mouse: highlight.
            ImU32 color = PinColor(drag.type, 0.9f);
            if (pinHit.pin >= 0 && pinHit.node != drag.node) {
                const PinInfo& target = layouts[layoutOf[pinHit.node]].pins[static_cast<std::size_t>(pinHit.pin)];
                const bool ok = target.output != drag.output &&
                                (drag.output ? CanConvert(drag.type, target.type) : CanConvert(target.type, drag.type));
                color = ok ? IM_COL32(120, 255, 120, 255) : IM_COL32(255, 80, 80, 255);
            }
            if (drag.output)
                bezier(p, io.MousePos, color, 2.5f * std::max(zoom, 0.5f));
            else
                bezier(io.MousePos, p, color, 2.5f * std::max(zoom, 0.5f));
        } else {
            if (pinHit.pin >= 0 && pinHit.node != drag.node) {
                const PinInfo&      target   = layouts[layoutOf[pinHit.node]].pins[static_cast<std::size_t>(pinHit.pin)];
                const std::uint32_t other    = pinHit.node;
                const std::string   otherPin = target.name;
                if (target.output != drag.output)
                    Edit("Connect", [&](ScriptGraph& graph) {
                        const std::string error = drag.output ? graph.Connect(drag.node, drag.pin, other, otherPin)
                                                              : graph.Connect(other, otherPin, drag.node, drag.pin);
                        if (!error.empty())
                            m_LastError = error;
                    });
            } else if (pinHit.pin < 0 && !nodeHit && hovered) {
                // Dropped on the background: pick a node to connect.
                drag.pending    = DragState::Pending{drag.node, drag.pin, drag.output, drag.type};
                drag.menuPos    = mouse;
                drag.openCreate = true;
            }
            drag.kind = DragState::Kind::None;
        }
        break;
    }
    case DragState::Kind::Box: {
        const glm::vec2 lo = glm::min(drag.start, mouse), hi = glm::max(drag.start, mouse);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            draw->AddRectFilled(toScreen(lo), toScreen(hi), IM_COL32(90, 150, 255, 40));
            draw->AddRect(toScreen(lo), toScreen(hi), IM_COL32(90, 150, 255, 200));
        } else {
            for (const NodeLayout& l : layouts)
                if (l.pos.x <= hi.x && l.pos.y <= hi.y && l.pos.x + l.size.x >= lo.x && l.pos.y + l.size.y >= lo.y &&
                    std::ranges::find(doc.selected, l.node->id) == doc.selected.end())
                    doc.selected.push_back(l.node->id);
            drag.kind = DragState::Kind::None;
        }
        break;
    }
    }

    // Tooltip: node description / pin type.
    if (hovered && drag.kind == DragState::Kind::None) {
        if (pinHit.pin >= 0) {
            const PinInfo& pin   = layouts[layoutOf[pinHit.node]].pins[static_cast<std::size_t>(pinHit.pin)];
            std::string    value;
            if (m_Watch && pin.type != PinType::Exec) { // debugger: the last value that went through
                std::pair<std::uint32_t, std::string> key{pinHit.node, pin.name};
                if (!pin.output)
                    for (const ScriptLink& link : g.links)
                        if (link.toNode == pinHit.node && link.toPin == pin.name)
                            key = {link.fromNode, link.fromPin};
                if (const auto it = m_Watch->pins.find(key); it != m_Watch->pins.end())
                    value = "\nValue: " + ToDisplayString(it->second);
                else if (pin.output || key.first != pinHit.node)
                    value = "\nValue: (not evaluated yet)";
            }
            ImGui::SetTooltip("%s (%s)%s\nAlt+click: break links", pin.name.c_str(), DisplayName(pin.type).c_str(), value.c_str());
        } else if (linkHit) {
            ImGui::SetTooltip("Double-click: add a reroute knot");
        } else if (nodeHit) {
            const NodeLayout& l = layouts[layoutOf[nodeHit]];
            for (const ScriptDiagnostic& d : Diagnostics())
                if (d.node == nodeHit)
                    ImGui::SetTooltip("%s", d.message.c_str());
            if (!errorNodes.contains(nodeHit) && l.desc && !l.desc->tooltip.empty() && io.KeyCtrl)
                ImGui::SetTooltip("%s", l.desc->tooltip.c_str());
        }
    }

    draw->PopClipRect();
    if (bringToFront) { // after the layouts (they point into g.nodes) are no longer used
        const auto it = std::ranges::find_if(g.nodes, [&](const ScriptNode& n) { return n.id == bringToFront; });
        if (it != g.nodes.end() && it + 1 != g.nodes.end())
            std::rotate(it, it + 1, g.nodes.end());
    }
    DrawCreateMenu(doc);
    ImGui::EndChild();
}

void ScriptGraphEditor::DrawCreateMenu(Document& doc)
{
    DragState& drag = *m_Drag;
    if (std::exchange(drag.openCreate, false)) {
        drag.search.clear();
        drag.focusSearch = true;
        ImGui::OpenPopup("##create");
    }
    if (std::exchange(drag.openNode, false))
        ImGui::OpenPopup("##node");

    if (ImGui::BeginPopup("##node")) {
        if (ImGui::MenuItem("Delete", "Del"))
            DeleteSelection();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
            DuplicateSelection();
        if (ImGui::MenuItem("Copy", "Ctrl+C"))
            ImGui::SetClipboardText(CopySelection().c_str());
        if (ImGui::MenuItem("Break all links")) {
            const std::vector<std::uint32_t> nodes = doc.selected;
            Edit("Break links", [&](ScriptGraph& g) {
                std::erase_if(g.links, [&](const ScriptLink& l) {
                    return std::ranges::find(nodes, l.fromNode) != nodes.end() || std::ranges::find(nodes, l.toNode) != nodes.end();
                });
            });
        }
        if (ImGui::MenuItem("Comment selection", "C"))
            CommentSelection();
        if (ImGui::MenuItem("Toggle breakpoint", "F9"))
            ToggleBreakpoints();
        if (ImGui::MenuItem("Collapse to function"))
            m_LastError = CollapseSelection(false);
        if (ImGui::MenuItem("Collapse to macro"))
            m_LastError = CollapseSelection(true);
        if (doc.selected.size() == 1)
            if (const ScriptNode* n = doc.graph.FindNode(doc.selected.front()); n && !n->param.empty() &&
                ImGui::MenuItem(("Find references to '" + n->param + "'").c_str()))
                OpenSearch(n->param, true);
        if (ImGui::BeginMenu("Align", doc.selected.size() >= 2)) {
            if (ImGui::MenuItem("Left", "Shift+A"))
                AlignSelection(Align::Left);
            if (ImGui::MenuItem("Right", "Shift+D"))
                AlignSelection(Align::Right);
            if (ImGui::MenuItem("Top", "Shift+W"))
                AlignSelection(Align::Top);
            if (ImGui::MenuItem("Bottom", "Shift+S"))
                AlignSelection(Align::Bottom);
            if (ImGui::MenuItem("Center horizontally"))
                AlignSelection(Align::CenterX);
            if (ImGui::MenuItem("Center vertically"))
                AlignSelection(Align::CenterY);
            ImGui::Separator();
            if (ImGui::MenuItem("Distribute horizontally"))
                AlignSelection(Align::DistributeX);
            if (ImGui::MenuItem("Distribute vertically"))
                AlignSelection(Align::DistributeY);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f, 0.0f), ImVec2(360.0f, 420.0f));
    if (!ImGui::BeginPopup("##create"))
        return;
    if (std::exchange(drag.focusSearch, false))
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search nodes...", &drag.search);
    const std::string filter = Lower(drag.search);
    ScriptGraph&      g      = doc.graph;

    struct Entry {
        std::string label, category, type, param;
    };
    std::vector<Entry>    entries;
    const ScriptFunction* scopeFunction = g.FindFunction(doc.scope);
    const bool            inMacro       = g.FindMacro(doc.scope) != nullptr;
    for (const NodeDesc& d : ScriptNodeTypes()) {
        if (d.hidden)
            continue; // Entry / Return / calls are offered per function below
        if (scopeFunction && (d.kind == NodeKind::Event || d.latent))
            continue; // functions run synchronously, without events
        if (inMacro && d.kind == NodeKind::Event)
            continue;
        if (d.paramKind == ParamKind::Variable) { // one entry per variable (locals of the scope first)
            const auto offer = [&](const ScriptVariable& v) {
                if (!d.arrayVariable || IsArray(v.type))
                    entries.push_back({d.title + " " + v.name, d.arrayVariable ? d.category : std::string("Variables"), d.type, v.name});
            };
            if (scopeFunction)
                for (const ScriptVariable& v : scopeFunction->locals)
                    offer(v);
            for (const ScriptVariable& v : g.variables)
                if (!scopeFunction || std::ranges::none_of(scopeFunction->locals, [&](const ScriptVariable& l) { return l.name == v.name; }))
                    offer(v);
            continue;
        }
        entries.push_back({d.title, d.category, d.type, {}});
    }
    for (const ScriptFunction& f : g.functions)
        if (f.name != doc.scope) // no recursion
            entries.push_back({"Call " + f.name, "Functions", f.pure ? "Function.CallPure" : "Function.Call", f.name});
    for (const ScriptMacro& m : g.macros)
        if (m.name != doc.scope)
            entries.push_back({"Macro " + m.name, "Macros", "Macro.Use", m.name});
    for (const std::string& lib : ScriptRegistry::LibraryNames()) {
        const ScriptLibrary* l = ScriptRegistry::FindLibrary(lib);
        for (const ScriptFunction& f : l->graph.functions)
            entries.push_back({lib + "." + f.name, "Libraries|" + lib, f.pure ? "Library.CallPure" : "Library.Call", lib + "." + f.name});
        for (const ScriptMacro& m : l->graph.macros)
            entries.push_back({lib + "." + m.name + " (macro)", "Libraries|" + lib, "Macro.Use", lib + "." + m.name});
    }
    for (const std::string& i : ScriptRegistry::InterfaceNames())
        for (const ScriptInterfaceFunction& f : ScriptRegistry::FindInterface(i)->functions)
            entries.push_back({f.name + " (Message: " + i + ")", "Interfaces", "Interface.Call", i + "." + f.name});
    for (const ScriptEventDecl& d : g.dispatchers) {
        entries.push_back({"Call " + d.name, "Dispatchers", "Dispatcher.Call", d.name});
        entries.push_back({"Bind to " + d.name, "Dispatchers", "Dispatcher.Bind", d.name});
        entries.push_back({"Unbind from " + d.name, "Dispatchers", "Dispatcher.Unbind", d.name});
    }
    for (const ScriptEventDecl& e : g.events) {
        if (!scopeFunction && !inMacro)
            entries.push_back({"Event " + e.name, "Events|Custom", "Event.Custom", e.name});
        entries.push_back({"Call " + e.name, "Events|Custom", "Flow.CallEvent", e.name});
    }
    if (!scopeFunction)
        for (const ScriptTimeline& t : g.timelines)
            entries.push_back({"Timeline " + t.name, "Timeline", "Timeline.Play", t.name});
    // Context: only nodes with a pin that fits the dragged one.
    const auto fits = [&](const Entry& e) -> std::optional<std::string> {
        if (!drag.pending)
            return std::string();
        ScriptNode probe;
        probe.type  = e.type;
        probe.param = e.param.empty() ? (FindScriptNodeType(e.type) ? FindScriptNodeType(e.type)->paramDefault : "") : e.param;
        for (const PinInfo& p : NodePins(g, probe))
            if (p.output != drag.pending->output &&
                (drag.pending->output ? CanConvert(drag.pending->type, p.type) : CanConvert(p.type, drag.pending->type)))
                return p.name;
        return std::nullopt;
    };

    std::optional<Entry> chosen;
    std::string          chosenPin;
    const auto item = [&](const Entry& e) {
        const auto pin = fits(e);
        if (!pin)
            return;
        if (ImGui::Selectable(e.label.c_str())) {
            chosen    = e;
            chosenPin = *pin;
        }
    };
    ImGui::BeginChild("##list", ImVec2(0.0f, 300.0f));
    if (filter.empty()) {
        std::vector<std::string> categories;
        for (const Entry& e : entries)
            if (std::ranges::find(categories, e.category) == categories.end() && fits(e))
                categories.push_back(e.category);
        for (const std::string& category : categories)
            if (ImGui::TreeNodeEx(category.c_str(), drag.pending ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                for (const Entry& e : entries)
                    if (e.category == category)
                        item(e);
                ImGui::TreePop();
            }
        if (!drag.pending && ImGui::Selectable("Add comment"))
            chosen = Entry{"comment", "", "", ""};
    } else {
        for (const Entry& e : entries)
            if (Lower(e.label).find(filter) != std::string::npos || Lower(e.category).find(filter) != std::string::npos)
                item(e);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter)) // first match
            for (const Entry& e : entries)
                if ((Lower(e.label).find(filter) != std::string::npos) && fits(e)) {
                    chosen    = e;
                    chosenPin = *fits(e);
                    break;
                }
    }
    ImGui::EndChild();

    if (chosen) {
        const glm::vec2 at = drag.menuPos;
        const std::string scope = doc.scope;
        if (chosen->label == "comment" && chosen->type.empty()) {
            Edit("Comment", [&](ScriptGraph& graph) { doc.selectedComment = graph.AddComment(at, {300.0f, 160.0f}, "Comment", scope); });
        } else {
            std::uint32_t id = 0;
            const auto    pending = drag.pending;
            Edit("Add node", [&](ScriptGraph& graph) {
                id = graph.AddNode(chosen->type, at, chosen->param, scope);
                if (pending) {
                    if (pending->output)
                        (void)graph.Connect(pending->node, pending->pin, id, chosenPin);
                    else
                        (void)graph.Connect(id, chosenPin, pending->node, pending->pin);
                }
            });
            doc.selected = {id};
        }
        drag.pending.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ScriptGraphEditor::DrawResults(Document& doc, const ScriptSystem* debug)
{
    ImGui::BeginChild("results", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    std::vector<ScriptDiagnostic> all = Diagnostics();
    if (debug && debug->Running())
        if (const ScriptDebugInfo* info = debug->Debug(doc.path))
            for (const ScriptDiagnostic& d : info->diagnostics)
                if (std::ranges::none_of(all, [&](const ScriptDiagnostic& x) { return x.node == d.node && x.message == d.message; }))
                    all.push_back(d);
    if (all.empty())
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Compiled: no errors");
    if (!m_LastError.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1.0f), "%s", m_LastError.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            m_LastError.clear();
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        const ScriptDiagnostic& d = all[i];
        ImGui::PushID(static_cast<int>(i));
        const ImVec4 color = d.error ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(1.0f, 0.85f, 0.4f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        const std::string label = std::format("{} {}{}", d.error ? "Error:" : "Warning:", d.node ? std::format("[node {}] ", d.node) : "",
                                              d.message);
        if (ImGui::Selectable(label.c_str()) && d.node && doc.graph.FindNode(d.node)) {
            doc.selected = {d.node};
            FrameNodes(doc, true);
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void ScriptGraphEditor::HandleKeys(Document& doc, ScriptSystem* debug, Scene* scene)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive() || m_Dialog->IsOpen() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false))
        ToggleBreakpoints();
    if (debug && scene && debug->DebugPaused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
            debug->DebugContinue(*scene);
        if (ImGui::IsKeyPressed(ImGuiKey_F10, false))
            debug->DebugStepOver(*scene);
        if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
            if (io.KeyShift)
                debug->DebugStepOut(*scene);
            else
                debug->DebugStep(*scene);
        }
    }
    if (io.KeyShift && !io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false))
            AlignSelection(Align::Top);
        if (ImGui::IsKeyPressed(ImGuiKey_S, false))
            AlignSelection(Align::Bottom);
        if (ImGui::IsKeyPressed(ImGuiKey_A, false))
            AlignSelection(Align::Left);
        if (ImGui::IsKeyPressed(ImGuiKey_D, false))
            AlignSelection(Align::Right);
        return;
    }
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (io.KeyShift)
                Redo();
            else
                Undo();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
            Redo();
        if (ImGui::IsKeyPressed(ImGuiKey_C, false))
            ImGui::SetClipboardText(CopySelection().c_str());
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            const char* text = ImGui::GetClipboardText();
            Paste(text ? text : "", -doc.scroll + glm::vec2(80.0f, 80.0f));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_D, false))
            DuplicateSelection();
        if (ImGui::IsKeyPressed(ImGuiKey_S, false))
            Save();
        if (ImGui::IsKeyPressed(ImGuiKey_F, false))
            OpenSearch(m_SearchQuery);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        DeleteSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_C, false) && !doc.selected.empty()) // like UE: around the selection
        CommentSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_F, false))
        FrameNodes(doc, !doc.selected.empty());
    if (ImGui::IsKeyPressed(ImGuiKey_M, false))
        doc.showMinimap = !doc.showMinimap;
}

void ScriptGraphEditor::FrameNodes(Document& doc, bool selectionOnly)
{
    glm::vec2 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    for (const ScriptNode& n : doc.graph.nodes) {
        if (n.function != doc.scope || (selectionOnly && std::ranges::find(doc.selected, n.id) == doc.selected.end()))
            continue;
        const NodeLayout l = Layout(doc.graph, n);
        lo                 = glm::min(lo, l.pos);
        hi                 = glm::max(hi, l.pos + l.size);
    }
    if (!selectionOnly) // comment titles stay in view
        for (const ScriptComment& c : doc.graph.comments)
            if (c.function == doc.scope) {
                lo = glm::min(lo, c.position);
                hi = glm::max(hi, c.position + c.size);
            }
    if (lo.x > hi.x)
        return;
    const glm::vec2 view   = doc.viewSize;
    const glm::vec2 extent = hi - lo + glm::vec2(80.0f);
    doc.zoom               = std::clamp(std::min(view.x / extent.x, view.y / extent.y), kMinZoom, 1.0f);
    doc.scroll             = view / (2.0f * doc.zoom) - (lo + hi) * 0.5f;
}


// --- Phase 20: macros, members, timelines, search, collapse ----------------------------------------

void ScriptGraphEditor::DrawMacro(Document& doc, ScriptMacro& m)
{
    ImGui::SeparatorText("Macro");
    std::string name = m.name;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##mname", &name);
    if (ImGui::IsItemDeactivatedAfterEdit() && name != m.name) {
        if (!IsValidScriptName(name) || doc.graph.HasScope(name)) {
            m_LastError = "Macro names must be unique (letters, digits, '_', ' ')";
        } else {
            const std::string from = m.name;
            Edit("Rename macro", [&](ScriptGraph& graph) { graph.RenameMacro(from, name); });
            doc.scope = name;
        }
        return;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    m_WidgetTouched |= ImGui::InputTextWithHint("##mdesc", "Description (tooltip)", &m.description);
    ImGui::TextDisabled("Pins: tick = exec. Latent nodes are fine here.");
    for (const bool outputs : {false, true}) {
        ImGui::SeparatorText(outputs ? "Outputs" : "Inputs");
        const auto edit = ParamListWidget(outputs ? m.outputs : m.inputs, true, outputs ? "+ Output" : "+ Input", outputs ? 2000 : 1000);
        if (!edit)
            continue;
        const std::string macro = m.name;
        Edit("Macro pins", [&](ScriptGraph& graph) {
            ScriptMacro* target                         = graph.FindMacro(macro);
            (outputs ? target->outputs : target->inputs) = edit->params;
            if (!edit->from.empty())
                RenamePins(graph, [&](const ScriptNode& n) {
                    if (n.type == "Macro.Use" && n.param == macro)
                        return outputs ? 1 : 2;
                    if (n.function == macro && n.type == (outputs ? "Macro.Outputs" : "Macro.Inputs"))
                        return outputs ? 2 : 1;
                    return 0;
                }, edit->from, edit->to);
            graph.RemoveDanglingLinks();
        });
        return;
    }
}

void ScriptGraphEditor::DrawMembers(Document& doc)
{
    ScriptGraph&    g      = doc.graph;
    const glm::vec2 center = -doc.scroll + glm::vec2(80.0f, 80.0f);
    const std::string scope = doc.scope;
    const auto addNode = [&](const char* type, const std::string& param) {
        Edit("Add node", [&](ScriptGraph& graph) { doc.selected = {graph.AddNode(type, center, param, scope)}; });
    };
    if (g.library)
        return; // functions and macros only

    // Custom events with parameters and event dispatchers: same editor.
    for (const bool dispatchers : {false, true}) {
        std::vector<ScriptEventDecl>& list = dispatchers ? g.dispatchers : g.events;
        if (!ImGui::CollapsingHeader(dispatchers ? "Event Dispatchers" : "Custom Events"))
            continue;
        std::optional<std::size_t> remove;
        for (std::size_t i = 0; i < list.size(); ++i) {
            ImGui::PushID(static_cast<int>(i) + (dispatchers ? 7000 : 6000));
            std::string name = list[i].name;
            ImGui::SetNextItemWidth(-130.0f);
            ImGui::InputText("##ename", &name);
            if (ImGui::IsItemDeactivatedAfterEdit() && name != list[i].name && IsValidScriptName(name) &&
                std::ranges::none_of(list, [&](const ScriptEventDecl& e) { return e.name == name; })) {
                const std::string from = list[i].name;
                Edit(dispatchers ? "Rename dispatcher" : "Rename event", [&](ScriptGraph& graph) {
                    for (ScriptEventDecl& e : dispatchers ? graph.dispatchers : graph.events)
                        if (e.name == from)
                            e.name = name;
                    for (ScriptNode& n : graph.nodes) {
                        const bool user = dispatchers ? n.type.starts_with("Dispatcher.") : n.type == "Event.Custom" || n.type == "Flow.CallEvent";
                        if (user && n.param == from)
                            n.param = name;
                    }
                });
                ImGui::PopID();
                return;
            }
            ImGui::SameLine();
            if (dispatchers) {
                if (ImGui::SmallButton("Call"))
                    addNode("Dispatcher.Call", list[i].name);
                ImGui::SameLine();
                if (ImGui::SmallButton("Bind"))
                    addNode("Dispatcher.Bind", list[i].name);
            } else {
                if (ImGui::SmallButton("Event") && scope.empty())
                    addNode("Event.Custom", list[i].name);
                ImGui::SameLine();
                if (ImGui::SmallButton("Call"))
                    addNode("Flow.CallEvent", list[i].name);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                remove = i;
            ImGui::Indent();
            if (const auto edit = ParamListWidget(list[i].params, false, "+ Parameter", 100 * static_cast<int>(i) + (dispatchers ? 20000 : 10000))) {
                const std::string which = list[i].name;
                Edit("Parameters", [&](ScriptGraph& graph) {
                    for (ScriptEventDecl& e : dispatchers ? graph.dispatchers : graph.events)
                        if (e.name == which)
                            e.params = edit->params;
                    if (!edit->from.empty())
                        RenamePins(graph, [&](const ScriptNode& n) {
                            if (n.param != which)
                                return 0;
                            if (dispatchers)
                                return n.type == "Dispatcher.Call" ? 2 : 0;
                            return n.type == "Event.Custom" ? 1 : n.type == "Flow.CallEvent" ? 2 : 0;
                        }, edit->from, edit->to);
                    graph.RemoveDanglingLinks();
                });
                ImGui::Unindent();
                ImGui::PopID();
                return;
            }
            ImGui::Unindent();
            ImGui::PopID();
        }
        if (remove) {
            const std::string name = list[*remove].name;
            Edit("Remove", [&](ScriptGraph& graph) {
                std::erase_if(dispatchers ? graph.dispatchers : graph.events, [&](const ScriptEventDecl& e) { return e.name == name; });
                graph.RemoveDanglingLinks();
            });
            return;
        }
        if (ImGui::SmallButton(dispatchers ? "+ Dispatcher" : "+ Event")) {
            std::string name = dispatchers ? "OnChanged" : "MyEvent";
            for (int n = 1; std::ranges::any_of(list, [&](const ScriptEventDecl& e) { return e.name == name; }); ++n)
                name = (dispatchers ? "OnChanged" : "MyEvent") + std::to_string(n);
            Edit("Add", [&](ScriptGraph& graph) { (dispatchers ? graph.dispatchers : graph.events).push_back({name, {}}); });
            return;
        }
    }

    // Interfaces: adding one creates its missing functions.
    if (ImGui::CollapsingHeader("Interfaces")) {
        std::optional<std::string> remove;
        for (const std::string& name : g.interfaces) {
            ImGui::PushID(name.c_str());
            ImGui::BulletText("%s%s", name.c_str(), ScriptRegistry::FindInterface(name) ? "" : " (missing)");
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                remove = name;
            ImGui::PopID();
        }
        if (remove) {
            const std::string name = *remove;
            Edit("Remove interface", [&](ScriptGraph& graph) { std::erase(graph.interfaces, name); });
            return;
        }
        if (ImGui::BeginCombo("##addinterface", "Implement interface...")) {
            for (const std::string& name : ScriptRegistry::InterfaceNames())
                if (std::ranges::find(g.interfaces, name) == g.interfaces.end() && ImGui::Selectable(name.c_str())) {
                    const ScriptInterface iface = *ScriptRegistry::FindInterface(name);
                    Edit("Implement interface", [&](ScriptGraph& graph) {
                        graph.interfaces.push_back(name);
                        float y = 0.0f;
                        for (const ScriptInterfaceFunction& f : iface.functions) {
                            if (graph.FindFunction(f.name))
                                continue; // an existing function of that name must match by hand
                            graph.AddFunction(f.name, {0.0f, y += 200.0f});
                            ScriptFunction* fn = graph.FindFunction(f.name);
                            fn->inputs         = f.inputs;
                            fn->outputs        = f.outputs;
                            fn->description    = "Implements " + name;
                            graph.FunctionSignatureChanged(f.name);
                        }
                    });
                }
            ImGui::EndCombo();
        }
    }

    // Timelines.
    if (ImGui::CollapsingHeader("Timelines")) {
        std::optional<std::string> remove;
        for (const ScriptTimeline& t : g.timelines) {
            ImGui::PushID(("tl:" + t.name).c_str());
            if (ImGui::Selectable(std::format("{} ({:.2f} s{})", t.name, t.length, t.loop ? ", loop" : "").c_str(), m_Timeline == t.name,
                                  ImGuiSelectableFlags_AllowDoubleClick, ImVec2(-80.0f, 0.0f)))
                OpenTimeline(t.name);
            ImGui::SameLine();
            if (ImGui::SmallButton("Node") && scope.empty())
                addNode("Timeline.Play", t.name);
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                remove = t.name;
            ImGui::PopID();
        }
        if (remove) {
            const std::string name = *remove;
            Edit("Remove timeline", [&](ScriptGraph& graph) {
                std::erase_if(graph.timelines, [&](const ScriptTimeline& t) { return t.name == name; });
                std::vector<std::uint32_t> users;
                for (const ScriptNode& n : graph.nodes)
                    if (n.type == "Timeline.Play" && n.param == name)
                        users.push_back(n.id);
                for (std::uint32_t id : users)
                    graph.RemoveNode(id);
            });
            std::erase_if(doc.selected, [&](std::uint32_t id) { return !doc.graph.FindNode(id); });
            return;
        }
        if (ImGui::SmallButton("+ Timeline")) {
            std::string name = "Timeline";
            for (int n = 1; g.FindTimeline(name); ++n)
                name = "Timeline" + std::to_string(n);
            Edit("Add timeline", [&](ScriptGraph& graph) {
                ScriptTimeline t{name, 1.0f, false, false, {}};
                t.tracks.push_back({"Alpha", ScriptTrackKind::Float, {{0.0f, glm::vec3(0.0f)}, {1.0f, glm::vec3(1.0f)}}});
                graph.timelines.push_back(std::move(t));
            });
            OpenTimeline(name);
        }
    }
}

void ScriptGraphEditor::OpenTimeline(const std::string& name)
{
    m_Timeline      = name;
    m_TimelineTrack = 0;
    m_TimelineKey   = -1;
}

void ScriptGraphEditor::DrawTimeline(Document& doc)
{
    m_TimelineCurveRect = glm::vec4(0.0f);
    if (m_Timeline.empty())
        return;
    ScriptTimeline* t = doc.graph.FindTimeline(m_Timeline);
    if (!t) {
        m_Timeline.clear();
        return;
    }
    bool          open   = true;
    const ImVec2  center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(ImVec2(center.x - 40.0f, center.y - 60.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(640.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(("Timeline: " + t->name + "###TimelineEditor").c_str(), &open)) {
        ImGui::End();
        if (!open)
            m_Timeline.clear();
        return;
    }
    // Name (renaming updates the nodes), length, loop, auto play.
    std::string name = t->name;
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputText("##tlname", &name);
    if (ImGui::IsItemDeactivatedAfterEdit() && name != t->name && IsValidScriptName(name) && !doc.graph.FindTimeline(name)) {
        const std::string from = t->name;
        Edit("Rename timeline", [&](ScriptGraph& graph) {
            graph.FindTimeline(from)->name = name;
            for (ScriptNode& n : graph.nodes)
                if (n.type == "Timeline.Play" && n.param == from)
                    n.param = name;
        });
        m_Timeline = name;
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    m_WidgetTouched |= ImGui::DragFloat("Length", &t->length, 0.05f, 0.01f, 3600.0f, "%.2f s");
    t->length = std::max(t->length, 0.01f);
    ImGui::SameLine();
    m_WidgetTouched |= ImGui::Checkbox("Loop", &t->loop);
    ImGui::SameLine();
    m_WidgetTouched |= ImGui::Checkbox("Auto play", &t->autoPlay);

    // Tracks.
    std::optional<std::size_t> removeTrack;
    for (std::size_t i = 0; i < t->tracks.size(); ++i) {
        ScriptTimelineTrack& track = t->tracks[i];
        ImGui::PushID(static_cast<int>(i));
        static constexpr const char* kKinds[] = {"float", "vector", "event"};
        if (ImGui::RadioButton("##sel", m_TimelineTrack == static_cast<int>(i))) {
            m_TimelineTrack = static_cast<int>(i);
            m_TimelineKey   = -1;
        }
        ImGui::SameLine();
        std::string tname = track.name;
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputText("##trname", &tname);
        if (ImGui::IsItemDeactivatedAfterEdit() && tname != track.name && IsValidScriptName(tname) &&
            std::ranges::none_of(t->tracks, [&](const ScriptTimelineTrack& x) { return x.name == tname; })) {
            const std::string timeline = t->name, from = track.name;
            Edit("Rename track", [&](ScriptGraph& graph) {
                for (ScriptTimelineTrack& x : graph.FindTimeline(timeline)->tracks)
                    if (x.name == from)
                        x.name = tname;
                RenamePins(graph, [&](const ScriptNode& n) { return n.type == "Timeline.Play" && n.param == timeline ? 1 : 0; }, from,
                           tname);
            });
            ImGui::PopID();
            ImGui::End();
            return;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s, %zu keys", kKinds[static_cast<int>(track.kind)], track.keys.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            removeTrack = i;
        ImGui::PopID();
    }
    const auto addTrack = [&](const char* base, ScriptTrackKind kind) {
        std::string tname = base;
        for (int n = 1; std::ranges::any_of(t->tracks, [&](const ScriptTimelineTrack& x) { return x.name == tname; }); ++n)
            tname = base + std::to_string(n);
        const std::string timeline = t->name;
        Edit("Add track", [&](ScriptGraph& graph) {
            ScriptTimelineTrack track{tname, kind, {}};
            if (kind != ScriptTrackKind::Event)
                track.keys = {{0.0f, glm::vec3(0.0f)}, {graph.FindTimeline(timeline)->length, glm::vec3(1.0f)}};
            graph.FindTimeline(timeline)->tracks.push_back(std::move(track));
        });
        m_TimelineTrack = static_cast<int>(t->tracks.size()) - 1;
    };
    if (removeTrack) {
        const std::string timeline = t->name, track = t->tracks[*removeTrack].name;
        Edit("Remove track", [&](ScriptGraph& graph) {
            std::erase_if(graph.FindTimeline(timeline)->tracks, [&](const ScriptTimelineTrack& x) { return x.name == track; });
            graph.RemoveDanglingLinks();
        });
        m_TimelineTrack = 0;
        ImGui::End();
        return;
    }
    if (ImGui::SmallButton("+ Float track"))
        addTrack("Value", ScriptTrackKind::Float);
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Vector track"))
        addTrack("Vector", ScriptTrackKind::Vector);
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Event track"))
        addTrack("Event", ScriptTrackKind::Event);
    t = doc.graph.FindTimeline(m_Timeline); // Edit may have replaced the graph
    if (!t || t->tracks.empty()) {
        ImGui::End();
        if (!open)
            m_Timeline.clear();
        return;
    }
    m_TimelineTrack             = std::clamp(m_TimelineTrack, 0, static_cast<int>(t->tracks.size()) - 1);
    ScriptTimelineTrack& track  = t->tracks[static_cast<std::size_t>(m_TimelineTrack)];
    const bool           events = track.kind == ScriptTrackKind::Event;
    if (track.kind == ScriptTrackKind::Vector) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        ImGui::Combo("Drag", &m_TimelineComponent, "X\0Y\0Z\0");
    }

    // Curve view: time to the right, value up. Double-click adds a key, drag moves one.
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y * 0.55f, 120.0f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    m_TimelineCurveRect = {p0.x, p0.y, size.x, size.y};
    ImGui::InvisibleButton("##curve", size);
    const bool  hovered = ImGui::IsItemHovered();
    ImDrawList* dl      = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(28, 28, 32, 255));
    float lo = 0.0f, hi = 1.0f;
    for (const ScriptTimelineKey& k : track.keys)
        for (int c = 0; c < (track.kind == ScriptTrackKind::Vector ? 3 : 1); ++c) {
            lo = std::min(lo, k.value[c]);
            hi = std::max(hi, k.value[c]);
        }
    const float pad = (hi - lo) * 0.1f + 1e-3f;
    lo -= pad;
    hi += pad;
    const float length = std::max(t->length, 0.01f);
    const auto  toScreen = [&](float time, float value) {
        return ImVec2(p0.x + time / length * size.x, p0.y + (1.0f - (value - lo) / (hi - lo)) * size.y);
    };
    const auto fromScreen = [&](ImVec2 p) {
        return glm::vec2(std::clamp((p.x - p0.x) / size.x, 0.0f, 1.0f) * length, lo + (1.0f - (p.y - p0.y) / size.y) * (hi - lo));
    };
    for (int sec = 0; sec <= static_cast<int>(length); ++sec) // second marks
        dl->AddLine(toScreen(static_cast<float>(sec), lo), toScreen(static_cast<float>(sec), hi), IM_COL32(60, 60, 68, 255));
    if (lo < 0.0f && hi > 0.0f)
        dl->AddLine(toScreen(0.0f, 0.0f), toScreen(length, 0.0f), IM_COL32(80, 80, 90, 255));
    static constexpr ImU32 kColors[] = {IM_COL32(240, 90, 80, 255), IM_COL32(110, 220, 100, 255), IM_COL32(90, 150, 250, 255)};
    if (!events)
        for (int c = 0; c < (track.kind == ScriptTrackKind::Vector ? 3 : 1); ++c) {
            ImVec2 prev = toScreen(0.0f, EvaluateTrack(track, 0.0f)[c]);
            for (int i = 1; i <= 96; ++i) {
                const float  time = length * static_cast<float>(i) / 96.0f;
                const ImVec2 cur  = toScreen(time, EvaluateTrack(track, time)[c]);
                dl->AddLine(prev, cur, track.kind == ScriptTrackKind::Vector ? kColors[c] : IM_COL32(240, 200, 80, 255), 2.0f);
                prev = cur;
            }
        }
    const int component = track.kind == ScriptTrackKind::Vector ? m_TimelineComponent : 0;
    int       hoveredKey = -1;
    for (std::size_t i = 0; i < track.keys.size(); ++i) {
        const ScriptTimelineKey& k = track.keys[i];
        const ImVec2 at = events ? ImVec2(toScreen(k.time, 0.0f).x, p0.y + size.y * 0.5f) : toScreen(k.time, k.value[component]);
        if (events)
            dl->AddLine(ImVec2(at.x, p0.y), ImVec2(at.x, p0.y + size.y), IM_COL32(230, 160, 60, 255), 2.0f);
        const bool near = hovered && std::abs(ImGui::GetMousePos().x - at.x) < 6.0f && std::abs(ImGui::GetMousePos().y - at.y) < 6.0f;
        if (near)
            hoveredKey = static_cast<int>(i);
        dl->AddCircleFilled(at, static_cast<int>(i) == m_TimelineKey ? 6.0f : 4.5f,
                            near || static_cast<int>(i) == m_TimelineKey ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 200, 255));
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_TimelineKey      = hoveredKey;
        m_TimelineDragging = hoveredKey >= 0;
    }
    if (hovered && hoveredKey < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const glm::vec2   at   = fromScreen(ImGui::GetMousePos());
        ScriptTimelineKey key{at.x, EvaluateTrack(track, at.x), ScriptInterp::Linear};
        if (!events)
            key.value[component] = at.y;
        track.keys.push_back(key);
        m_WidgetTouched = true;
    }
    if (m_TimelineDragging && m_TimelineKey >= 0 && m_TimelineKey < static_cast<int>(track.keys.size())) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const glm::vec2 at = fromScreen(ImGui::GetMousePos());
            ScriptTimelineKey& k = track.keys[static_cast<std::size_t>(m_TimelineKey)];
            k.time               = at.x;
            if (!events)
                k.value[component] = at.y;
            m_WidgetTouched = true;
        } else {
            m_TimelineDragging = false;
        }
    }

    // Keys as a table.
    std::optional<std::size_t> removeKey;
    if (ImGui::BeginTable("##keys", events ? 2 : 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        for (std::size_t i = 0; i < track.keys.size(); ++i) {
            ScriptTimelineKey& k = track.keys[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            m_WidgetTouched |= ImGui::DragFloat("##t", &k.time, 0.01f, 0.0f, t->length, "%.3f s");
            if (ImGui::IsItemActivated())
                m_TimelineKey = static_cast<int>(i);
            if (!events) {
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                m_WidgetTouched |= track.kind == ScriptTrackKind::Vector ? ImGui::DragFloat3("##v", &k.value.x, 0.01f)
                                                                         : ImGui::DragFloat("##v", &k.value.x, 0.01f);
            }
            ImGui::TableNextColumn();
            if (!events) {
                int interp = static_cast<int>(k.interp);
                ImGui::SetNextItemWidth(90.0f);
                if (ImGui::Combo("##i", &interp, "Linear\0Constant\0Smooth\0")) {
                    k.interp        = static_cast<ScriptInterp>(interp);
                    m_WidgetTouched = true;
                }
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("x"))
                removeKey = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeKey) {
        track.keys.erase(track.keys.begin() + static_cast<std::ptrdiff_t>(*removeKey));
        m_TimelineKey   = -1;
        m_WidgetTouched = true;
    }
    // Keys stay sorted (the selected one is followed).
    if (!std::ranges::is_sorted(track.keys, {}, &ScriptTimelineKey::time)) {
        const std::optional<ScriptTimelineKey> selected =
            m_TimelineKey >= 0 && m_TimelineKey < static_cast<int>(track.keys.size()) ? std::optional(track.keys[static_cast<std::size_t>(m_TimelineKey)])
                                                                                      : std::nullopt;
        std::ranges::stable_sort(track.keys, {}, &ScriptTimelineKey::time);
        if (selected)
            for (std::size_t i = 0; i < track.keys.size(); ++i)
                if (track.keys[i].time == selected->time && track.keys[i].value == selected->value)
                    m_TimelineKey = static_cast<int>(i);
    }
    for (ScriptTimelineKey& k : track.keys)
        k.time = std::clamp(k.time, 0.0f, t->length);
    ImGui::TextDisabled("Double-click adds a key, drag moves it. Event tracks fire their output at each key.");
    ImGui::End();
    if (!open)
        m_Timeline.clear();
}

std::string ScriptGraphEditor::CollapseSelection(bool macro)
{
    Document* doc = ActiveDoc();
    if (!doc || doc->selected.empty())
        return "Nothing selected";
    const std::string name = UniqueName(doc->graph, {}, macro ? "NewMacro" : "NewFunction", true);
    glm::vec2         center(0.0f);
    for (std::uint32_t id : doc->selected)
        if (const ScriptNode* n = doc->graph.FindNode(id))
            center += n->position / static_cast<float>(doc->selected.size());
    ScriptGraph       probe = doc->graph; // the graph stays untouched (and without an undo step) on errors
    const std::string error = probe.Collapse(doc->selected, name, macro, center);
    if (!error.empty())
        return error;
    const std::vector<std::uint32_t> nodes = doc->selected;
    Edit(macro ? "Collapse to macro" : "Collapse to function",
         [&](ScriptGraph& graph) { (void)graph.Collapse(nodes, name, macro, center); });
    doc->selected.clear();
    for (const ScriptNode& n : doc->graph.nodes)
        if (n.param == name && n.function == doc->scope && (n.type == "Macro.Use" || n.type.starts_with("Function.Call")))
            doc->selected = {n.id};
    return {};
}

std::vector<ScriptGraphEditor::SearchHit> ScriptGraphEditor::Search(const std::string& query, bool references) const
{
    std::vector<SearchHit> hits;
    if (query.empty())
        return hits;
    std::vector<std::pair<std::filesystem::path, const ScriptGraph*>> graphs;
    std::unordered_set<std::string>                                    seen;
    for (const auto& d : m_Docs) {
        graphs.emplace_back(d->path, &d->graph);
        seen.insert(ScriptSystem::Key(d->path));
    }
    std::error_code ec;
    if (!m_SearchRoot.empty())
        for (std::filesystem::recursive_directory_iterator it(m_SearchRoot, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            const std::string key = ScriptSystem::Key(it->path());
            if (it->path().extension() != ".ugraph" || seen.contains(key))
                continue;
            std::error_code timeError;
            const auto      time   = std::filesystem::last_write_time(it->path(), timeError);
            auto&           cached = m_SearchCache[key];
            if (!cached.second || cached.first != time) { // parsed again only when the file changed
                try {
                    cached = {time, std::make_shared<const ScriptGraph>(LoadScriptGraph(it->path()))};
                } catch (const std::exception&) { // broken files are not searched
                    m_SearchCache.erase(key);
                    continue;
                }
            }
            graphs.emplace_back(it->path(), cached.second.get());
        }
    const std::string lower = Lower(query);
    for (const auto& [path, graph] : graphs)
        for (const ScriptNode& n : graph->nodes) {
            const NodeDesc*   desc  = FindScriptNodeType(n.type);
            const std::string label = NodeTitle(desc, n);
            bool              match = false;
            if (references) {
                const std::string_view param = n.param;
                match = param == query || (param.size() > query.size() && param.ends_with(query) && param[param.size() - query.size() - 1] == '.') ||
                        std::ranges::any_of(n.defaults, [&](const auto& kv) {
                            const auto* text = std::get_if<std::string>(&kv.second);
                            return text && *text == query;
                        });
            } else {
                match = Lower(label + " " + n.param + " " + n.type).find(lower) != std::string::npos;
            }
            if (match)
                hits.push_back({path, n.function, n.id, label});
        }
    return hits;
}

void ScriptGraphEditor::OpenSearch(std::string query, bool references)
{
    m_SearchQuery      = std::move(query);
    m_SearchReferences = references;
    m_SearchOpen       = true;
    m_SearchFocus      = true;
    m_SearchHits       = Search(m_SearchQuery, m_SearchReferences);
}

void ScriptGraphEditor::ShowHit(const SearchHit& hit)
{
    if (!Open(hit.file))
        return;
    Document& doc = *ActiveDoc();
    OpenScope(hit.scope);
    if (doc.graph.FindNode(hit.node)) {
        doc.selected = {hit.node};
        FrameNodes(doc, true);
        doc.framed = true;
    }
    m_FocusRequested = true;
}

void ScriptGraphEditor::DrawSearch()
{
    if (!m_SearchOpen)
        return;
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(ImVec2(center.x + 140.0f, center.y - 320.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 300.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Find in Blueprints", &m_SearchOpen)) {
        ImGui::End();
        return;
    }
    if (std::exchange(m_SearchFocus, false))
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-200.0f);
    const bool enter = ImGui::InputTextWithHint("##query", "Search node titles / names...", &m_SearchQuery,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool changed = ImGui::Checkbox("References", &m_SearchReferences);
    ImGui::SameLine();
    if (ImGui::Button("Search") || enter || changed)
        m_SearchHits = Search(m_SearchQuery, m_SearchReferences);
    ImGui::TextDisabled("%zu result(s)%s", m_SearchHits.size(),
                        m_SearchRoot.empty() ? " in the open graphs" : " in the open graphs and the project");
    std::optional<SearchHit> show;
    if (ImGui::BeginTable("##hits", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Node");
        ImGui::TableSetupColumn("Graph");
        ImGui::TableSetupColumn("File");
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < m_SearchHits.size(); ++i) {
            const SearchHit& h = m_SearchHits[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(std::format("{}##hit{}", h.label, i).c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
                show = h;
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(h.scope.empty() ? "Event Graph" : h.scope.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(h.file.filename().string().c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
    if (show)
        ShowHit(*show);
}

} // namespace Engine
