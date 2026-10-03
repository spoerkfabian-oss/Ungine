#include "Engine/Assets/Animation.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace Engine {

namespace {

glm::quat ToQuaternion(const glm::vec4& value)
{
    return {value.w, value.x, value.y, value.z};
}

glm::vec4 ToVector(const glm::quat& value)
{
    return {value.x, value.y, value.z, value.w};
}

glm::vec4 Hermite(const glm::vec4& start, const glm::vec4& outTangent, const glm::vec4& end,
                  const glm::vec4& inTangent, float t, float interval)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * start +
           (t3 - 2.0f * t2 + t) * interval * outTangent +
           (-2.0f * t3 + 3.0f * t2) * end + (t3 - t2) * interval * inTangent;
}

glm::vec4 Sample(const AnimationTrack& track, float time)
{
    const auto upper = std::upper_bound(track.times.begin(), track.times.end(), time);
    const std::size_t right = static_cast<std::size_t>(upper - track.times.begin());
    const std::size_t left = right == 0 ? 0 : right - 1;
    if (left >= track.values.size())
        return track.values.empty() ? glm::vec4{0.0f} : track.values.back();
    if (right >= track.times.size() || right >= track.values.size() ||
        track.interpolation == AnimationInterpolation::Step)
        return track.values[left];

    const float interval = track.times[right] - track.times[left];
    if (!(interval > 0.0f))
        return track.values[left];
    const float alpha = std::clamp((time - track.times[left]) / interval, 0.0f, 1.0f);

    if (track.interpolation == AnimationInterpolation::CubicSpline &&
        track.inTangents.size() == track.values.size() && track.outTangents.size() == track.values.size())
        return Hermite(track.values[left], track.outTangents[left], track.values[right],
                       track.inTangents[right], alpha, interval);
    if (track.path == AnimationPath::Rotation)
        return ToVector(glm::slerp(ToQuaternion(track.values[left]), ToQuaternion(track.values[right]), alpha));
    return glm::mix(track.values[left], track.values[right], alpha);
}

} // namespace

void EvaluateAnimation(const Model& model, const AnimationClip& clip, float timeSeconds,
                       AnimationPlayback playback, std::span<Transform> pose)
{
    if (pose.size() != model.nodes.size())
        throw std::invalid_argument("EvaluateAnimation: pose size must match model nodes");
    if (!std::isfinite(timeSeconds))
        throw std::invalid_argument("EvaluateAnimation: time must be finite");

    for (std::size_t i = 0; i < model.nodes.size(); ++i)
        pose[i] = model.nodes[i].local;

    float time = std::max(timeSeconds, 0.0f);
    if (clip.duration > 0.0f) {
        if (playback == AnimationPlayback::Loop)
            time = std::fmod(time, clip.duration);
        else
            time = std::min(time, clip.duration);
    }

    for (const AnimationTrack& track : clip.tracks) {
        if (track.node >= pose.size() || track.times.empty() || track.values.empty())
            continue;
        const glm::vec4 value = Sample(track, time);
        Transform& target = pose[track.node];
        switch (track.path) {
        case AnimationPath::Translation: target.position = glm::vec3(value); break;
        case AnimationPath::Scale: target.scale = glm::vec3(value); break;
        case AnimationPath::Rotation: {
            const glm::quat rotation = ToQuaternion(value);
            target.rotation = glm::dot(rotation, rotation) > 1.0e-8f ? glm::normalize(rotation) : target.rotation;
            break;
        }
        }
    }
}

void UpdateAnimations(Scene& scene, AssetManager& assets, float deltaSeconds)
{
    Registry& registry = scene.GetRegistry();
    const float dt = std::isfinite(deltaSeconds) ? std::max(deltaSeconds, 0.0f) : 0.0f;
    std::unordered_map<Entity, std::vector<Entity>> nodeEntitiesByRoot;
    registry.ViewOf<ModelNodeRef>().Each([&](Entity entity, ModelNodeRef& reference) {
        if (reference.instanceRoot == NullEntity)
            return;
        auto& nodes = nodeEntitiesByRoot[reference.instanceRoot];
        if (nodes.size() <= reference.node)
            nodes.resize(static_cast<std::size_t>(reference.node) + 1, NullEntity);
        nodes[reference.node] = entity;
    });

    registry.ViewOf<Animator, ModelInstance>().Each([&](Entity root, Animator& animator, ModelInstance& instance) {
        const Model* model = assets.Get(instance.model);
        if (!model || model->animations.empty())
            return;
        if (animator.clipIndex >= model->animations.size()) {
            animator.playing = false;
            return;
        }
        if (animator.sampledClip != animator.clipIndex) {
            animator.timeSeconds = 0.0f;
            animator.sampledClip = animator.clipIndex;
        }

        const AnimationClip& clip = model->animations[animator.clipIndex];
        if (animator.playing) {
            const float speed = std::isfinite(animator.speed) ? std::max(animator.speed, 0.0f) : 0.0f;
            animator.timeSeconds += dt * speed;
            if (!animator.looping && animator.timeSeconds >= clip.duration) {
                animator.timeSeconds = clip.duration;
                animator.playing = false;
            }
        }
        if (!std::isfinite(animator.timeSeconds))
            animator.timeSeconds = 0.0f;

        auto& nodeEntities = nodeEntitiesByRoot[root];
        if (nodeEntities.size() < model->nodes.size())
            nodeEntities.resize(model->nodes.size(), NullEntity);

        std::vector<Transform> pose(model->nodes.size());
        EvaluateAnimation(*model, clip, animator.timeSeconds,
                          animator.looping ? AnimationPlayback::Loop : AnimationPlayback::Once, pose);
        std::vector<std::uint8_t> animated(model->nodes.size(), 0);
        for (const AnimationTrack& track : clip.tracks)
            if (track.node < animated.size())
                animated[track.node] = 1;
        for (std::size_t i = 0; i < animated.size(); ++i)
            if (animated[i] && nodeEntities[i] != NullEntity && registry.Valid(nodeEntities[i]))
                scene.SetTransform(nodeEntities[i], pose[i]);
    });
}

} // namespace Engine
