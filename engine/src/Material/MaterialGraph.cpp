#include "Engine/Material/MaterialGraph.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <system_error>

namespace Engine {

namespace fs = std::filesystem;
using json   = nlohmann::json;

const char* ToString(MaterialBlendMode mode)
{
    switch (mode) {
    case MaterialBlendMode::Opaque: return "Opaque";
    case MaterialBlendMode::Masked: return "Masked";
    case MaterialBlendMode::Translucent: return "Translucent";
    case MaterialBlendMode::Additive: return "Additive";
    }
    return "Opaque";
}

const char* ToString(MaterialShadingModel model)
{
    return model == MaterialShadingModel::Unlit ? "Unlit" : "Lit";
}

std::optional<MaterialBlendMode> MaterialBlendModeFromString(std::string_view text)
{
    for (const MaterialBlendMode mode : {MaterialBlendMode::Opaque, MaterialBlendMode::Masked,
                                         MaterialBlendMode::Translucent, MaterialBlendMode::Additive})
        if (text == ToString(mode))
            return mode;
    return std::nullopt;
}

std::optional<MaterialShadingModel> MaterialShadingModelFromString(std::string_view text)
{
    if (text == "Lit")
        return MaterialShadingModel::Lit;
    if (text == "Unlit")
        return MaterialShadingModel::Unlit;
    return std::nullopt;
}

const char* ToString(MaterialParameterType type)
{
    switch (type) {
    case MaterialParameterType::Scalar: return "Scalar";
    case MaterialParameterType::Vector: return "Vector";
    case MaterialParameterType::Texture: return "Texture";
    }
    return "Scalar";
}

namespace {

std::optional<MaterialParameterType> ParameterTypeFromString(std::string_view text)
{
    for (const MaterialParameterType type :
         {MaterialParameterType::Scalar, MaterialParameterType::Vector, MaterialParameterType::Texture})
        if (text == ToString(type))
            return type;
    return std::nullopt;
}

TextureKind TextureKindFromString(std::string_view text)
{
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(TextureKind::Count); ++i)
        if (text == ToString(static_cast<TextureKind>(i)))
            return static_cast<TextureKind>(i);
    return TextureKind::Color;
}

// Relative to the material file (also "../Textures/x.png"); absolute only without a relative path.
std::string RelativePath(const std::string& path, const fs::path& folder)
{
    if (path.empty() || folder.empty())
        return path;
    const fs::path absolute = PathFromUtf8(path).lexically_normal();
    const fs::path relative = absolute.lexically_relative(folder);
    if (!absolute.is_absolute() || relative.empty())
        return path;
    const std::u8string generic = relative.generic_u8string();
    return std::string(generic.begin(), generic.end());
}

std::string AbsolutePath(const std::string& path, const fs::path& folder)
{
    if (path.empty())
        return path;
    const fs::path p = PathFromUtf8(path);
    return PathToUtf8((p.is_absolute() || folder.empty() ? p : folder / p).lexically_normal());
}

json Vec(glm::vec4 v) { return json::array({v.x, v.y, v.z, v.w}); }
json Vec(glm::vec2 v) { return json::array({v.x, v.y}); }

glm::vec4 ReadVec4(const json& j, glm::vec4 fallback)
{
    if (!j.is_array())
        return fallback;
    for (std::size_t i = 0; i < 4 && i < j.size(); ++i)
        if (j[i].is_number()) {
            const float v = j[i].get<float>();
            fallback[static_cast<int>(i)] = std::isfinite(v) ? v : 0.0f;
        }
    return fallback;
}

glm::vec2 ReadVec2(const json& j, glm::vec2 fallback)
{
    const glm::vec4 v = ReadVec4(j, glm::vec4(fallback, 0.0f, 0.0f));
    return {v.x, v.y};
}

bool UsesTexturePath(const MaterialNode& node)
{
    const MaterialNodeDesc* desc = FindMaterialNodeType(node.type);
    return desc && desc->param == MaterialNodeParam::Texture;
}

bool WriteText(const fs::path& file, const std::string& text, std::string* error)
{
    std::error_code ec;
    if (file.has_parent_path())
        fs::create_directories(file.parent_path(), ec);
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text << '\n';
        if (!out) {
            if (error)
                *error = "cannot write '" + PathToUtf8(temp) + "'";
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec && error)
        *error = "cannot write '" + PathToUtf8(file) + "': " + ec.message();
    return !ec;
}

json ParameterValueJson(const MaterialParameterValue& v, const fs::path& folder)
{
    json j{{"name", v.name}, {"type", ToString(v.type)}};
    if (v.type == MaterialParameterType::Texture)
        j["texture"] = RelativePath(v.texture, folder);
    else
        j["value"] = Vec(v.value);
    return j;
}

MaterialParameterValue ReadParameterValue(const json& j, const fs::path& folder)
{
    MaterialParameterValue v;
    v.name = j.value("name", std::string{});
    v.type = ParameterTypeFromString(j.value("type", std::string{"Scalar"})).value_or(MaterialParameterType::Scalar);
    v.value   = ReadVec4(j.value("value", json{}), glm::vec4(0.0f));
    v.texture = AbsolutePath(j.value("texture", std::string{}), folder);
    return v;
}

} // namespace

