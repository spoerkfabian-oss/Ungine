#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptNodes.h"
#include "Engine/Script/ScriptRegistry.h"

#include "ScriptExpand.h"
#include "ScriptJson.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Engine {

using json = nlohmann::json;

namespace {
constexpr int kGraphVersion = 2;

std::string ToUtf8(const std::filesystem::path& p)
{
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

template <class Range, class Id>
auto FindById(Range& range, Id id)
{
    const auto it = std::ranges::find_if(range, [id](const auto& x) { return x.id == id; });
    return it != range.end() ? &*it : nullptr;
}

template <class Range>
auto FindByName(Range& range, std::string_view name)
{
    const auto it = std::ranges::find_if(range, [&](const auto& x) { return x.name == name; });
    return it != range.end() ? &*it : nullptr;
}

bool IsFunctionNode(const std::string& type)
{
    return type == "Function.Entry" || type == "Function.Return" || type == "Function.Call" || type == "Function.CallPure";
}

// Enums / structs must be registered (ScriptRegistry).
bool KnownType(PinType t)
{
    const auto check = [](PinKind kind, std::uint16_t name) {
        if (kind == PinKind::Enum)
            return ScriptRegistry::FindEnum(TypeNameOf(name)) != nullptr;
        if (kind == PinKind::Struct)
            return ScriptRegistry::FindStruct(TypeNameOf(name)) != nullptr;
        return true;
    };
    return check(t.kind, t.name) && (!IsMap(t) || check(t.keyKind, t.keyName));
}

bool UsesVariable(const ScriptNode& node, const std::string& name)
{
    const NodeDesc* desc = FindScriptNodeType(node.type);
    return desc && desc->paramKind == ParamKind::Variable && node.param == name;
}
} // namespace

// --- Graph editing -----------------------------------------------------------------------------

ScriptNode*           ScriptGraph::FindNode(std::uint32_t id) { return FindById(nodes, id); }
const ScriptNode*     ScriptGraph::FindNode(std::uint32_t id) const { return FindById(nodes, id); }
ScriptComment*        ScriptGraph::FindComment(std::uint32_t id) { return FindById(comments, id); }
ScriptVariable*       ScriptGraph::FindVariable(std::string_view name) { return FindByName(variables, name); }
const ScriptVariable* ScriptGraph::FindVariable(std::string_view name) const { return FindByName(variables, name); }
ScriptMacro*          ScriptGraph::FindMacro(std::string_view name) { return FindByName(macros, name); }
const ScriptMacro*    ScriptGraph::FindMacro(std::string_view name) const { return FindByName(macros, name); }
const ScriptEventDecl* ScriptGraph::FindEvent(std::string_view name) const { return FindByName(events, name); }
const ScriptEventDecl* ScriptGraph::FindDispatcher(std::string_view name) const { return FindByName(dispatchers, name); }
const ScriptTimeline* ScriptGraph::FindTimeline(std::string_view name) const { return FindByName(timelines, name); }
ScriptTimeline*       ScriptGraph::FindTimeline(std::string_view name) { return FindByName(timelines, name); }

glm::vec3 EvaluateTrack(const ScriptTimelineTrack& track, float time)
{
    const std::vector<ScriptTimelineKey>& k = track.keys;
    if (k.empty())
        return glm::vec3(0.0f);
    if (time <= k.front().time)
        return k.front().value;
    if (time >= k.back().time)
        return k.back().value;
    const auto next = std::ranges::upper_bound(k, time, {}, &ScriptTimelineKey::time);
    const ScriptTimelineKey& a = *(next - 1);
    const ScriptTimelineKey& b = *next;
    float t = (time - a.time) / std::max(b.time - a.time, 1e-6f);
    switch (a.interp) {
    case ScriptInterp::Constant: t = 0.0f; break;
    case ScriptInterp::Smooth: t = t * t * (3.0f - 2.0f * t); break;
    case ScriptInterp::Linear: break;
    }
    return a.value + (b.value - a.value) * t;
}
bool ScriptGraph::HasScope(std::string_view name) const { return FindFunction(name) || FindMacro(name); }
ScriptFunction*       ScriptGraph::FindFunction(std::string_view name) { return FindByName(functions, name); }
const ScriptFunction* ScriptGraph::FindFunction(std::string_view name) const { return FindByName(functions, name); }

const ScriptVariable* ScriptGraph::FindVariableInScope(std::string_view function, std::string_view name) const
{
    if (!function.empty())
        if (const ScriptFunction* f = FindFunction(function))
            if (const ScriptVariable* local = FindByName(f->locals, name))
                return local;
    return FindVariable(name);
}

ScriptVariable* ScriptGraph::FindVariableInScope(std::string_view function, std::string_view name)
{
    return const_cast<ScriptVariable*>(std::as_const(*this).FindVariableInScope(function, name));
}

std::uint32_t ScriptGraph::AddNode(const std::string& type, glm::vec2 position, std::string param, std::string function)
{
    if (param.empty())
        if (const NodeDesc* desc = FindScriptNodeType(type))
            param = desc->paramDefault;
    const std::uint32_t id = nextId++;
    nodes.push_back({.id = id, .type = type, .position = position, .param = std::move(param), .defaults = {},
                     .function = std::move(function)});
    return id;
}

void ScriptGraph::RemoveNode(std::uint32_t id)
{
    std::erase_if(nodes, [id](const ScriptNode& n) { return n.id == id; });
    std::erase_if(links, [id](const ScriptLink& l) { return l.fromNode == id || l.toNode == id; });
    SetBreakpoint(id, false);
}

std::uint32_t ScriptGraph::AddComment(glm::vec2 position, glm::vec2 size, std::string text, std::string function)
{
    const std::uint32_t id = nextId++;
    comments.push_back({.id       = id,
                        .position = position,
                        .size     = size,
                        .text     = std::move(text),
                        .color    = {0.3f, 0.45f, 0.7f},
                        .function = std::move(function)});
    return id;
}

void ScriptGraph::RemoveComment(std::uint32_t id)
{
    std::erase_if(comments, [id](const ScriptComment& c) { return c.id == id; });
}

std::string ScriptGraph::Connect(std::uint32_t from, const std::string& fromPin, std::uint32_t to, const std::string& toPin)
{
    ScriptNode* a = FindNode(from);
    ScriptNode* b = FindNode(to);
    if (!a || !b)
        return "Unknown node";
    if (from == to)
        return "Cannot connect a node to itself";
    if (a->function != b->function)
        return "Nodes are in different functions";
    std::optional<PinInfo> outA = FindPin(*this, *a, fromPin, true);
    std::optional<PinInfo> inB  = FindPin(*this, *b, toPin, false);
    if (!outA || !inB)
        return "Connect an output to an input";

    // Generic nodes adapt their param to what is connected: reroutes take the pin type, array
    // nodes the element type, map nodes the map type, struct / enum nodes the type's name.
    bool            changed = false;
    const NodeDesc* descA   = FindScriptNodeType(a->type);
    const NodeDesc* descB   = FindScriptNodeType(b->type);
    const auto      adapt   = [&](ScriptNode& node, const NodeDesc& desc, PinType wanted, bool output) {
        if (wanted == PinType::Exec)
            return;
        std::string param;
        switch (desc.inference) {
        case ParamInference::PinType:
        case ParamInference::MapType: param = ToString(wanted); break;
        case ParamInference::ElementType: param = ToString(ElementType(wanted)); break;
        case ParamInference::UserType: param = TypeNameOf(wanted.name); break;
        case ParamInference::None: return;
        }
        if (desc.paramKind == ParamKind::TypeAndCount) // keep the count
            param += ":" + std::to_string(SplitTypeAndCount(node.param, 2).second);
        // A reroute whose input is connected keeps its type: only a free one adapts to its target.
        if (node.param == param ||
            (output && desc.inference == ParamInference::PinType &&
             std::ranges::any_of(links, [&](const ScriptLink& l) { return l.toNode == node.id; })))
            return;
        node.param = param;
        changed    = true;
    };
    // Whether `desc` adapts when its pin (of type `own`) meets `other`.
    const auto infers = [](const NodeDesc* desc, const std::string& pin, PinType own, PinType other) {
        if (!desc || (desc->infers && !desc->infers(pin)))
            return false;
        switch (desc->inference) {
        case ParamInference::PinType: return true;
        case ParamInference::ElementType: return IsArray(own) && IsArray(other);
        case ParamInference::MapType: return IsMap(own) && IsMap(other);
        case ParamInference::UserType:
            return !IsContainer(own) && !IsContainer(other) && IsUserType(own) && own.kind == other.kind;
        case ParamInference::None: break;
        }
        return false;
    };
    if (inB->type != outA->type && infers(descB, toPin, inB->type, outA->type))
        adapt(*b, *descB, outA->type, false);
    inB = FindPin(*this, *b, toPin, false);
    if (inB && !CanConvert(outA->type, inB->type) && infers(descA, fromPin, outA->type, inB->type))
        adapt(*a, *descA, inB->type, true);
    outA = FindPin(*this, *a, fromPin, true);
    inB  = FindPin(*this, *b, toPin, false);
    if (!outA || !inB)
        return "Connect an output to an input";
    if (!CanConvert(outA->type, inB->type)) {
        if (changed)
            RemoveDanglingLinks();
        return std::format("{} is not compatible with {}", DisplayName(outA->type), DisplayName(inB->type));
    }

    const ScriptLink link{.fromNode = from, .fromPin = fromPin, .toNode = to, .toPin = toPin};
    if (std::ranges::find(links, link) == links.end()) {
        if (outA->type == PinType::Exec) // one target per exec output
            std::erase_if(links, [&](const ScriptLink& l) { return l.fromNode == from && l.fromPin == fromPin; });
        else // one source per data input
            std::erase_if(links, [&](const ScriptLink& l) { return l.toNode == to && l.toPin == toPin; });
        links.push_back(link);
    }
    if (changed) // other links of the adapted node may not fit anymore
        RemoveDanglingLinks();
    return {};
}

void ScriptGraph::Disconnect(std::uint32_t node, const std::string& pin, bool output)
{
    std::erase_if(links, [&](const ScriptLink& l) {
        return output ? l.fromNode == node && l.fromPin == pin : l.toNode == node && l.toPin == pin;
    });
}

std::size_t ScriptGraph::RemoveDanglingLinks()
{
    return std::erase_if(links, [&](const ScriptLink& l) {
        const ScriptNode* a = FindNode(l.fromNode);
        const ScriptNode* b = FindNode(l.toNode);
        if (!a || !b || a->function != b->function)
            return true;
        const auto out = FindPin(*this, *a, l.fromPin, true);
        const auto in  = FindPin(*this, *b, l.toPin, false);
        return !out || !in || !CanConvert(out->type, in->type);
    });
}

bool ScriptGraph::RenameVariable(const std::string& from, const std::string& to, const std::string& function)
{
    std::vector<ScriptVariable>* list = &variables;
    if (!function.empty()) {
        ScriptFunction* f = FindFunction(function);
        if (!f)
            return false;
        list = &f->locals;
    }
    ScriptVariable* v = FindByName(*list, from);
    if (!v || !IsValidScriptName(to) || FindByName(*list, to))
        return false;
    v->name = to;
    for (ScriptNode& n : nodes) {
        if (!UsesVariable(n, from))
            continue;
        // A graph variable is hidden inside functions with a local of the same name.
        const ScriptFunction* scope    = n.function.empty() ? nullptr : FindFunction(n.function);
        const bool            shadowed = scope && FindByName(scope->locals, from);
        const bool            inScope  = function.empty() ? !shadowed : n.function == function;
        if (inScope)
            n.param = to;
    }
    return true;
}

void ScriptGraph::SetVariableType(const std::string& name, PinType type, const std::string& function)
{
    ScriptVariable* v = function.empty() ? FindVariable(name) : nullptr;
    if (!function.empty())
        if (ScriptFunction* f = FindFunction(function))
            v = FindByName(f->locals, name);
    if (!v || v->type == type || type.kind == PinKind::Exec)
        return;
    v->type  = type;
    v->value = DefaultValue(type);
    for (ScriptNode& n : nodes)
        if (UsesVariable(n, name) && (function.empty() || n.function == function))
            n.defaults.clear(); // typed like the variable
    RemoveDanglingLinks();
}

bool IsValidScriptName(std::string_view name)
{
    return !name.empty() && name.size() <= 64 && name.front() != ' ' && name.back() != ' ' &&
           std::ranges::all_of(name, [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ' ';
           });
}

bool ScriptGraph::AddFunction(const std::string& name, glm::vec2 entryPosition)
{
    if (!IsValidScriptName(name) || HasScope(name))
        return false;
    functions.push_back({.name = name, .inputs = {}, .outputs = {}, .locals = {}, .pure = false, .description = {}});
    const std::uint32_t entry = AddNode("Function.Entry", entryPosition, name, name);
    const std::uint32_t ret   = AddNode("Function.Return", entryPosition + glm::vec2(400.0f, 0.0f), name, name);
    Connect(entry, "Then", ret, "In");
    return true;
}

bool ScriptGraph::RenameFunction(const std::string& from, const std::string& to)
{
    ScriptFunction* f = FindFunction(from);
    if (!f || !IsValidScriptName(to) || HasScope(to))
        return false;
    f->name = to;
    for (ScriptNode& n : nodes) {
        if (n.function == from)
            n.function = to;
        if (IsFunctionNode(n.type) && n.param == from)
            n.param = to;
    }
    for (ScriptComment& c : comments)
        if (c.function == from)
            c.function = to;
    return true;
}

void ScriptGraph::RemoveFunction(const std::string& name)
{
    std::vector<std::uint32_t> remove;
    for (const ScriptNode& n : nodes)
        if (n.function == name || ((n.type == "Function.Call" || n.type == "Function.CallPure") && n.param == name))
            remove.push_back(n.id);
    for (std::uint32_t id : remove)
        RemoveNode(id);
    std::erase_if(comments, [&](const ScriptComment& c) { return c.function == name; });
    std::erase_if(functions, [&](const ScriptFunction& f) { return f.name == name; });
}

void ScriptGraph::SetFunctionPure(const std::string& name, bool pure)
{
    ScriptFunction* f = FindFunction(name);
    if (!f || f->pure == pure)
        return;
    f->pure = pure;
    for (ScriptNode& n : nodes)
        if ((n.type == "Function.Call" || n.type == "Function.CallPure") && n.param == name)
            n.type = pure ? "Function.CallPure" : "Function.Call";
    RemoveDanglingLinks(); // exec links of the calls
}

bool ScriptGraph::AddMacro(const std::string& name, glm::vec2 inputsPosition)
{
    if (!IsValidScriptName(name) || HasScope(name))
        return false;
    macros.push_back({.name = name, .inputs = {{"In", PinType::Exec}}, .outputs = {{"Out", PinType::Exec}}, .description = {}});
    const std::uint32_t in  = AddNode("Macro.Inputs", inputsPosition, name, name);
    const std::uint32_t out = AddNode("Macro.Outputs", inputsPosition + glm::vec2(400.0f, 0.0f), name, name);
    Connect(in, "In", out, "Out");
    return true;
}

bool ScriptGraph::RenameMacro(const std::string& from, const std::string& to)
{
    ScriptMacro* m = FindMacro(from);
    if (!m || !IsValidScriptName(to) || HasScope(to))
        return false;
    m->name = to;
    for (ScriptNode& n : nodes) {
        if (n.function == from)
            n.function = to;
        if ((n.type == "Macro.Inputs" || n.type == "Macro.Outputs" || n.type == "Macro.Use") && n.param == from)
            n.param = to;
    }
    for (ScriptComment& c : comments)
        if (c.function == from)
            c.function = to;
    return true;
}

void ScriptGraph::RemoveMacro(const std::string& name)
{
    std::vector<std::uint32_t> remove;
    for (const ScriptNode& n : nodes)
        if (n.function == name || (n.type == "Macro.Use" && n.param == name))
            remove.push_back(n.id);
    for (std::uint32_t id : remove)
        RemoveNode(id);
    std::erase_if(comments, [&](const ScriptComment& c) { return c.function == name; });
    std::erase_if(macros, [&](const ScriptMacro& m) { return m.name == name; });
}

void ScriptGraph::FunctionSignatureChanged(const std::string& name)
{
    (void)name;
    RemoveDanglingLinks();
}

std::string ScriptGraph::Collapse(const std::vector<std::uint32_t>& ids, const std::string& name, bool macro, glm::vec2 position)
{
    if (ids.empty())
        return "Nothing selected";
    if (!IsValidScriptName(name) || HasScope(name))
        return "The name must be a new unique name (letters, digits, '_', ' ')";
    const std::unordered_set<std::uint32_t> sel(ids.begin(), ids.end());
    const std::string                       scope = FindNode(ids.front()) ? FindNode(ids.front())->function : std::string();
    bool                                    impure = false;
    for (std::uint32_t id : ids) {
        const ScriptNode* n    = FindNode(id);
        const NodeDesc*   desc = n ? FindScriptNodeType(n->type) : nullptr;
        if (!n || !desc)
            return "Unknown node in the selection";
        if (n->function != scope)
            return "The nodes must belong to the same graph / function";
        if (desc->kind == NodeKind::Event || n->type == "Function.Return" || n->type == "Macro.Outputs")
            return "Events, function entries / returns and macro tunnels cannot be collapsed";
        if (!macro && desc->latent)
            return desc->title + " is latent: collapse to a macro instead";
        impure |= desc->kind == NodeKind::Impure;
    }

    // Boundary links, grouped: inside targets of outside exec, outside data sources, inside data
    // sources used outside, inside exec outputs leading out.
    using PinKey = std::pair<std::uint32_t, std::string>;
    struct Boundary {
        PinKey             key;
        PinType            type;
        std::string        name;
        std::vector<PinKey> other; // the pins on the other side
    };
    std::vector<Boundary> execIn, dataIn, execOut, dataOut;
    const auto group = [](std::vector<Boundary>& list, PinKey key, PinType type, PinKey other) {
        auto it = std::ranges::find(list, key, &Boundary::key);
        if (it == list.end()) {
            list.push_back({key, type, {}, {}});
            it = list.end() - 1;
        }
        it->other.push_back(std::move(other));
    };
    for (const ScriptLink& l : links) {
        const bool from = sel.contains(l.fromNode), to = sel.contains(l.toNode);
        if (from == to)
            continue;
        const ScriptNode* source = FindNode(l.fromNode);
        const auto        pin    = source ? FindPin(*this, *source, l.fromPin, true) : std::nullopt;
        if (!pin)
            continue;
        const bool exec = pin->type == PinType::Exec;
        if (to)
            group(exec ? execIn : dataIn, exec ? PinKey{l.toNode, l.toPin} : PinKey{l.fromNode, l.fromPin}, pin->type,
                  exec ? PinKey{l.fromNode, l.fromPin} : PinKey{l.toNode, l.toPin});
        else
            group(exec ? execOut : dataOut, {l.fromNode, l.fromPin}, pin->type, {l.toNode, l.toPin});
    }
    if (!macro && (execIn.size() > 1 || execOut.size() > 1))
        return "A function has one exec entry and exit: collapse to a macro instead";

    // Parameter names: unique per side, never the call's own exec pins.
    std::unordered_set<std::string> inNames{"In"}, outNames{"Then"};
    const auto unique = [](std::unordered_set<std::string>& used, std::string base) {
        std::string candidate = base;
        for (int i = 2; !IsValidScriptName(candidate) || used.contains(candidate); ++i)
            candidate = (IsValidScriptName(base) ? base : std::string("Value")) + " " + std::to_string(i);
        used.insert(candidate);
        return candidate;
    };
    if (macro) {
        inNames.clear();
        outNames.clear();
        for (std::size_t i = 0; i < execIn.size(); ++i)
            execIn[i].name = unique(inNames, "In");
        for (std::size_t i = 0; i < execOut.size(); ++i)
            execOut[i].name = unique(outNames, "Out");
    }
    for (Boundary& b : dataIn)
        b.name = unique(inNames, b.other.front().second); // named after the first inside input
    for (Boundary& b : dataOut)
        b.name = unique(outNames, b.key.second);

    // Build the scope.
    glm::vec2 lo(1e30f);
    for (std::uint32_t id : ids)
        lo = glm::min(lo, FindNode(id)->position);
    std::uint32_t inputsNode = 0, outputsNode = 0;
    if (macro) {
        AddMacro(name, lo - glm::vec2(320.0f, 0.0f));
        ScriptMacro* m = FindMacro(name);
        m->inputs.clear();
        m->outputs.clear();
        for (const auto* list : {&execIn, &dataIn})
            for (const Boundary& b : *list)
                m->inputs.push_back({b.name, b.type});
        for (const auto* list : {&execOut, &dataOut})
            for (const Boundary& b : *list)
                m->outputs.push_back({b.name, b.type});
        for (const ScriptNode& n : nodes) {
            if (n.function == name && n.type == "Macro.Inputs")
                inputsNode = n.id;
            if (n.function == name && n.type == "Macro.Outputs")
                outputsNode = n.id;
        }
    } else {
        AddFunction(name, lo - glm::vec2(320.0f, 0.0f));
        ScriptFunction* f = FindFunction(name);
        for (const Boundary& b : dataIn)
            f->inputs.push_back({b.name, b.type});
        for (const Boundary& b : dataOut)
            f->outputs.push_back({b.name, b.type});
        f->pure = execIn.empty() && execOut.empty() && !impure;
        for (const ScriptNode& n : nodes) {
            if (n.function == name && n.type == "Function.Entry")
                inputsNode = n.id;
            if (n.function == name && n.type == "Function.Return")
                outputsNode = n.id;
        }
        if (FindNode(outputsNode))
            FindNode(outputsNode)->position = glm::vec2(lo.x + 900.0f, lo.y);
    }
    // The default In -> Out / Entry -> Return link stays only for pure functions (their body).
    const bool keepFrameLink = !macro && execIn.empty() && execOut.empty() && !impure;
    if (!keepFrameLink)
        std::erase_if(links, [&](const ScriptLink& l) { return l.fromNode == inputsNode && l.toNode == outputsNode; });
    for (std::uint32_t id : ids)
        FindNode(id)->function = name;
    std::erase_if(links, [&](const ScriptLink& l) { return sel.contains(l.fromNode) != sel.contains(l.toNode); });

    // The call / Macro node outside.
    const std::uint32_t call = AddNode(macro ? "Macro.Use" : (FindFunction(name)->pure ? "Function.CallPure" : "Function.Call"),
                                       position, name, scope);
    const auto link = [&](std::uint32_t a, const std::string& ap, std::uint32_t b, const std::string& bp) {
        links.push_back({a, ap, b, bp});
    };
    for (const Boundary& b : execIn) { // outside exec -> call; tunnel -> inside target
        const std::string in = macro ? b.name : "In", tunnel = macro ? b.name : "Then";
        for (const PinKey& src : b.other)
            link(src.first, src.second, call, in);
        link(inputsNode, tunnel, b.key.first, b.key.second);
    }
    for (const Boundary& b : dataIn) { // outside source -> call; tunnel -> inside inputs
        link(b.key.first, b.key.second, call, b.name);
        for (const PinKey& target : b.other)
            link(inputsNode, b.name, target.first, target.second);
    }
    for (const Boundary& b : execOut) { // inside exec -> tunnel; call -> outside target
        link(b.key.first, b.key.second, outputsNode, macro ? b.name : "In");
        for (const PinKey& target : b.other)
            link(call, macro ? b.name : "Then", target.first, target.second);
    }
    for (const Boundary& b : dataOut) {
        link(b.key.first, b.key.second, outputsNode, b.name);
        for (const PinKey& target : b.other)
            link(call, b.name, target.first, target.second);
    }
    return {};
}

bool ScriptGraph::HasBreakpoint(std::uint32_t node) const { return std::ranges::find(breakpoints, node) != breakpoints.end(); }

void ScriptGraph::SetBreakpoint(std::uint32_t node, bool enabled)
{
    std::erase(breakpoints, node);
    if (enabled)
        breakpoints.push_back(node);
    else
        breakpointOptions.erase(node);
}

std::vector<PinInfo> NodePins(const ScriptGraph& graph, const ScriptNode& node)
{
    const NodeDesc* desc = FindScriptNodeType(node.type);
    if (!desc)
        return {};
    return desc->resolvePins ? desc->resolvePins(graph, node) : desc->pins;
}

std::optional<PinInfo> FindPin(const ScriptGraph& graph, const ScriptNode& node, std::string_view pin, bool output)
{
    for (PinInfo& p : NodePins(graph, node))
        if (p.output == output && p.name == pin)
            return std::move(p);
    return std::nullopt;
}

// --- JSON --------------------------------------------------------------------------------------

namespace {
json VariableToJson(const ScriptVariable& v)
{
    json j{{"name", v.name}, {"type", ToString(v.type)}, {"value", ScriptValueToJson(v.value)}};
    if (v.exposed)
        j["exposed"] = true;
    return j;
}

ScriptVariable VariableFromJson(const json& v)
{
    ScriptVariable var;
    var.name = v.at("name").get<std::string>();
    var.type = PinTypeFromString(v.value("type", std::string("float"))).value_or(PinType::Float);
    if (var.type.kind == PinKind::Exec)
        var.type = PinType::Float;
    var.value   = v.contains("value") ? ScriptValueFromJson(v["value"], var.type) : DefaultValue(var.type);
    var.exposed = v.value("exposed", false);
    return var;
}

json ParamsToJson(const std::vector<ScriptParam>& params)
{
    json list = json::array();
    for (const ScriptParam& p : params)
        list.push_back({{"name", p.name}, {"type", ToString(p.type)}});
    return list;
}

std::vector<ScriptParam> ParamsFromJson(const json& j, bool allowExec = false)
{
    std::vector<ScriptParam> params;
    for (const json& p : j) {
        ScriptParam param{p.at("name").get<std::string>(),
                          PinTypeFromString(p.value("type", std::string("float"))).value_or(PinType::Float)};
        if (param.type.kind == PinKind::Exec && !allowExec)
            param.type = PinType::Float;
        params.push_back(std::move(param));
    }
    return params;
}
} // namespace

std::string ScriptGraphToJson(const ScriptGraph& graph)
{
    json variables = json::array();
    for (const ScriptVariable& v : graph.variables)
        variables.push_back(VariableToJson(v));
    json functions = json::array();
    for (const ScriptFunction& f : graph.functions) {
        json locals = json::array();
        for (const ScriptVariable& v : f.locals)
            locals.push_back(VariableToJson(v));
        json j{{"name", f.name}, {"pure", f.pure}, {"inputs", ParamsToJson(f.inputs)}, {"outputs", ParamsToJson(f.outputs)},
               {"locals", std::move(locals)}};
        if (!f.description.empty())
            j["description"] = f.description;
        functions.push_back(std::move(j));
    }
    json nodes = json::array();
    for (const ScriptNode& n : graph.nodes) {
        json j{{"id", n.id}, {"type", n.type}, {"position", {n.position.x, n.position.y}}};
        if (!n.param.empty())
            j["param"] = n.param;
        if (!n.function.empty())
            j["function"] = n.function;
        if (!n.defaults.empty()) {
            json defaults = json::object();
            for (const auto& [pin, value] : n.defaults)
                defaults[pin] = {{"type", ToString(TypeOf(value))}, {"value", ScriptValueToJson(value)}};
            j["defaults"] = std::move(defaults);
        }
        nodes.push_back(std::move(j));
    }
    json links = json::array();
    for (const ScriptLink& l : graph.links)
        links.push_back({{"from", {l.fromNode, l.fromPin}}, {"to", {l.toNode, l.toPin}}});
    json comments = json::array();
    for (const ScriptComment& c : graph.comments) {
        json j{{"id", c.id},
               {"position", {c.position.x, c.position.y}},
               {"size", {c.size.x, c.size.y}},
               {"text", c.text},
               {"color", {c.color.r, c.color.g, c.color.b}}};
        if (!c.function.empty())
            j["function"] = c.function;
        comments.push_back(std::move(j));
    }
    json root{{"version", kGraphVersion}, {"nextId", graph.nextId}, {"variables", std::move(variables)},
              {"nodes", std::move(nodes)},  {"links", std::move(links)}, {"comments", std::move(comments)}};
    if (!graph.functions.empty())
        root["functions"] = std::move(functions);
    if (!graph.macros.empty()) {
        json macros = json::array();
        for (const ScriptMacro& m : graph.macros) {
            json j{{"name", m.name}, {"inputs", ParamsToJson(m.inputs)}, {"outputs", ParamsToJson(m.outputs)}};
            if (!m.description.empty())
                j["description"] = m.description;
            macros.push_back(std::move(j));
        }
        root["macros"] = std::move(macros);
    }
    const auto decls = [](const std::vector<ScriptEventDecl>& list) {
        json out = json::array();
        for (const ScriptEventDecl& d : list)
            out.push_back({{"name", d.name}, {"params", ParamsToJson(d.params)}});
        return out;
    };
    if (!graph.events.empty())
        root["events"] = decls(graph.events);
    if (!graph.dispatchers.empty())
        root["dispatchers"] = decls(graph.dispatchers);
    if (!graph.interfaces.empty())
        root["interfaces"] = graph.interfaces;
    if (!graph.timelines.empty()) {
        static constexpr const char* kKinds[]  = {"float", "vector", "event"};
        static constexpr const char* kInterp[] = {"linear", "constant", "smooth"};
        json timelines = json::array();
        for (const ScriptTimeline& t : graph.timelines) {
            json tracks = json::array();
            for (const ScriptTimelineTrack& track : t.tracks) {
                json keys = json::array();
                for (const ScriptTimelineKey& k : track.keys) {
                    json key{{"t", k.time}, {"i", kInterp[static_cast<int>(k.interp)]}};
                    if (track.kind == ScriptTrackKind::Vector)
                        key["v"] = {k.value.x, k.value.y, k.value.z};
                    else if (track.kind == ScriptTrackKind::Float)
                        key["v"] = k.value.x;
                    keys.push_back(std::move(key));
                }
                tracks.push_back({{"name", track.name}, {"kind", kKinds[static_cast<int>(track.kind)]}, {"keys", std::move(keys)}});
            }
            timelines.push_back({{"name", t.name}, {"length", t.length}, {"loop", t.loop}, {"autoPlay", t.autoPlay},
                                 {"tracks", std::move(tracks)}});
        }
        root["timelines"] = std::move(timelines);
    }
    if (graph.library)
        root["library"] = true;
    if (!graph.breakpoints.empty())
        root["breakpoints"] = graph.breakpoints;
    if (!graph.breakpointOptions.empty()) {
        json options = json::array();
        for (const auto& [node, o] : graph.breakpointOptions)
            options.push_back({{"node", node}, {"condition", o.condition}, {"hitCount", o.hitCount}});
        root["breakpointOptions"] = std::move(options);
    }
    return root.dump(2);
}

ScriptGraph ScriptGraphFromJson(const std::string& text)
{
    ScriptGraph graph;
    try {
        const json root    = json::parse(text);
        const int  version = root.value("version", 0);
        if (version < 1 || version > kGraphVersion)
            throw std::runtime_error("unsupported graph version " + std::to_string(version));
        for (const json& v : root.value("variables", json::array()))
            graph.variables.push_back(VariableFromJson(v));
        for (const json& f : root.value("functions", json::array())) {
            ScriptFunction fn;
            fn.name        = f.at("name").get<std::string>();
            fn.pure        = f.value("pure", false);
            fn.description = f.value("description", std::string());
            fn.inputs      = ParamsFromJson(f.value("inputs", json::array()));
            fn.outputs     = ParamsFromJson(f.value("outputs", json::array()));
            for (const json& v : f.value("locals", json::array()))
                fn.locals.push_back(VariableFromJson(v));
            graph.functions.push_back(std::move(fn));
        }
        for (const json& m : root.value("macros", json::array()))
            graph.macros.push_back({.name        = m.at("name").get<std::string>(),
                                    .inputs      = ParamsFromJson(m.value("inputs", json::array()), true),
                                    .outputs     = ParamsFromJson(m.value("outputs", json::array()), true),
                                    .description = m.value("description", std::string())});
        for (const char* key : {"events", "dispatchers"})
            for (const json& d : root.value(key, json::array()))
                (std::string_view(key) == "events" ? graph.events : graph.dispatchers)
                    .push_back({d.at("name").get<std::string>(), ParamsFromJson(d.value("params", json::array()))});
        for (const json& i : root.value("interfaces", json::array()))
            graph.interfaces.push_back(i.get<std::string>());
        graph.library = root.value("library", false);
        for (const json& t : root.value("timelines", json::array())) {
            ScriptTimeline timeline;
            timeline.name     = t.at("name").get<std::string>();
            timeline.length   = t.value("length", 1.0f);
            timeline.loop     = t.value("loop", false);
            timeline.autoPlay = t.value("autoPlay", false);
            for (const json& tr : t.value("tracks", json::array())) {
                ScriptTimelineTrack track;
                track.name              = tr.at("name").get<std::string>();
                const std::string kind  = tr.value("kind", std::string("float"));
                track.kind = kind == "vector" ? ScriptTrackKind::Vector : kind == "event" ? ScriptTrackKind::Event : ScriptTrackKind::Float;
                for (const json& k : tr.value("keys", json::array())) {
                    ScriptTimelineKey key;
                    key.time                = k.value("t", 0.0f);
                    const std::string interp = k.value("i", std::string("linear"));
                    key.interp = interp == "constant" ? ScriptInterp::Constant : interp == "smooth" ? ScriptInterp::Smooth : ScriptInterp::Linear;
                    if (const auto v = k.find("v"); v != k.end())
                        key.value = v->is_array() ? glm::vec3(v->at(0).get<float>(), v->at(1).get<float>(), v->at(2).get<float>())
                                                  : glm::vec3(v->get<float>(), 0.0f, 0.0f);
                    track.keys.push_back(key);
                }
                std::ranges::stable_sort(track.keys, {}, &ScriptTimelineKey::time);
                timeline.tracks.push_back(std::move(track));
            }
            graph.timelines.push_back(std::move(timeline));
        }
        std::uint32_t maxId = 0;
        for (const json& n : root.at("nodes")) {
            ScriptNode node;
            node.id   = n.at("id").get<std::uint32_t>();
            node.type = n.at("type").get<std::string>();
            if (const auto p = n.find("position"); p != n.end())
                node.position = {p->at(0).get<float>(), p->at(1).get<float>()};
            node.param    = n.value("param", std::string());
            node.function = n.value("function", std::string());
            if (const auto d = n.find("defaults"); d != n.end())
                for (auto it = d->begin(); it != d->end(); ++it) {
                    const PinType type =
                        PinTypeFromString(it->value("type", std::string("float"))).value_or(PinType::Float);
                    if (type.kind != PinKind::Exec && type != PinType::Entity)
                        node.defaults[it.key()] = ScriptValueFromJson(it->at("value"), type);
                }
            if (node.id == 0 || graph.FindNode(node.id))
                throw std::runtime_error("duplicate or invalid node id " + std::to_string(node.id));
            maxId = std::max(maxId, node.id);
            graph.nodes.push_back(std::move(node));
        }
        for (const json& l : root.value("links", json::array()))
            graph.links.push_back({.fromNode = l.at("from").at(0).get<std::uint32_t>(),
                                   .fromPin  = l.at("from").at(1).get<std::string>(),
                                   .toNode   = l.at("to").at(0).get<std::uint32_t>(),
                                   .toPin    = l.at("to").at(1).get<std::string>()});
        for (const json& c : root.value("comments", json::array())) {
            ScriptComment comment;
            comment.id       = c.at("id").get<std::uint32_t>();
            comment.position = {c.at("position").at(0).get<float>(), c.at("position").at(1).get<float>()};
            comment.size     = {c.at("size").at(0).get<float>(), c.at("size").at(1).get<float>()};
            comment.text     = c.value("text", std::string());
            comment.function = c.value("function", std::string());
            if (const auto col = c.find("color"); col != c.end())
                comment.color = {col->at(0).get<float>(), col->at(1).get<float>(), col->at(2).get<float>()};
            maxId = std::max(maxId, comment.id);
            graph.comments.push_back(std::move(comment));
        }
        for (const json& o : root.value("breakpointOptions", json::array()))
            graph.breakpointOptions[o.at("node").get<std::uint32_t>()] = {o.value("condition", std::string()),
                                                                          o.value("hitCount", 0u)};
        for (const json& b : root.value("breakpoints", json::array()))
            if (graph.FindNode(b.get<std::uint32_t>()))
                graph.breakpoints.push_back(b.get<std::uint32_t>());
        graph.nextId = std::max(root.value("nextId", 1u), maxId + 1);
    } catch (const json::exception& e) {
        throw std::runtime_error(e.what());
    }
    return graph;
}

void SaveScriptGraph(const std::filesystem::path& file, const ScriptGraph& graph)
{
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write '" + ToUtf8(temp) + "'");
        out << ScriptGraphToJson(graph) << '\n';
        if (!out)
            throw std::runtime_error("write failed: '" + ToUtf8(temp) + "'");
    }
    std::error_code ec;
    std::filesystem::rename(temp, file, ec);
    if (ec)
        throw std::runtime_error("cannot replace '" + ToUtf8(file) + "': " + ec.message());
}

