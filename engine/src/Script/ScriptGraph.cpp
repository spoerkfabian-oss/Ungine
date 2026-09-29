#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptNodes.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Engine {

using json = nlohmann::json;

namespace {
constexpr int kGraphVersion = 1;

constexpr const char* kTypeNames[] = {"exec", "bool", "int", "float", "vec3", "string", "entity"};

std::string ToUtf8(const std::filesystem::path& p)
{
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

json ValueToJson(const ScriptValue& value)
{
    return std::visit(
        [](const auto& v) -> json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, glm::vec3>)
                return json::array({v.x, v.y, v.z});
            else if constexpr (std::is_same_v<T, Entity>)
                return nullptr; // entities are runtime values (self when unconnected)
            else
                return v;
        },
        value);
}

ScriptValue ValueFromJson(const json& j, PinType type)
{
    ScriptValue value = DefaultValue(type);
    try {
        switch (type) {
        case PinType::Bool: value = j.get<bool>(); break;
        case PinType::Int: value = j.get<std::int32_t>(); break;
        case PinType::Float: value = j.get<float>(); break;
        case PinType::Vec3: value = glm::vec3(j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()); break;
        case PinType::String: value = j.get<std::string>(); break;
        default: break;
        }
    } catch (const json::exception&) { // wrong type in the file: keep the default
    }
    return value;
}

template <class Range, class Id>
auto FindById(Range& range, Id id)
{
    const auto it = std::ranges::find_if(range, [id](const auto& x) { return x.id == id; });
    return it != range.end() ? &*it : nullptr;
}
} // namespace

// --- Values -----------------------------------------------------------------------------------

const char* ToString(PinType type) { return kTypeNames[static_cast<std::size_t>(type)]; }

std::optional<PinType> PinTypeFromString(std::string_view name)
{
    for (std::size_t i = 0; i < std::size(kTypeNames); ++i)
        if (name == kTypeNames[i])
            return static_cast<PinType>(i);
    return std::nullopt;
}

PinType TypeOf(const ScriptValue& value) { return static_cast<PinType>(value.index() + 1); }

ScriptValue DefaultValue(PinType type)
{
    switch (type) {
    case PinType::Bool: return false;
    case PinType::Int: return std::int32_t{0};
    case PinType::Float: return 0.0f;
    case PinType::Vec3: return glm::vec3(0.0f);
    case PinType::String: return std::string();
    case PinType::Entity: return NullEntity;
    case PinType::Exec: break;
    }
    return false;
}

bool CanConvert(PinType from, PinType to)
{
    if (from == to)
        return true;
    if (from == PinType::Exec || to == PinType::Exec)
        return false;
    if (to == PinType::String)
        return true;
    const bool numeric = [](PinType t) { return t == PinType::Int || t == PinType::Float; }(to);
    return numeric && (from == PinType::Int || from == PinType::Float || from == PinType::Bool);
}

std::string ToDisplayString(const ScriptValue& value)
{
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::int32_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, float>)
                return std::format("{:g}", v);
            else if constexpr (std::is_same_v<T, glm::vec3>)
                return std::format("({:g}, {:g}, {:g})", v.x, v.y, v.z);
            else if constexpr (std::is_same_v<T, std::string>)
                return v;
            else
                return v == NullEntity ? std::string("none")
                                       : std::format("entity {}:{}", EntityIndex(v), EntityGeneration(v));
        },
        value);
}

ScriptValue Convert(const ScriptValue& value, PinType to)
{
    const PinType from = TypeOf(value);
    if (from == to)
        return value;
    if (to == PinType::String)
        return ToDisplayString(value);
    if (to == PinType::Float) {
        if (const auto* i = std::get_if<std::int32_t>(&value))
            return static_cast<float>(*i);
        if (const auto* b = std::get_if<bool>(&value))
            return *b ? 1.0f : 0.0f;
    }
    if (to == PinType::Int) {
        if (const auto* f = std::get_if<float>(&value))
            return std::isfinite(*f) ? static_cast<std::int32_t>(std::clamp(*f, -2147483520.0f, 2147483520.0f))
                                     : std::int32_t{0};
        if (const auto* b = std::get_if<bool>(&value))
            return std::int32_t{*b ? 1 : 0};
    }
    return DefaultValue(to);
}

// --- Graph editing -----------------------------------------------------------------------------

ScriptNode*           ScriptGraph::FindNode(std::uint32_t id) { return FindById(nodes, id); }
const ScriptNode*     ScriptGraph::FindNode(std::uint32_t id) const { return FindById(nodes, id); }
ScriptComment*        ScriptGraph::FindComment(std::uint32_t id) { return FindById(comments, id); }