// --- MaterialGraph ---

MaterialNode* MaterialGraph::FindNode(std::uint32_t id)
{
    for (MaterialNode& node : nodes)
        if (node.id == id)
            return &node;
    return nullptr;
}

const MaterialNode* MaterialGraph::FindNode(std::uint32_t id) const
{
    return const_cast<MaterialGraph*>(this)->FindNode(id);
}

const MaterialNode* MaterialGraph::OutputNode() const
{
    for (const MaterialNode& node : nodes)
        if (node.type == kMaterialOutputNode)
            return &node;
    return nullptr;
}

MaterialParameter* MaterialGraph::FindParameter(std::string_view name)
{
    for (MaterialParameter& p : parameters)
        if (p.name == name)
            return &p;
    return nullptr;
}

const MaterialParameter* MaterialGraph::FindParameter(std::string_view name) const
{
    return const_cast<MaterialGraph*>(this)->FindParameter(name);
}

const MaterialLink* MaterialGraph::LinkInto(std::uint32_t node, std::string_view pin) const
{
    for (const MaterialLink& link : links)
        if (link.toNode == node && link.toPin == pin)
            return &link;
    return nullptr;
}

std::uint32_t MaterialGraph::AddNode(std::string type, glm::vec2 position, std::string param)
{
    MaterialNode node;
    node.id       = nextId++;
    node.type     = std::move(type);
    node.position = position;
    node.param    = std::move(param);
    if (node.type == "Input.TexCoord")
        node.value = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    else if (node.type == "Color")
        node.value = glm::vec4(1.0f);
    else if (node.type == "Math.Mask" && node.param.empty())
        node.param = "rgb";
    nodes.push_back(std::move(node));
    return nodes.back().id;
}

void MaterialGraph::RemoveNode(std::uint32_t id)
{
    const MaterialNode* node = FindNode(id);
    if (!node || node->type == kMaterialOutputNode)
        return;
    std::erase_if(links, [&](const MaterialLink& l) { return l.fromNode == id || l.toNode == id; });
    std::erase_if(nodes, [&](const MaterialNode& n) { return n.id == id; });
}

bool MaterialGraph::Connect(std::uint32_t fromNode, std::string_view fromPin, std::uint32_t toNode, std::string_view toPin)
{
    if (fromNode == toNode)
        return false;
    const MaterialNode* from = FindNode(fromNode);
    const MaterialNode* to   = FindNode(toNode);
    if (!from || !to)
        return false;
    const MaterialNodeDesc* fromDesc = FindMaterialNodeType(from->type);
    const MaterialNodeDesc* toDesc   = FindMaterialNodeType(to->type);
    if (!fromDesc || !toDesc)
        return false;
    const auto has = [](const std::vector<MaterialPinDesc>& pins, std::string_view name) {
        return std::any_of(pins.begin(), pins.end(), [&](const MaterialPinDesc& p) { return p.name == name; });
    };
    if (!has(fromDesc->outputs, fromPin) || !has(toDesc->inputs, toPin))
        return false;
    Disconnect(toNode, toPin);
    links.push_back({fromNode, std::string(fromPin), toNode, std::string(toPin)});
    return true;
}

void MaterialGraph::Disconnect(std::uint32_t toNode, std::string_view toPin)
{
    std::erase_if(links, [&](const MaterialLink& l) { return l.toNode == toNode && l.toPin == toPin; });
}

