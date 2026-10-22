#include "Engine/Animation/AnimGraph.h"
#include "Engine/Assets/Model.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <system_error>

namespace Engine {

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

constexpr std::array kParamTypes{"float", "int", "bool", "trigger"};
constexpr std::array kCompares{">", "<", ">=", "<=", "==", "!=", "true", "false", "triggered"};
constexpr std::array kStateKinds{"clip", "blend1d", "blend2d"};
constexpr std::array kLayerModes{"override", "additive"};
constexpr std::array kIkKinds{"twoBone", "footPlacement", "lookAt", "fabrik"};

template <typename E, std::size_t N>
E EnumFrom(const json& j, const char* key, const std::array<const char*, N>& names, E fallback)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return fallback;
    const std::string value = it->get<std::string>();
    for (std::size_t i = 0; i < N; ++i)
        if (value == names[i])
            return static_cast<E>(i);
    return fallback;
}

template <typename E, std::size_t N>
const char* EnumName(E value, const std::array<const char*, N>& names)
{
    const auto index = static_cast<std::size_t>(value);
    return index < N ? names[index] : names[0];
}

float Finite(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

float GetFloat(const json& j, const char* key, float fallback)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? Finite(it->get<float>(), fallback) : fallback;
}

bool GetBool(const json& j, const char* key, bool fallback)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

std::string GetString(const json& j, const char* key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::uint32_t GetUint(const json& j, const char* key, std::uint32_t fallback)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_number_unsigned() ? it->get<std::uint32_t>() : fallback;
}

glm::vec2 GetVec2(const json& j, const char* key, glm::vec2 fallback)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number())
        return fallback;
    return {Finite((*it)[0].get<float>(), fallback.x), Finite((*it)[1].get<float>(), fallback.y)};
}

glm::vec3 GetVec3(const json& j, const char* key, glm::vec3 fallback)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 3)
        return fallback;
    glm::vec3 v = fallback;
    for (glm::length_t i = 0; i < 3; ++i)
        if ((*it)[static_cast<std::size_t>(i)].is_number())
            v[i] = Finite((*it)[static_cast<std::size_t>(i)].get<float>(), fallback[i]);
    return v;
}

std::vector<std::string> GetStrings(const json& j, const char* key)
{
    std::vector<std::string> out;
    const auto               it = j.find(key);
    if (it != j.end() && it->is_array())
        for (const json& value : *it)
            if (value.is_string())
                out.push_back(value.get<std::string>());
    return out;
}

const json& Array(const json& j, const char* key)
{
    static const json kEmpty = json::array();
    const auto        it     = j.find(key);
    return it != j.end() && it->is_array() ? *it : kEmpty;
}

// Relative to the graph file (also "../Sounds/x.wav"); absolute only without a relative path.
std::string StorePath(const std::string& path, const fs::path& folder)
{
    if (path.empty())
        return path;
    const fs::path absolute = PathFromUtf8(path).lexically_normal();
    const fs::path relative = absolute.lexically_relative(folder);
    if (!absolute.is_absolute() || relative.empty())
        return path;
    const std::u8string generic = relative.generic_u8string();
    return {generic.begin(), generic.end()};
}

std::string LoadPath(const std::string& path, const fs::path& folder)
{
    if (path.empty())
        return path;
    const fs::path p = PathFromUtf8(path);
    return PathToUtf8((p.is_absolute() ? p : folder / p).lexically_normal());
}

json Vec(glm::vec2 v) { return json::array({v.x, v.y}); }
json Vec(glm::vec3 v) { return json::array({v.x, v.y, v.z}); }

// Makes `name` unique among `used` ("Walk", "Walk 2", ...); empty names get `fallback`.
std::string Unique(std::string name, std::set<std::string>& used, const char* fallback)
{
    if (name.empty())
        name = fallback;
    std::string candidate = name;
    for (int n = 2; used.contains(candidate); ++n)
        candidate = name + " " + std::to_string(n);
    used.insert(candidate);
    return candidate;
}

} // namespace