ScriptVariable* ScriptGraph::FindVariable(std::string_view name)
{
    const auto it = std::ranges::find_if(variables, [&](const ScriptVariable& v) { return v.name == name; });
    return it != variables.end() ? &*it : nullptr;
}

const ScriptVariable* ScriptGraph::FindVariable(std::string_view name) const
{
    const auto it = std::ranges::find_if(variables, [&](const ScriptVariable& v) { return v.name == name; });
    return it != variables.end() ? &*it : nullptr;
}

std::uint32_t ScriptGraph::AddNode(const std::string& type, glm::vec2 position, std::string param)
{
    if (param.empty())
        if (const NodeDesc* desc = FindScriptNodeType(type))
            param = desc->paramDefault;
    const std::uint32_t id = nextId++;
    nodes.push_back({.id = id, .type = type, .position = position, .param = std::move(param), .defaults = {}});
    return id;
}

void ScriptGraph::RemoveNode(std::uint32_t id)
{
    std::erase_if(nodes, [id](const ScriptNode& n) { return n.id == id; });
    std::erase_if(links, [id](const ScriptLink& l) { return l.fromNode == id || l.toNode == id; });
}

std::uint32_t ScriptGraph::AddComment(glm::vec2 position, glm::vec2 size, std::string text)
{
    const std::uint32_t id = nextId++;
    comments.push_back({.id = id, .position = position, .size = size, .text = std::move(text), .color = {0.3f, 0.45f, 0.7f}});
    return id;
}

void ScriptGraph::RemoveComment(std::uint32_t id)
{
    std::erase_if(comments, [id](const ScriptComment& c) { return c.id == id; });
}

std::string ScriptGraph::Connect(std::uint32_t from, const std::string& fromPin, std::uint32_t to, const std::string& toPin)
{
    const ScriptNode* a = FindNode(from);
    const ScriptNode* b = FindNode(to);
    if (!a || !b)
        return "Unknown node";
    if (from == to)
        return "Cannot connect a node to itself";
    const std::optional<PinInfo> outA = FindPin(*this, *a, fromPin, true);
    const std::optional<PinInfo> inB  = FindPin(*this, *b, toPin, false);
    if (!outA || !inB)
        return "Connect an output to an input";
    if (!CanConvert(outA->type, inB->type))
        return std::format("{} is not compatible with {}", ToString(outA->type), ToString(inB->type));

    const ScriptLink link{.fromNode = from, .fromPin = fromPin, .toNode = to, .toPin = toPin};
    if (std::ranges::find(links, link) != links.end())
        return {};
    if (outA->type == PinType::Exec) // one target per exec output
        std::erase_if(links, [&](const ScriptLink& l) { return l.fromNode == from && l.fromPin == fromPin; });
    else // one source per data input
        std::erase_if(links, [&](const ScriptLink& l) { return l.toNode == to && l.toPin == toPin; });
    links.push_back(link);
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
        if (!a || !b)
            return true;
        const auto out = FindPin(*this, *a, l.fromPin, true);
        const auto in  = FindPin(*this, *b, l.toPin, false);
        return !out || !in || !CanConvert(out->type, in->type);
    });
}

bool ScriptGraph::RenameVariable(const std::string& from, const std::string& to)
{
    ScriptVariable* v = FindVariable(from);
    if (!v || to.empty() || FindVariable(to))
        return false;
    v->name = to;
    for (ScriptNode& n : nodes)
        if ((n.type == "Variable.Get" || n.type == "Variable.Set") && n.param == from)
            n.param = to;
    return true;
}

