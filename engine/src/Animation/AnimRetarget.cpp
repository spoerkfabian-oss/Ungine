#include "Engine/Animation/AnimRetarget.h"
#include "Engine/Assets/Animation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>

namespace Engine {

namespace {

// "mixamorig:LeftArm" -> "leftarm"
std::string BoneKey(std::string_view name)
{
    if (const std::size_t colon = name.rfind(':'); colon != std::string_view::npos)
        name.remove_prefix(colon + 1);
    std::string key(name);
    std::ranges::transform(key, key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return key;
}

} // namespace

std::size_t AnimRetarget::Mapped() const
{
    return static_cast<std::size_t>(std::ranges::count_if(sourceOf, [](int s) { return s >= 0; }));
}

AnimRetarget BuildRetarget(const Model& target, const Model& source)
{
    AnimRetarget map;
    map.source = &source;
    map.sourceOf.assign(target.nodes.size(), -1);
    map.root.assign(target.nodes.size(), 0);
    map.scale.assign(target.nodes.size(), 1.0f);
    std::unordered_map<std::string, int> byName;
    for (std::size_t s = 0; s < source.nodes.size(); ++s)
        byName.emplace(BoneKey(source.nodes[s].name), static_cast<int>(s)); // the first of equal names
    for (std::size_t t = 0; t < target.nodes.size(); ++t)
        if (const auto it = byName.find(BoneKey(target.nodes[t].name)); it != byName.end())
            map.sourceOf[t] = it->second;
    // Nodes are stored parents first: a mapped node is a root unless an ancestor is mapped.
    std::vector<std::uint8_t> mappedBelow(target.nodes.size(), 0); // has a mapped ancestor
    for (std::size_t t = 0; t < target.nodes.size(); ++t) {
        const int parent = target.nodes[t].parent;
        if (parent >= 0 && static_cast<std::size_t>(parent) < t)
            mappedBelow[t] = mappedBelow[static_cast<std::size_t>(parent)] || map.sourceOf[static_cast<std::size_t>(parent)] >= 0;
        if (map.sourceOf[t] < 0 || mappedBelow[t])
            continue;
        map.root[t] = 1;
        const float targetLength = glm::length(target.nodes[t].local.position);
        const float sourceLength = glm::length(source.nodes[static_cast<std::size_t>(map.sourceOf[t])].local.position);
        if (std::isfinite(targetLength) && std::isfinite(sourceLength) && targetLength > 1.0e-4f && sourceLength > 1.0e-4f)
            map.scale[t] = targetLength / sourceLength;
    }
    return map;
}

void RetargetPose(const AnimRetarget& map, const Model& target, std::span<const Transform> sourcePose,
                  std::span<Transform> targetPose)
{
    RestPose(target, targetPose);
    if (!map.source)
        return;
    for (std::size_t t = 0; t < target.nodes.size() && t < map.sourceOf.size() && t < targetPose.size(); ++t) {
        const int s = map.sourceOf[t];
        if (s < 0 || static_cast<std::size_t>(s) >= sourcePose.size() || static_cast<std::size_t>(s) >= map.source->nodes.size())
            continue;
        const Transform& sourceRest = map.source->nodes[static_cast<std::size_t>(s)].local;
        const Transform& targetRest = target.nodes[t].local;
        const Transform& animated   = sourcePose[static_cast<std::size_t>(s)];
        const glm::quat  rotation   = targetRest.rotation * glm::inverse(sourceRest.rotation) * animated.rotation;
        if (glm::dot(rotation, rotation) > 1.0e-8f)
            targetPose[t].rotation = glm::normalize(rotation);
        if (map.root[t])
            targetPose[t].position = targetRest.position + (animated.position - sourceRest.position) * map.scale[t];
    }
}

Model AnimationSourceModel(ModelData&& data)
{
    Model model;
    model.name       = std::move(data.name);
    model.nodes      = std::move(data.nodes);
    model.meshes     = std::move(data.meshes);
    model.animations = std::move(data.animations);
    for (Mesh& mesh : model.meshes)
        mesh.morph.deltas = {}; // only the layout matters here
    BuildMorphPoseLayout(model);
    return model;
}

} // namespace Engine