const char* AnimParamTypeName(AnimParamType type) { return EnumName(type, kParamTypes); }
const char* AnimCompareName(AnimCompare op) { return EnumName(op, kCompares); }
const char* AnimStateKindName(AnimStateKind kind) { return EnumName(kind, kStateKinds); }
const char* AnimIkKindName(AnimIkKind kind) { return EnumName(kind, kIkKinds); }

const AnimState* AnimLayer::FindState(std::string_view stateName) const
{
    const auto it = std::ranges::find(states, stateName, &AnimState::name);
    return it == states.end() ? nullptr : &*it;
}

const AnimParameter* AnimGraph::FindParameter(std::string_view parameterName) const
{
    const auto it = std::ranges::find(parameters, parameterName, &AnimParameter::name);
    return it == parameters.end() ? nullptr : &*it;
}

AnimGraph MakeDefaultAnimGraph()
{
    AnimGraph graph;
    graph.parameters.push_back(AnimParameter{.name = "Speed", .type = AnimParamType::Float});
    AnimLayer layer;
    layer.entryState = "Idle";
    layer.states.push_back(AnimState{.name = "Idle", .position = {40.0f, 60.0f}});
    graph.layers.push_back(std::move(layer));
    return graph;
}

bool IsAnimGraphFile(const fs::path& file)
{
    std::string extension = PathToUtf8(file.extension());
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".uanim";
}

std::string AnimGraphToJson(const AnimGraph& graph, const fs::path& folderIn)
{
    const fs::path folder = folderIn.lexically_normal();
    json           root;
    root["version"] = 1;
    json& parameters = root["parameters"] = json::array();
    for (const AnimParameter& p : graph.parameters)
        parameters.push_back({{"name", p.name}, {"type", AnimParamTypeName(p.type)}, {"default", p.defaultValue}});

    json& layers = root["layers"] = json::array();
    for (const AnimLayer& layer : graph.layers) {
        json l{{"name", layer.name},
               {"mode", EnumName(layer.mode, kLayerModes)},
               {"weight", layer.weight},
               {"weightParameter", layer.weightParameter},
               {"entry", layer.entryState}};
        json& mask = l["mask"] = json::array();
        for (const AnimBoneMask& m : layer.mask)
            mask.push_back({{"bone", m.bone}, {"children", m.includeChildren}});
        json& states = l["states"] = json::array();
        for (const AnimState& s : layer.states) {
            json state{{"name", s.name},
                       {"kind", AnimStateKindName(s.kind)},
                       {"speed", s.speed},
                       {"loop", s.loop},
                       {"position", Vec(s.position)}};
            if (!s.speedParameter.empty())
                state["speedParameter"] = s.speedParameter;
            if (s.kind == AnimStateKind::Clip)
                state["clip"] = s.clip;
            else {
                state["x"] = s.xParameter;
                if (s.kind == AnimStateKind::BlendSpace2D)
                    state["y"] = s.yParameter;
                json& samples = state["samples"] = json::array();
                for (const AnimBlendSample& sample : s.samples)
                    samples.push_back({{"clip", sample.clip}, {"position", Vec(sample.position)}, {"speed", sample.speed}});
            }
            if (!s.notifies.empty()) {
                json& notifies = state["notifies"] = json::array();
                for (const AnimNotify& n : s.notifies) {
                    json notify{{"name", n.name}, {"time", n.time}};
                    if (!n.sound.empty())
                        notify["sound"] = StorePath(n.sound, folder), notify["volume"] = n.volume;
                    if (!n.effect.empty())
                        notify["effect"] = StorePath(n.effect, folder);
                    if (!n.bone.empty())
                        notify["bone"] = n.bone;
                    notifies.push_back(std::move(notify));
                }
            }
            states.push_back(std::move(state));
        }
        json& transitions = l["transitions"] = json::array();
        for (const AnimTransition& t : layer.transitions) {
            json transition{{"from", t.from},           {"to", t.to},
                            {"duration", t.duration},   {"hasExitTime", t.hasExitTime},
                            {"exitTime", t.exitTime},   {"canInterrupt", t.canInterrupt}};
            json& conditions = transition["conditions"] = json::array();
            for (const AnimCondition& c : t.conditions)
                conditions.push_back({{"parameter", c.parameter}, {"op", AnimCompareName(c.op)}, {"value", c.value}});
            transitions.push_back(std::move(transition));
        }
        layers.push_back(std::move(l));
    }

    json& ik = root["ik"] = json::array();
    for (const AnimIk& goal : graph.ik) {
        json g{{"name", goal.name},
               {"kind", AnimIkKindName(goal.kind)},
               {"enabled", goal.enabled},
               {"weight", goal.weight},
               {"weightParameter", goal.weightParameter}};
        switch (goal.kind) {
        case AnimIkKind::TwoBone:
            g["endBone"] = goal.endBone, g["keepEndRotation"] = goal.keepEndRotation;
            break;
        case AnimIkKind::Fabrik:
            g["rootBone"] = goal.rootBone, g["endBone"] = goal.endBone;
            g["iterations"] = goal.iterations, g["tolerance"] = goal.tolerance;
            break;
        case AnimIkKind::LookAt:
            g["bones"] = goal.bones, g["forwardAxis"] = Vec(goal.forwardAxis), g["maxAngle"] = goal.maxAngle;
            break;
        case AnimIkKind::FootPlacement:
            g["feet"] = goal.feet, g["pelvisBone"] = goal.pelvisBone;
            g["rayHeight"] = goal.rayHeight, g["rayDepth"] = goal.rayDepth;
            g["maxPelvisOffset"] = goal.maxPelvisOffset, g["alignToNormal"] = goal.alignToNormal;
            g["maxFootAngle"] = goal.maxFootAngle, g["smoothing"] = goal.smoothing, g["layerMask"] = goal.layerMask;
            break;
        }
        ik.push_back(std::move(g));
    }
    if (!graph.rootMotionBone.empty())
        root["rootMotionBone"] = graph.rootMotionBone;
    if (!graph.previewModel.empty())
        root["previewModel"] = StorePath(graph.previewModel, folder);
    return root.dump(2);
}