void ScriptGraph::SetVariableType(const std::string& name, PinType type)
{
    ScriptVariable* v = FindVariable(name);
    if (!v || v->type == type || type == PinType::Exec)
        return;
    v->type  = type;
    v->value = DefaultValue(type);
    for (ScriptNode& n : nodes)
        if (n.type == "Variable.Set" && n.param == name)
            n.defaults.erase("Value");
    RemoveDanglingLinks();
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

std::string ScriptGraphToJson(const ScriptGraph& graph)
{
    json variables = json::array();
    for (const ScriptVariable& v : graph.variables)
        variables.push_back({{"name", v.name}, {"type", ToString(v.type)}, {"value", ValueToJson(v.value)}});
    json nodes = json::array();
    for (const ScriptNode& n : graph.nodes) {
        json j{{"id", n.id}, {"type", n.type}, {"position", {n.position.x, n.position.y}}};
        if (!n.param.empty())
            j["param"] = n.param;
        if (!n.defaults.empty()) {
            json defaults = json::object();
            for (const auto& [pin, value] : n.defaults)
                defaults[pin] = {{"type", ToString(TypeOf(value))}, {"value", ValueToJson(value)}};
            j["defaults"] = std::move(defaults);
        }
        nodes.push_back(std::move(j));
    }
    json links = json::array();
    for (const ScriptLink& l : graph.links)
        links.push_back({{"from", {l.fromNode, l.fromPin}}, {"to", {l.toNode, l.toPin}}});
    json comments = json::array();
    for (const ScriptComment& c : graph.comments)
        comments.push_back({{"id", c.id},
                            {"position", {c.position.x, c.position.y}},
                            {"size", {c.size.x, c.size.y}},
                            {"text", c.text},
                            {"color", {c.color.r, c.color.g, c.color.b}}});
    const json root{{"version", kGraphVersion}, {"nextId", graph.nextId}, {"variables", std::move(variables)},
                    {"nodes", std::move(nodes)},  {"links", std::move(links)}, {"comments", std::move(comments)}};
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
        for (const json& v : root.value("variables", json::array())) {
            ScriptVariable var;
            var.name  = v.at("name").get<std::string>();
            var.type  = PinTypeFromString(v.value("type", std::string("float"))).value_or(PinType::Float);
            if (var.type == PinType::Exec)
                var.type = PinType::Float;
            var.value = v.contains("value") ? ValueFromJson(v["value"], var.type) : DefaultValue(var.type);
            graph.variables.push_back(std::move(var));
        }
        std::uint32_t maxId = 0;
        for (const json& n : root.at("nodes")) {
            ScriptNode node;
            node.id   = n.at("id").get<std::uint32_t>();
            node.type = n.at("type").get<std::string>();
            if (const auto p = n.find("position"); p != n.end())
                node.position = {p->at(0).get<float>(), p->at(1).get<float>()};
            node.param = n.value("param", std::string());
            if (const auto d = n.find("defaults"); d != n.end())
                for (auto it = d->begin(); it != d->end(); ++it) {
                    const PinType type =
                        PinTypeFromString(it->value("type", std::string("float"))).value_or(PinType::Float);
                    if (type != PinType::Exec && type != PinType::Entity)
                        node.defaults[it.key()] = ValueFromJson(it->at("value"), type);
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
            if (const auto col = c.find("color"); col != c.end())
                comment.color = {col->at(0).get<float>(), col->at(1).get<float>(), col->at(2).get<float>()};
            maxId = std::max(maxId, comment.id);
            graph.comments.push_back(std::move(comment));
        }
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

std::vector<ScriptDiagnostic> ValidateScriptGraph(const ScriptGraph& graph)
{
    std::vector<ScriptDiagnostic> out;
    const auto error = [&](std::uint32_t node, std::string message) { out.push_back({node, std::move(message), true}); };

    std::unordered_set<std::string> names;
    for (const ScriptVariable& v : graph.variables) {
        if (v.name.empty())
            error(0, "A variable has no name");
        else if (!names.insert(v.name).second)
            error(0, "Variable '" + v.name + "' is defined twice");
    }

    std::unordered_map<std::uint32_t, std::vector<PinInfo>> pins;
    for (const ScriptNode& n : graph.nodes) {
        const NodeDesc* desc = FindScriptNodeType(n.type);
        if (!desc) {
            error(n.id, "Unknown node type '" + n.type + "'");
            continue;
        }
        if (desc->paramKind == ParamKind::Variable && !graph.FindVariable(n.param))
            error(n.id, "Unknown variable '" + n.param + "'");
        if (desc->paramKind == ParamKind::Key && KeyFromName(n.param) < 0)
            error(n.id, "Unknown key '" + n.param + "'");
        if ((desc->paramKind == ParamKind::Text) && n.param.empty())
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
        if (!graph.FindNode(l.fromNode) || !graph.FindNode(l.toNode)) {
            error(0, "A link refers to a missing node");
            continue;
        }
        if (!from || !to) {
            error(from ? l.toNode : l.fromNode,
                  "Link to missing pin '" + (from ? l.toPin : l.fromPin) + "'");
            continue;
        }
        if (!CanConvert(from->type, to->type))
            error(l.toNode, std::format("Pin '{}': {} is not compatible with {}", l.toPin, ToString(from->type),
                                        ToString(to->type)));
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

    for (const ScriptNode& n : graph.nodes) {
        const NodeDesc* desc = FindScriptNodeType(n.type);
        if (desc && desc->kind == NodeKind::Event &&
            std::ranges::none_of(graph.links, [&](const ScriptLink& l) { return l.fromNode == n.id; }))
            out.push_back({n.id, desc->title + ": nothing connected", false});
    }
    return out;
}

} // namespace Engine