bool MaterialGraph::RenameParameter(std::string_view from, std::string_view to)
{
    if (to.empty() || FindParameter(to))
        return false;
    MaterialParameter* p = FindParameter(from);
    if (!p)
        return false;
    for (MaterialNode& node : nodes) {
        const MaterialNodeDesc* desc = FindMaterialNodeType(node.type);
        if (desc && desc->param == MaterialNodeParam::Parameter && node.param == from)
            node.param = std::string(to);
    }
    p->name = std::string(to);
    return true;
}

glm::vec4 MaterialGraph::InputValue(const MaterialNode& node, std::string_view pin, glm::vec4 fallback) const
{
    for (const MaterialPinValue& v : node.inputs)
        if (v.pin == pin)
            return v.value;
    return fallback;
}

void MaterialGraph::SetInputValue(MaterialNode& node, std::string_view pin, glm::vec4 value)
{
    for (MaterialPinValue& v : node.inputs)
        if (v.pin == pin) {
            v.value = value;
            return;
        }
    node.inputs.push_back({std::string(pin), value});
}

MaterialGraph MakeDefaultMaterialGraph()
{
    MaterialGraph graph;
    MaterialParameter color;
    color.name  = "BaseColor";
    color.type  = MaterialParameterType::Vector;
    color.value = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    MaterialParameter roughness;
    roughness.name  = "Roughness";
    roughness.value = glm::vec4(0.5f, 0.0f, 0.0f, 0.0f);
    MaterialParameter metallic;
    metallic.name = "Metallic";
    graph.parameters = {color, roughness, metallic};

    const std::uint32_t output = graph.AddNode(kMaterialOutputNode, {420.0f, 0.0f});
    const std::uint32_t c      = graph.AddNode("Param.Vector", {80.0f, -40.0f}, "BaseColor");
    const std::uint32_t r      = graph.AddNode("Param.Scalar", {80.0f, 120.0f}, "Roughness");
    const std::uint32_t m      = graph.AddNode("Param.Scalar", {80.0f, 60.0f}, "Metallic");
    graph.Connect(c, "RGB", output, "BaseColor");
    graph.Connect(m, "Value", output, "Metallic");
    graph.Connect(r, "Value", output, "Roughness");
    return graph;
}

// --- Files ---

std::string MaterialGraphToJson(const MaterialGraph& graph, const fs::path& folder)
{
    json root;
    root["version"] = 1;
    const MaterialSettings& s = graph.settings;
    root["settings"] = {{"blendMode", ToString(s.blendMode)},
                        {"shadingModel", ToString(s.shadingModel)},
                        {"twoSided", s.twoSided},
                        {"castShadows", s.castShadows},
                        {"alphaCutoff", s.alphaCutoff},
                        {"maxDisplacement", s.maxDisplacement}};
    json parameters = json::array();
    for (const MaterialParameter& p : graph.parameters) {
        json j{{"name", p.name}, {"type", ToString(p.type)}};
        if (p.type == MaterialParameterType::Texture) {
            j["texture"]     = RelativePath(p.texture, folder);
            j["textureKind"] = ToString(p.textureKind);
        } else {
            j["value"] = Vec(p.value);
        }
        if (!p.group.empty())
            j["group"] = p.group;
        if (p.type == MaterialParameterType::Scalar)
            j["range"] = Vec(p.range);
        parameters.push_back(std::move(j));
    }
    root["parameters"] = std::move(parameters);
    json nodes = json::array();
    for (const MaterialNode& n : graph.nodes) {
        json j{{"id", n.id}, {"type", n.type}, {"position", Vec(n.position)}};
        if (!n.param.empty())
            j["param"] = UsesTexturePath(n) ? RelativePath(n.param, folder) : n.param;
        if (n.value != glm::vec4(0.0f))
            j["value"] = Vec(n.value);
        if (UsesTexturePath(n))
            j["textureKind"] = ToString(n.textureKind);
        if (!n.inputs.empty()) {
            json inputs = json::object();
            for (const MaterialPinValue& v : n.inputs)
                inputs[v.pin] = Vec(v.value);
            j["inputs"] = std::move(inputs);
        }
        nodes.push_back(std::move(j));
    }
    root["nodes"] = std::move(nodes);
    json links = json::array();
    for (const MaterialLink& l : graph.links)
        links.push_back({{"from", {l.fromNode, l.fromPin}}, {"to", {l.toNode, l.toPin}}});
    root["links"] = std::move(links);
    if (!graph.comments.empty()) {
        json comments = json::array();
        for (const MaterialComment& c : graph.comments)
            comments.push_back({{"id", c.id}, {"text", c.text}, {"position", Vec(c.position)}, {"size", Vec(c.size)},
                                {"color", Vec(c.color)}});
        root["comments"] = std::move(comments);
    }
    root["nextId"] = graph.nextId;
    return root.dump(2);
}