std::optional<AnimGraph> ParseAnimGraph(std::string_view text, const fs::path& folderIn, std::string* error)
{
    const fs::path folder = folderIn.lexically_normal();
    json           root   = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        if (error)
            *error = "not a JSON object";
        return std::nullopt;
    }
    if (root.value("version", 1) > 1) {
        if (error)
            *error = "made by a newer version";
        return std::nullopt;
    }

    AnimGraph             graph;
    std::set<std::string> parameterNames;
    for (const json& p : Array(root, "parameters")) {
        if (!p.is_object())
            continue;
        AnimParameter parameter;
        parameter.name         = Unique(GetString(p, "name"), parameterNames, "Parameter");
        parameter.type         = EnumFrom(p, "type", kParamTypes, AnimParamType::Float);
        parameter.defaultValue = GetFloat(p, "default", 0.0f);
        if (parameter.type == AnimParamType::Int)
            parameter.defaultValue = std::round(parameter.defaultValue);
        else if (parameter.type != AnimParamType::Float)
            parameter.defaultValue = parameter.defaultValue != 0.0f ? 1.0f : 0.0f;
        graph.parameters.push_back(std::move(parameter));
    }

    std::set<std::string> layerNames;
    for (const json& l : Array(root, "layers")) {
        if (!l.is_object())
            continue;
        AnimLayer layer;
        layer.name            = Unique(GetString(l, "name"), layerNames, "Layer");
        layer.mode            = EnumFrom(l, "mode", kLayerModes, AnimLayerMode::Override);
        layer.weight          = std::clamp(GetFloat(l, "weight", 1.0f), 0.0f, 1.0f);
        layer.weightParameter = GetString(l, "weightParameter");
        layer.entryState      = GetString(l, "entry");
        for (const json& m : Array(l, "mask"))
            if (m.is_object() && !GetString(m, "bone").empty())
                layer.mask.push_back(AnimBoneMask{.bone = GetString(m, "bone"), .includeChildren = GetBool(m, "children", true)});
        std::set<std::string> stateNames;
        for (const json& s : Array(l, "states")) {
            if (!s.is_object())
                continue;
            AnimState state;
            state.name           = Unique(GetString(s, "name"), stateNames, "State");
            state.kind           = EnumFrom(s, "kind", kStateKinds, AnimStateKind::Clip);
            state.clip           = GetString(s, "clip");
            state.speed          = std::clamp(GetFloat(s, "speed", 1.0f), -100.0f, 100.0f);
            state.speedParameter = GetString(s, "speedParameter");
            state.loop           = GetBool(s, "loop", true);
            state.xParameter     = GetString(s, "x");
            state.yParameter     = GetString(s, "y");
            state.position       = GetVec2(s, "position", {0.0f, 0.0f});
            for (const json& sample : Array(s, "samples"))
                if (sample.is_object())
                    state.samples.push_back(AnimBlendSample{.clip     = GetString(sample, "clip"),
                                                            .position = GetVec2(sample, "position", {0.0f, 0.0f}),
                                                            .speed = std::clamp(GetFloat(sample, "speed", 1.0f), 0.01f, 100.0f)});
            for (const json& n : Array(s, "notifies"))
                if (n.is_object())
                    state.notifies.push_back(AnimNotify{.name   = GetString(n, "name"),
                                                        .time   = std::clamp(GetFloat(n, "time", 0.0f), 0.0f, 1.0f),
                                                        .sound  = LoadPath(GetString(n, "sound"), folder),
                                                        .effect = LoadPath(GetString(n, "effect"), folder),
                                                        .bone   = GetString(n, "bone"),
                                                        .volume = std::clamp(GetFloat(n, "volume", 1.0f), 0.0f, 4.0f)});
            layer.states.push_back(std::move(state));
        }
        for (const json& t : Array(l, "transitions")) {
            if (!t.is_object())
                continue;
            AnimTransition transition;
            transition.from         = GetString(t, "from");
            transition.to           = GetString(t, "to");
            transition.duration     = std::clamp(GetFloat(t, "duration", 0.2f), 0.0f, 60.0f);
            transition.hasExitTime  = GetBool(t, "hasExitTime", false);
            transition.exitTime     = std::clamp(GetFloat(t, "exitTime", 1.0f), 0.0f, 1000.0f);
            transition.canInterrupt = GetBool(t, "canInterrupt", true);
            for (const json& c : Array(t, "conditions"))
                if (c.is_object())
                    transition.conditions.push_back(AnimCondition{.parameter = GetString(c, "parameter"),
                                                                  .op    = EnumFrom(c, "op", kCompares, AnimCompare::Greater),
                                                                  .value = GetFloat(c, "value", 0.0f)});
            layer.transitions.push_back(std::move(transition));
        }
        if (layer.entryState.empty() && !layer.states.empty())
            layer.entryState = layer.states.front().name;
        graph.layers.push_back(std::move(layer));
    }

    std::set<std::string> ikNames;
    for (const json& g : Array(root, "ik")) {
        if (!g.is_object())
            continue;
        AnimIk goal;
        goal.name            = Unique(GetString(g, "name"), ikNames, "IK");
        goal.kind            = EnumFrom(g, "kind", kIkKinds, AnimIkKind::TwoBone);
        goal.enabled         = GetBool(g, "enabled", true);
        goal.weight          = std::clamp(GetFloat(g, "weight", 1.0f), 0.0f, 1.0f);
        goal.weightParameter = GetString(g, "weightParameter");
        goal.endBone         = GetString(g, "endBone");
        goal.rootBone        = GetString(g, "rootBone");
        goal.keepEndRotation = GetBool(g, "keepEndRotation", true);
        goal.iterations      = std::clamp(GetUint(g, "iterations", 12), 1u, 64u);
        goal.tolerance       = std::clamp(GetFloat(g, "tolerance", 0.001f), 1.0e-6f, 1.0f);
        goal.bones           = GetStrings(g, "bones");
        goal.forwardAxis     = GetVec3(g, "forwardAxis", {0.0f, 0.0f, 1.0f});
        if (glm::dot(goal.forwardAxis, goal.forwardAxis) < 1.0e-8f)
            goal.forwardAxis = {0.0f, 0.0f, 1.0f};
        goal.maxAngle        = std::clamp(GetFloat(g, "maxAngle", 70.0f), 0.0f, 180.0f);
        goal.feet            = GetStrings(g, "feet");
        goal.pelvisBone      = GetString(g, "pelvisBone");
        goal.rayHeight       = std::clamp(GetFloat(g, "rayHeight", 0.5f), 0.0f, 100.0f);
        goal.rayDepth        = std::clamp(GetFloat(g, "rayDepth", 0.75f), 0.0f, 100.0f);
        goal.maxPelvisOffset = std::clamp(GetFloat(g, "maxPelvisOffset", 0.4f), 0.0f, 100.0f);
        goal.alignToNormal   = GetBool(g, "alignToNormal", true);
        goal.maxFootAngle    = std::clamp(GetFloat(g, "maxFootAngle", 30.0f), 0.0f, 90.0f);
        goal.smoothing       = std::clamp(GetFloat(g, "smoothing", 12.0f), 0.0f, 1000.0f);
        goal.layerMask       = GetUint(g, "layerMask", 0xFFFF) & 0xFFFFu;
        graph.ik.push_back(std::move(goal));
    }
    graph.rootMotionBone = GetString(root, "rootMotionBone");
    graph.previewModel   = LoadPath(GetString(root, "previewModel"), folder);
    return graph;
}