ScriptGraph LoadScriptGraph(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read '" + ToUtf8(file) + "'");
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    try {
        return ScriptGraphFromJson(text);
    } catch (const std::exception& e) {
        throw std::runtime_error("'" + ToUtf8(file) + "': " + e.what());
    }
}

// --- Validation --------------------------------------------------------------------------------

namespace {

bool SameParams(const std::vector<ScriptParam>& a, const std::vector<ScriptParam>& b)
{
    return std::ranges::equal(a, b, [](const ScriptParam& x, const ScriptParam& y) { return x.name == y.name && x.type == y.type; });
}

// The graph and local name of a macro reference: a macro of `graph`, or "<Library>.<Macro>".
std::pair<const ScriptGraph*, std::string> ResolveMacro(const ScriptGraph& graph, const std::string& name)
{
    if (graph.FindMacro(name))
        return {&graph, name};
    const std::size_t dot = name.find('.');
    const ScriptLibrary* lib = dot == std::string::npos ? nullptr : ScriptRegistry::FindLibrary(name.substr(0, dot));
    if (lib && lib->graph.FindMacro(name.substr(dot + 1)))
        return {&lib->graph, name.substr(dot + 1)};
    return {nullptr, {}};
}

// Does the macro contain latent nodes (directly or through the macros it uses)?
bool MacroIsLatent(const ScriptGraph& graph, const std::string& name, int depth = 0)
{
    const auto [g, local] = ResolveMacro(graph, name);
    if (!g || depth > 32)
        return false;
    for (const ScriptNode& n : g->nodes) {
        if (n.function != local)
            continue;
        const NodeDesc* d = FindScriptNodeType(n.type);
        if ((d && d->latent) || (n.type == "Macro.Use" && MacroIsLatent(*g, n.param, depth + 1)))
            return true;
    }
    return false;
}

std::vector<ScriptDiagnostic> Validate(const ScriptGraph& graph, bool expanded)
{
    std::vector<ScriptDiagnostic> out;
    const auto error = [&](std::uint32_t node, std::string message) { out.push_back({node, std::move(message), true}); };

    const auto checkVariables = [&](const std::vector<ScriptVariable>& list, const std::string& where) {
        std::unordered_set<std::string> names;
        for (const ScriptVariable& v : list) {
            if (v.name.empty())
                error(0, "A variable" + where + " has no name");
            else if (!names.insert(v.name).second)
                error(0, "Variable '" + v.name + "'" + where + " is defined twice");
            if (!KnownType(v.type))
                error(0, "Variable '" + v.name + "'" + where + ": unknown type " + DisplayName(v.type));
            else if (!ValueFits(v.value, v.type))
                error(0, "Variable '" + v.name + "'" + where + " has a value of the wrong type");
        }
    };
    checkVariables(graph.variables, "");
    if (graph.library) {
        if (!graph.variables.empty() || !graph.events.empty() || !graph.dispatchers.empty() || !graph.interfaces.empty())
            error(0, "A library has only functions and macros (no variables, events, dispatchers or interfaces)");
        for (const ScriptNode& n : graph.nodes)
            if (n.function.empty())
                error(n.id, "A library has no event graph: nodes belong into functions or macros");
    }
    const auto checkParams = [&](const std::vector<ScriptParam>& params, const std::string& where, bool allowExec) {
        std::unordered_set<std::string> names;
        for (const ScriptParam& p : params) {
            if (!IsValidScriptName(p.name) || !names.insert(p.name).second)
                error(0, where + ": parameter names must be unique names ('" + p.name + "')");
            if (p.type.kind == PinKind::Exec ? !allowExec || IsContainer(p.type) : !KnownType(p.type))
                error(0, where + ", parameter '" + p.name + "': invalid type " + DisplayName(p.type));
        }
    };
    for (const auto* list : {&graph.events, &graph.dispatchers}) {
        const char*                     what = list == &graph.events ? "Event" : "Dispatcher";
        std::unordered_set<std::string> names;
        for (const ScriptEventDecl& e : *list) {
            if (!IsValidScriptName(e.name) || !names.insert(e.name).second)
                error(0, std::string(what) + " names must be unique names ('" + e.name + "')");
            checkParams(e.params, std::string(what) + " '" + e.name + "'", false);
        }
    }
    {
        std::unordered_set<std::string> seen;
        for (const std::string& name : graph.interfaces) {
            const ScriptInterface* iface = ScriptRegistry::FindInterface(name);
            if (!seen.insert(name).second)
                error(0, "Interface '" + name + "' is listed twice");
            if (!iface) {
                error(0, "Unknown interface '" + name + "'");
                continue;
            }
            for (const ScriptInterfaceFunction& f : iface->functions) {
                const ScriptFunction* impl = graph.FindFunction(f.name);
                if (!impl)
                    error(0, "Interface '" + name + "': function '" + f.name + "' is not implemented");
                else if (!SameParams(impl->inputs, f.inputs) || !SameParams(impl->outputs, f.outputs))
                    error(0, "Interface '" + name + "': the inputs / outputs of '" + f.name + "' differ from the interface");
            }
        }
    }
    {
        std::unordered_set<std::string> names;
        for (const ScriptTimeline& t : graph.timelines) {
            if (!IsValidScriptName(t.name) || !names.insert(t.name).second)
                error(0, "Timeline names must be unique names ('" + t.name + "')");
            if (!(t.length > 0.0f))
                error(0, "Timeline '" + t.name + "': the length must be > 0");
            std::unordered_set<std::string> tracks;
            for (const ScriptTimelineTrack& track : t.tracks) {
                static constexpr std::array<std::string_view, 4> kReserved{"Update", "Finished", "Reversed", "Time"};
                if (!IsValidScriptName(track.name) || !tracks.insert(track.name).second ||
                    std::ranges::find(kReserved, track.name) != kReserved.end())
                    error(0, "Timeline '" + t.name + "': track names must be unique names, not Update / Finished / Reversed / Time ('" +
                                 track.name + "')");
                if (!std::ranges::is_sorted(track.keys, {}, &ScriptTimelineKey::time))
                    error(0, "Timeline '" + t.name + "', track '" + track.name + "': keys are not sorted by time");
            }
        }
    }
    for (const ScriptMacro& m : graph.macros) {
        if (!IsValidScriptName(m.name) || graph.FindFunction(m.name) ||
            std::ranges::count(graph.macros, m.name, &ScriptMacro::name) > 1)
            error(0, "Macro '" + m.name + "': the name must be a unique name (functions and macros)");
        checkParams(m.inputs, "Macro '" + m.name + "'", true);
        checkParams(m.outputs, "Macro '" + m.name + "'", true);
        for (const char* type : {"Macro.Inputs", "Macro.Outputs"}) {
            const auto count = std::ranges::count_if(graph.nodes, [&](const ScriptNode& n) {
                return n.type == type && n.function == m.name;
            });
            if (count > 1 || (count == 0 && std::string_view(type) == "Macro.Inputs"))
                error(0, std::format("Macro '{}' needs {} {} node ({} found)", m.name,
                                     std::string_view(type) == "Macro.Inputs" ? "exactly one" : "at most one", type + 6, count));
        }
    }
    std::unordered_set<std::string> functionNames;
    for (const ScriptFunction& f : graph.functions) {
        if (!expanded && !IsValidScriptName(f.name)) // expanded: library functions are "<Library>.<Function>"
            error(0, "Invalid function name '" + f.name + "'");
        else if (!functionNames.insert(f.name).second)
            error(0, "Function '" + f.name + "' is defined twice");
        checkVariables(f.locals, " of function '" + f.name + "'");
        for (const auto* params : {&f.inputs, &f.outputs}) {
            std::unordered_set<std::string> names;
            for (const ScriptParam& p : *params) {
                if (p.name.empty() || !names.insert(p.name).second)
                    error(0, "Function '" + f.name + "': parameter names must be unique and not empty");
                if (!KnownType(p.type))
                    error(0, "Function '" + f.name + "', parameter '" + p.name + "': unknown type " + DisplayName(p.type));
            }
        }
        const auto entries = std::ranges::count_if(graph.nodes, [&](const ScriptNode& n) {
            return n.type == "Function.Entry" && n.function == f.name;
        });
        if (entries != 1)
            error(0, std::format("Function '{}' needs exactly one Entry node ({} found)", f.name, entries));
    }

    std::unordered_map<std::uint32_t, std::vector<PinInfo>> pins;
    for (const ScriptNode& n : graph.nodes) {
        const NodeDesc* desc = FindScriptNodeType(n.type);
        if (!desc) {
            error(n.id, "Unknown node type '" + n.type + "'");
            continue;
        }
        const ScriptFunction* scope = n.function.empty() ? nullptr : graph.FindFunction(n.function);
        const ScriptMacro*    macro = n.function.empty() || scope ? nullptr : graph.FindMacro(n.function);
        if (!n.function.empty() && !scope && !macro)
            error(n.id, "Unknown function / macro '" + n.function + "'");
        if (desc->paramKind == ParamKind::Variable) {
            const ScriptVariable* v = graph.FindVariableInScope(n.function, n.param);
            if (!v)
                error(n.id, "Unknown variable '" + n.param + "'");
            else if (desc->arrayVariable && !IsArray(v->type))
                error(n.id, "Variable '" + n.param + "' is not an array");
            else if (desc->mapVariable && !IsMap(v->type))
                error(n.id, "Variable '" + n.param + "' is not a map");
        }
        // Struct / enum parameters must name registered types.
        const std::size_t dot = n.param.find('.');
        switch (desc->paramKind) {
        case ParamKind::StructType:
            if (!ScriptRegistry::FindStruct(n.param))
                error(n.id, "Unknown struct '" + n.param + "'");
            break;
        case ParamKind::EnumType:
            if (!ScriptRegistry::FindEnum(n.param))
                error(n.id, "Unknown enum '" + n.param + "'");
            break;
        case ParamKind::EnumValue:
            if (dot == std::string::npos || ScriptRegistry::EnumValueIndex(n.param.substr(0, dot), n.param.substr(dot + 1)) < 0)
                error(n.id, "Unknown enum value '" + n.param + "'");
            break;
        case ParamKind::StructField: {
            const ScriptStructDef* def = dot == std::string::npos ? nullptr : ScriptRegistry::FindStruct(n.param.substr(0, dot));
            if (!def || std::ranges::none_of(def->fields, [&](const ScriptStructField& f) { return f.name == n.param.substr(dot + 1); }))
                error(n.id, "Unknown struct field '" + n.param + "'");
            break;
        }
        case ParamKind::Cases: {
            std::unordered_set<std::string> seen;
            std::size_t                     count = 0;
            for (std::size_t start = 0; start <= n.param.size();) {
                const std::size_t comma = std::min(n.param.find(',', start), n.param.size());
                std::string       c     = n.param.substr(start, comma - start);
                c.erase(0, c.find_first_not_of(' '));
                c.erase(c.find_last_not_of(' ') + 1);
                start = comma + 1;
                if (c.empty())
                    continue;
                ++count;
                if (!seen.insert(c).second || c == "Default")
                    error(n.id, "Case '" + c + "' is listed twice (or named Default)");
                if (n.type == "Flow.SwitchInt" && c.find_first_not_of("-0123456789") != std::string::npos)
                    error(n.id, "Case '" + c + "' is not a number");
            }
            if (count == 0)
                error(n.id, "No cases");
            break;
        }
        case ParamKind::PinType:
            if (const auto t = PinTypeFromString(n.param); !t || !KnownType(*t))
                error(n.id, "Unknown type '" + n.param + "'");
            break;
        case ParamKind::ElementType:
        case ParamKind::TypeAndCount:
            if (const auto t = PinTypeFromString(SplitTypeAndCount(n.param, 0).first); !t || !KnownType(*t))
                error(n.id, "Unknown type '" + n.param + "'");
            break;
        default: break;
        }
        if (desc->paramKind == ParamKind::Function) {
            const ScriptFunction* f = graph.FindFunction(n.param);
            if (!f)
                error(n.id, "Unknown function '" + n.param + "'");
            else if ((n.type == "Function.Entry" || n.type == "Function.Return") && n.param != n.function)
                error(n.id, "Entry / Return nodes belong into their own function");
            else if (n.type == "Function.Call" && f->pure)
                error(n.id, "'" + f->name + "' is pure: use a pure call");
            else if (n.type == "Function.CallPure" && !f->pure)
                error(n.id, "'" + f->name + "' is not pure: use a call with exec pins");
        }
        if (scope && desc->kind == NodeKind::Event && n.type != "Function.Entry")
            error(n.id, desc->title + " cannot be used inside a function");
        if (macro && desc->kind == NodeKind::Event && n.type != "Macro.Inputs")
            error(n.id, desc->title + " cannot be used inside a macro");
        if (scope && desc->latent)
            error(n.id, desc->title + " is latent: not allowed inside a function");
        switch (desc->paramKind) {
        case ParamKind::Macro:
            if (n.type != "Macro.Use") {
                if (n.param != n.function || !macro)
                    error(n.id, "Macro Inputs / Outputs nodes belong into their own macro");
            } else if (!ResolveMacro(graph, n.param).first) {
                error(n.id, "Unknown macro '" + n.param + "'");
            } else if (scope && MacroIsLatent(graph, n.param)) {
                error(n.id, "Macro '" + n.param + "' contains latent nodes: not allowed inside a function");
            }
            break;
        case ParamKind::LibraryFunction: {
            const ScriptFunction* f = ScriptRegistry::FindLibraryFunction(n.param);
            const std::vector<ScriptDiagnostic>* libErrors =
                f ? ScriptRegistry::LibraryDiagnostics(n.param.substr(0, n.param.find('.'))) : nullptr;
            if (!f)
                error(n.id, "Unknown library function '" + n.param + "'");
            else if ((n.type == "Library.CallPure") != f->pure)
                error(n.id, "'" + n.param + (f->pure ? "' is pure: use a pure call" : "' is not pure: use a call with exec pins"));
            else if (libErrors && std::ranges::any_of(*libErrors, &ScriptDiagnostic::error))
                error(n.id, "Library '" + n.param.substr(0, n.param.find('.')) + "' has errors");
            break;
        }
        case ParamKind::Interface:
            if (!ScriptRegistry::FindInterface(n.param))
                error(n.id, "Unknown interface '" + n.param + "'");
            break;
        case ParamKind::InterfaceFunction:
            if (!ScriptRegistry::FindInterfaceFunction(n.param))
                error(n.id, "Unknown interface function '" + n.param + "'");
            break;
        case ParamKind::Dispatcher:
            if (!graph.FindDispatcher(n.param))
                error(n.id, "Unknown dispatcher '" + n.param + "'");
            break;
        default: break;
        }
        if (desc->paramKind == ParamKind::Key && KeyFromName(n.param) < 0)
            error(n.id, "Unknown key '" + n.param + "'");
        if (desc->paramKind == ParamKind::Timeline && !graph.FindTimeline(n.param))
            error(n.id, "Unknown timeline '" + n.param + "'");
        if ((desc->paramKind == ParamKind::Text || desc->paramKind == ParamKind::InputAction ||
             desc->paramKind == ParamKind::InputAxis) && n.param.empty())
            error(n.id, desc->paramLabel + " is empty");
        pins[n.id] = NodePins(graph, n);
    }

    const auto pinOf = [&](std::uint32_t node, const std::string& name, bool output) -> const PinInfo* {
        const auto it = pins.find(node);
        if (it == pins.end())
            return nullptr;
        const auto p = std::ranges::find_if(it->second, [&](const PinInfo& x) { return x.output == output && x.name == name; });
        return p != it->second.end() ? &*p : nullptr;
    };
    std::map<std::pair<std::uint32_t, std::string>, int> execOut, dataIn;
    for (const ScriptLink& l : graph.links) {
        const PinInfo* from = pinOf(l.fromNode, l.fromPin, true);
        const PinInfo* to   = pinOf(l.toNode, l.toPin, false);
        const ScriptNode* fromNode = graph.FindNode(l.fromNode);
        const ScriptNode* toNode   = graph.FindNode(l.toNode);
        if (!fromNode || !toNode) {
            error(0, "A link refers to a missing node");
            continue;
        }
        if (fromNode->function != toNode->function) {
            error(l.toNode, "Link between different functions");
            continue;
        }
        if (!from || !to) {
            error(from ? l.toNode : l.fromNode,
                  "Link to missing pin '" + (from ? l.toPin : l.fromPin) + "'");
            continue;
        }
        if (!CanConvert(from->type, to->type))
            error(l.toNode, std::format("Pin '{}': {} is not compatible with {}", l.toPin, DisplayName(from->type),
                                        DisplayName(to->type)));
        if (from->type == PinType::Exec && ++execOut[{l.fromNode, l.fromPin}] == 2)
            error(l.fromNode, "Exec output '" + l.fromPin + "' has several links");
        if (to->type != PinType::Exec && ++dataIn[{l.toNode, l.toPin}] == 2)
            error(l.toNode, "Input '" + l.toPin + "' has several links");
    }

    // Pure nodes evaluate their inputs recursively: a cycle through pure nodes never ends.
    std::unordered_map<std::uint32_t, int> color; // 0 new, 1 on stack, 2 done
    const auto isPure = [&](std::uint32_t id) {
        const ScriptNode* n    = graph.FindNode(id);
        const NodeDesc*   desc = n ? FindScriptNodeType(n->type) : nullptr;
        return desc && desc->kind == NodeKind::Pure;
    };
    std::function<bool(std::uint32_t)> cyclic = [&](std::uint32_t id) {
        int& c = color[id];
        if (c == 1)
            return true;
        if (c == 2)
            return false;
        c = 1;
        for (const ScriptLink& l : graph.links)
            if (l.toNode == id && isPure(l.fromNode) && cyclic(l.fromNode))
                return true;
        color[id] = 2;
        return false;
    };
    for (const ScriptNode& n : graph.nodes)
        if (isPure(n.id) && color[n.id] == 0 && cyclic(n.id))
            error(n.id, "Cycle through pure nodes");

    // Functions calling each other in a cycle (no recursion: a function's nodes exist once).
    std::unordered_map<std::string, int> visit; // 0 new, 1 on stack, 2 done
    std::function<bool(const std::string&)> recursive = [&](const std::string& fn) {
        int& v = visit[fn];
        if (v == 1)
            return true;
        if (v == 2)
            return false;
        v = 1;
        for (const ScriptNode& n : graph.nodes)
            if (n.function == fn && (n.type == "Function.Call" || n.type == "Function.CallPure") &&
                graph.FindFunction(n.param) && recursive(n.param)) {
                error(n.id, "Recursive call of '" + n.param + "'");
                return true;
            }
        visit[fn] = 2;
        return false;
    };
    for (const ScriptFunction& f : graph.functions)
        if (visit[f.name] == 0)
            recursive(f.name);

    // Macros using themselves (their copies would never end).
    std::unordered_map<std::string, int> macroVisit;
    std::function<bool(const std::string&)> macroCycle = [&](const std::string& name) {
        int& v = macroVisit[name];
        if (v == 1)
            return true;
        if (v == 2)
            return false;
        v = 1;
        for (const ScriptNode& n : graph.nodes)
            if (n.function == name && n.type == "Macro.Use" && graph.FindMacro(n.param) && macroCycle(n.param)) {
                error(n.id, "Macro '" + n.param + "' uses itself");
                return true;
            }
        macroVisit[name] = 2;
        return false;
    };
    for (const ScriptMacro& m : graph.macros)
        if (macroVisit[m.name] == 0)
            macroCycle(m.name);

    if (expanded)
        return out;
    for (const ScriptNode& n : graph.nodes) {
        const NodeDesc* desc = FindScriptNodeType(n.type);
        if (desc && desc->kind == NodeKind::Event && n.type != "Macro.Inputs" &&
            std::ranges::none_of(graph.links, [&](const ScriptLink& l) { return l.fromNode == n.id; }))
            out.push_back({n.id, desc->title + ": nothing connected", false});
    }
    return out;
}

} // namespace

std::vector<ScriptDiagnostic> ValidateScriptGraph(const ScriptGraph& graph) { return Validate(graph, false); }
std::vector<ScriptDiagnostic> ValidateExpandedScriptGraph(const ScriptGraph& graph) { return Validate(graph, true); }

} // namespace Engine