std::optional<MaterialGraph> ParseMaterialGraph(std::string_view text, const fs::path& folder, std::string* error)
{
    try {
        const json    root = json::parse(text);
        MaterialGraph graph;
        if (const auto it = root.find("settings"); it != root.end() && it->is_object()) {
            MaterialSettings& s = graph.settings;
            s.blendMode = MaterialBlendModeFromString(it->value("blendMode", std::string{"Opaque"})).value_or(s.blendMode);
            s.shadingModel =
                MaterialShadingModelFromString(it->value("shadingModel", std::string{"Lit"})).value_or(s.shadingModel);
            s.twoSided        = it->value("twoSided", s.twoSided);
            s.castShadows     = it->value("castShadows", s.castShadows);
            s.alphaCutoff     = it->value("alphaCutoff", s.alphaCutoff);
            s.maxDisplacement = it->value("maxDisplacement", s.maxDisplacement);
            if (!std::isfinite(s.alphaCutoff))
                s.alphaCutoff = 0.5f;
            if (!std::isfinite(s.maxDisplacement))
                s.maxDisplacement = 0.0f;
            s.alphaCutoff     = std::clamp(s.alphaCutoff, 0.0f, 1.0f);
            s.maxDisplacement = std::clamp(s.maxDisplacement, 0.0f, 1000.0f);
        }
        std::set<std::string> names;
        for (const json& j : root.value("parameters", json::array())) {
            MaterialParameter p;
            p.name = j.value("name", std::string{"Param"});
            if (p.name.empty())
                p.name = "Param";
            for (int suffix = 2; names.contains(p.name); ++suffix)
                p.name = j.value("name", std::string{"Param"}) + std::to_string(suffix);
            names.insert(p.name);
            p.type = ParameterTypeFromString(j.value("type", std::string{"Scalar"})).value_or(MaterialParameterType::Scalar);
            p.value       = ReadVec4(j.value("value", json{}), p.value);
            p.texture     = AbsolutePath(j.value("texture", std::string{}), folder);
            p.textureKind = TextureKindFromString(j.value("textureKind", std::string{"Color"}));
            p.group       = j.value("group", std::string{});
            p.range       = ReadVec2(j.value("range", json{}), p.range);
            graph.parameters.push_back(std::move(p));
        }
        std::set<std::uint32_t> ids;
        std::uint32_t           maxId = 0;
        for (const json& j : root.value("nodes", json::array())) {
            MaterialNode n;
            n.id = j.value("id", 0u);
            if (n.id == 0 || ids.contains(n.id))
                continue;
            ids.insert(n.id);
            maxId      = std::max(maxId, n.id);
            n.type     = j.value("type", std::string{});
            n.position = ReadVec2(j.value("position", json{}), n.position);
            n.param    = j.value("param", std::string{});
            n.value    = ReadVec4(j.value("value", json{}), n.value);
            n.textureKind = TextureKindFromString(j.value("textureKind", std::string{"Color"}));
            if (UsesTexturePath(n))
                n.param = AbsolutePath(n.param, folder);
            if (const auto inputs = j.find("inputs"); inputs != j.end() && inputs->is_object())
                for (const auto& [pin, value] : inputs->items())
                    n.inputs.push_back({pin, ReadVec4(value, glm::vec4(0.0f))});
            if (n.type == kMaterialOutputNode && graph.OutputNode())
                continue; // one output node
            graph.nodes.push_back(std::move(n));
        }
        for (const json& j : root.value("links", json::array())) {
            const json& from = j.value("from", json::array());
            const json& to   = j.value("to", json::array());
            if (from.size() != 2 || to.size() != 2)
                continue;
            MaterialLink l{from[0].get<std::uint32_t>(), from[1].get<std::string>(), to[0].get<std::uint32_t>(),
                           to[1].get<std::string>()};
            if (!graph.FindNode(l.fromNode) || !graph.FindNode(l.toNode) || graph.LinkInto(l.toNode, l.toPin))
                continue;
            graph.links.push_back(std::move(l));
        }
        for (const json& j : root.value("comments", json::array())) {
            MaterialComment c;
            c.id = j.value("id", 0u);
            if (c.id == 0 || ids.contains(c.id))
                continue;
            ids.insert(c.id);
            maxId      = std::max(maxId, c.id);
            c.text     = j.value("text", c.text);
            c.position = ReadVec2(j.value("position", json{}), c.position);
            c.size     = ReadVec2(j.value("size", json{}), c.size);
            c.color    = ReadVec4(j.value("color", json{}), c.color);
            graph.comments.push_back(std::move(c));
        }
        graph.nextId = std::max(root.value("nextId", 1u), maxId + 1);
        if (!graph.OutputNode())
            graph.AddNode(kMaterialOutputNode, {420.0f, 0.0f});
        return graph;
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
        return std::nullopt;
    }
}