std::optional<AnimGraph> LoadAnimGraph(const fs::path& file, std::string* error)
{
    const std::optional<std::string> text = Vfs::ReadText(file);
    if (!text) {
        if (error)
            *error = "'" + PathToUtf8(file) + "': cannot read";
        return std::nullopt;
    }
    std::string parseError;
    auto        graph = ParseAnimGraph(*text, file.parent_path(), &parseError);
    if (!graph && error)
        *error = "'" + PathToUtf8(file) + "': " + parseError;
    return graph;
}

bool SaveAnimGraph(const fs::path& file, const AnimGraph& graph, std::string* error)
{
    std::error_code ec;
    if (file.has_parent_path())
        fs::create_directories(file.parent_path(), ec);
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << AnimGraphToJson(graph, file.parent_path());
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

std::vector<std::string> ValidateAnimGraph(const AnimGraph& graph, const Model* model)
{
    std::vector<std::string> problems;
    const auto parameter = [&](const std::string& name, const std::string& where, bool numeric) {
        if (name.empty())
            return;
        const AnimParameter* p = graph.FindParameter(name);
        if (!p)
            problems.push_back(where + ": unknown parameter '" + name + "'");
        else if (numeric && p->type != AnimParamType::Float && p->type != AnimParamType::Int)
            problems.push_back(where + ": parameter '" + name + "' is not a number");
    };
    const auto hasClip = [&](const std::string& clip) {
        return std::ranges::any_of(model->animations, [&](const AnimationClip& c) { return c.name == clip; });
    };
    const auto hasBone = [&](const std::string& bone) {
        return std::ranges::any_of(model->nodes, [&](const ModelNode& n) { return n.name == bone; });
    };
    const auto bone = [&](const std::string& name, const std::string& where) {
        if (model && !name.empty() && !hasBone(name))
            problems.push_back(where + ": bone '" + name + "' not in the model");
    };

    if (graph.layers.empty())
        problems.push_back("no layers: the graph does not animate anything");
    for (const AnimLayer& layer : graph.layers) {
        const std::string where = "layer '" + layer.name + "'";
        if (layer.states.empty())
            problems.push_back(where + ": no states");
        else if (!layer.FindState(layer.entryState))
            problems.push_back(where + ": entry state '" + layer.entryState + "' does not exist");
        parameter(layer.weightParameter, where, true);
        for (const AnimBoneMask& m : layer.mask)
            bone(m.bone, where + " mask");
        for (const AnimState& state : layer.states) {
            const std::string at = where + " state '" + state.name + "'";
            parameter(state.speedParameter, at, true);
            if (state.kind == AnimStateKind::Clip) {
                if (state.clip.empty())
                    problems.push_back(at + ": no clip");
                else if (model && !hasClip(state.clip))
                    problems.push_back(at + ": clip '" + state.clip + "' not in the model");
            } else {
                parameter(state.xParameter, at, true);
                if (state.xParameter.empty())
                    problems.push_back(at + ": blend space without an X parameter");
                if (state.kind == AnimStateKind::BlendSpace2D) {
                    parameter(state.yParameter, at, true);
                    if (state.yParameter.empty())
                        problems.push_back(at + ": blend space without a Y parameter");
                }
                if (state.samples.empty())
                    problems.push_back(at + ": blend space without samples");
                for (const AnimBlendSample& sample : state.samples)
                    if (model && !hasClip(sample.clip))
                        problems.push_back(at + ": clip '" + sample.clip + "' not in the model");
                if (state.kind == AnimStateKind::BlendSpace2D && state.samples.size() >= 1) {
                    // Needs three samples that are not on one line.
                    bool area = false;
                    for (std::size_t a = 0; a < state.samples.size() && !area; ++a)
                        for (std::size_t b = a + 1; b < state.samples.size() && !area; ++b)
                            for (std::size_t c = b + 1; c < state.samples.size() && !area; ++c) {
                                const glm::vec2 ab = state.samples[b].position - state.samples[a].position;
                                const glm::vec2 ac = state.samples[c].position - state.samples[a].position;
                                area = std::abs(ab.x * ac.y - ab.y * ac.x) > 1.0e-6f;
                            }
                    if (!area)
                        problems.push_back(at + ": 2D blend space needs three samples that are not on one line");
                }
            }
            for (const AnimNotify& notify : state.notifies)
                bone(notify.bone, at + " notify '" + notify.name + "'");
        }
        for (const AnimTransition& t : layer.transitions) {
            const std::string at = where + " transition '" + (t.from.empty() ? std::string("Any") : t.from) + "' -> '" + t.to + "'";
            if (!t.from.empty() && !layer.FindState(t.from))
                problems.push_back(at + ": unknown source state");
            if (!layer.FindState(t.to))
                problems.push_back(at + ": unknown target state");
            if (t.conditions.empty() && !t.hasExitTime)
                problems.push_back(at + ": no conditions and no exit time (fires at once)");
            for (const AnimCondition& c : t.conditions) {
                if (c.parameter.empty()) {
                    problems.push_back(at + ": condition without a parameter");
                    continue;
                }
                parameter(c.parameter, at, false);
                const AnimParameter* p = graph.FindParameter(c.parameter);
                if (p && c.op == AnimCompare::Triggered && p->type != AnimParamType::Trigger)
                    problems.push_back(at + ": 'triggered' needs a trigger parameter ('" + c.parameter + "')");
            }
        }
    }
    for (const AnimIk& goal : graph.ik) {
        const std::string at = "IK '" + goal.name + "'";
        parameter(goal.weightParameter, at, true);
        switch (goal.kind) {
        case AnimIkKind::TwoBone:
            if (goal.endBone.empty())
                problems.push_back(at + ": no end bone");
            bone(goal.endBone, at);
            break;
        case AnimIkKind::Fabrik:
            if (goal.endBone.empty() || goal.rootBone.empty())
                problems.push_back(at + ": needs a root and an end bone");
            bone(goal.rootBone, at), bone(goal.endBone, at);
            break;
        case AnimIkKind::LookAt:
            if (goal.bones.empty())
                problems.push_back(at + ": no bones");
            for (const std::string& b : goal.bones)
                bone(b, at);
            break;
        case AnimIkKind::FootPlacement:
            if (goal.feet.empty())
                problems.push_back(at + ": no feet");
            for (const std::string& b : goal.feet)
                bone(b, at);
            bone(goal.pelvisBone, at);
            break;
        }
    }
    bone(graph.rootMotionBone, "root motion");
    return problems;
}

} // namespace Engine