std::optional<MaterialGraph> LoadMaterialGraph(const fs::path& file, std::string* error)
{
    const std::optional<std::string> text = Vfs::ReadText(file);
    if (!text) {
        if (error)
            *error = "cannot read '" + PathToUtf8(file) + "'";
        return std::nullopt;
    }
    std::string                  why;
    std::optional<MaterialGraph> graph = ParseMaterialGraph(*text, file.parent_path(), &why);
    if (!graph && error)
        *error = "'" + PathToUtf8(file) + "': " + why;
    return graph;
}

bool SaveMaterialGraph(const fs::path& file, const MaterialGraph& graph, std::string* error)
{
    return WriteText(file, MaterialGraphToJson(graph, file.parent_path()), error);
}

std::string MaterialInstanceToJson(const MaterialInstanceFile& instance, const fs::path& folder)
{
    json root{{"version", 1}, {"parent", RelativePath(instance.parent, folder)}};
    json overrides = json::array();
    for (const MaterialParameterValue& v : instance.overrides)
        overrides.push_back(ParameterValueJson(v, folder));
    root["overrides"] = std::move(overrides);
    return root.dump(2);
}

std::optional<MaterialInstanceFile> ParseMaterialInstance(std::string_view text, const fs::path& folder, std::string* error)
{
    try {
        const json           root = json::parse(text);
        MaterialInstanceFile instance;
        instance.parent = AbsolutePath(root.value("parent", std::string{}), folder);
        for (const json& j : root.value("overrides", json::array())) {
            MaterialParameterValue v = ReadParameterValue(j, folder);
            if (v.name.empty())
                continue;
            std::erase_if(instance.overrides, [&](const MaterialParameterValue& o) { return o.name == v.name; });
            instance.overrides.push_back(std::move(v));
        }
        return instance;
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
        return std::nullopt;
    }
}

std::optional<MaterialInstanceFile> LoadMaterialInstance(const fs::path& file, std::string* error)
{
    const std::optional<std::string> text = Vfs::ReadText(file);
    if (!text) {
        if (error)
            *error = "cannot read '" + PathToUtf8(file) + "'";
        return std::nullopt;
    }
    std::string why;
    auto        instance = ParseMaterialInstance(*text, file.parent_path(), &why);
    if (!instance && error)
        *error = "'" + PathToUtf8(file) + "': " + why;
    return instance;
}

bool SaveMaterialInstance(const fs::path& file, const MaterialInstanceFile& instance, std::string* error)
{
    return WriteText(file, MaterialInstanceToJson(instance, file.parent_path()), error);
}

namespace {
bool HasExtension(const fs::path& path, std::string_view ext)
{
    std::string e = PathToUtf8(path.extension());
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ext;
}
} // namespace

bool IsMaterialFile(const fs::path& path) { return HasExtension(path, ".umat"); }
bool IsMaterialInstanceFile(const fs::path& path) { return HasExtension(path, ".uminst"); }

std::vector<MaterialParameter> ResolveMaterialParameters(const MaterialGraph& graph,
                                                         const std::vector<MaterialParameterValue>& overrides)
{
    std::vector<MaterialParameter> result = graph.parameters;
    for (const MaterialParameterValue& o : overrides)
        for (MaterialParameter& p : result)
            if (p.name == o.name && p.type == o.type) {
                if (o.type == MaterialParameterType::Texture)
                    p.texture = o.texture;
                else
                    p.value = o.value;
            }
    return result;
}

} // namespace Engine
