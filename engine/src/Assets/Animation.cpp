#include "Engine/Assets/Animation.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
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

glm::vec3 RootMotionAt(const AnimationClip& clip, std::uint32_t node, float time, bool looping)
{
    const auto track = std::ranges::find_if(clip.tracks, [node](const AnimationTrack& value) {
        return value.node == node && value.path == AnimationPath::Translation;
    });
    if (track == clip.tracks.end() || track->times.empty() || track->values.empty())
        return glm::vec3{0.0f};
    const float duration = clip.duration;
    if (!looping || !(duration > 0.0f))
        return glm::vec3(Sample(*track, std::clamp(time, 0.0f, std::max(duration, 0.0f))));
    const float cycles = std::floor(std::max(time, 0.0f) / duration);
    const float phase = std::fmod(std::max(time, 0.0f), duration);
    const glm::vec3 start(Sample(*track, 0.0f));
    const glm::vec3 end(Sample(*track, duration));
    const glm::vec3 current(Sample(*track, phase));
    return current + cycles * (end - start);
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

void BlendAnimationPoses(std::span<Transform> source, std::span<const Transform> target, float weight)
{
    if (source.size() != target.size())
        throw std::invalid_argument("BlendAnimationPoses: pose sizes must match");
    const float alpha = std::clamp(std::isfinite(weight) ? weight : 0.0f, 0.0f, 1.0f);
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i].position = glm::mix(source[i].position, target[i].position, alpha);
        source[i].rotation = glm::normalize(glm::slerp(source[i].rotation, target[i].rotation, alpha));
        source[i].scale = glm::mix(source[i].scale, target[i].scale, alpha);
    }
}

glm::vec3 SampleRootMotion(const AnimationClip& clip, std::uint32_t node, float timeSeconds, bool looping)
{
    if (!std::isfinite(timeSeconds))
        return glm::vec3{0.0f};
    return RootMotionAt(clip, node, std::max(timeSeconds, 0.0f), looping);
}

std::optional<std::pair<glm::vec3, glm::vec3>> ComputeSkinnedBounds(
    const Model& model, std::uint32_t meshIndex, std::uint32_t skinIndex,
    std::span<const glm::mat4> jointPalette)
{
    if (meshIndex >= model.meshes.size() || skinIndex >= model.skins.size() ||
        jointPalette.size() != model.skins[skinIndex].joints.size() ||
        model.skinInfluences.size() != model.collisionPositions.size())
        return std::nullopt;

    glm::vec3 boundsMin{std::numeric_limits<float>::max()};
    glm::vec3 boundsMax{std::numeric_limits<float>::lowest()};
    bool hasBounds = false;
    for (const Submesh& submesh : model.meshes[meshIndex].submeshes) {
        const std::size_t first = submesh.firstIndex;
        const std::size_t end = std::min(first + submesh.indexCount, model.collisionIndices.size());
        for (std::size_t index = first; index < end; ++index) {
            const std::int64_t vertexIndex = std::int64_t{submesh.vertexOffset} + model.collisionIndices[index];
            if (vertexIndex < 0 || static_cast<std::size_t>(vertexIndex) >= model.collisionPositions.size())
                continue;
            const std::size_t v = static_cast<std::size_t>(vertexIndex);
            const VertexSkinInfluence& influence = model.skinInfluences[v];
            glm::vec4 position{0.0f};
            float totalWeight = 0.0f;
            for (glm::length_t c = 0; c < 4; ++c) {
                const float weight = influence.weights[c];
                const std::uint32_t joint = influence.joints[c];
                if (weight <= 0.0f || joint >= jointPalette.size())
                    continue;
                position += jointPalette[joint] * glm::vec4(model.collisionPositions[v], 1.0f) * weight;
                totalWeight += weight;
            }
            if (totalWeight > 1.0e-6f)
                position /= totalWeight;
            else
                position = glm::vec4(model.collisionPositions[v], 1.0f);
            const glm::vec3 p(position);
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                continue;
            boundsMin = glm::min(boundsMin, p);
            boundsMax = glm::max(boundsMax, p);
            hasBounds = true;
        }
    }
    return hasBounds ? std::optional{std::pair{boundsMin, boundsMax}} : std::nullopt;
}

void UpdateAnimations(Scene& scene, AssetManager& assets, float deltaSeconds)
{
    Registry& registry = scene.GetRegistry();
    const float dt = std::isfinite(deltaSeconds) ? std::max(deltaSeconds, 0.0f) : 0.0f;
    std::unordered_map<Entity, std::vector<Entity>> nodeEntitiesByRoot;
    registry.ViewOf<ModelNodeRef>().Each([&](Entity entity, ModelNodeRef& reference) {
        reference.instanceRoot = FindModelInstanceRoot(registry, entity); // refresh the hint
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
        if (animator.blendClipIndex >= model->animations.size())
            animator.blendWeight = 0.0f;
        else if (animator.sampledBlendClip != animator.blendClipIndex) {
            animator.blendTimeSeconds = 0.0f;
            animator.sampledBlendClip = animator.blendClipIndex;
        }

        const AnimationClip& clip = model->animations[animator.clipIndex];
        if (!std::isfinite(animator.timeSeconds))
            animator.timeSeconds = 0.0f;
        if (!std::isfinite(animator.blendTimeSeconds))
            animator.blendTimeSeconds = 0.0f;
        const float previousTime = animator.timeSeconds;
        const float previousBlendTime = animator.blendTimeSeconds;
        if (animator.playing) {
            const float speed = std::isfinite(animator.speed) ? std::max(animator.speed, 0.0f) : 0.0f;
            animator.timeSeconds += dt * speed;
            if (animator.blendClipIndex < model->animations.size())
                animator.blendTimeSeconds += dt * speed;
            if (!animator.looping && animator.timeSeconds >= clip.duration) {
                animator.timeSeconds = clip.duration;
                if (animator.blendClipIndex < model->animations.size())
                    animator.blendTimeSeconds = std::min(animator.blendTimeSeconds,
                                                         model->animations[animator.blendClipIndex].duration);
                animator.playing = false;
            }
        }
        if (!std::isfinite(animator.timeSeconds)) {
            animator.timeSeconds = 0.0f;
            animator.playing = false;
        }
        if (!std::isfinite(animator.blendTimeSeconds))
            animator.blendTimeSeconds = 0.0f;

        auto& nodeEntities = nodeEntitiesByRoot[root];
        if (nodeEntities.size() < model->nodes.size())
            nodeEntities.resize(model->nodes.size(), NullEntity);

        std::vector<Transform> pose(model->nodes.size());
        EvaluateAnimation(*model, clip, animator.timeSeconds,
                          animator.looping ? AnimationPlayback::Loop : AnimationPlayback::Once, pose);
        const bool blending = animator.blendClipIndex < model->animations.size() &&
                              animator.blendClipIndex != animator.clipIndex && animator.blendWeight > 0.0f;
        const float weight = std::clamp(std::isfinite(animator.blendWeight) ? animator.blendWeight : 0.0f, 0.0f, 1.0f);
        if (blending) {
            const AnimationClip& blendClip = model->animations[animator.blendClipIndex];
            std::vector<Transform> blendPose(model->nodes.size());
            EvaluateAnimation(*model, blendClip, animator.blendTimeSeconds,
                              animator.looping ? AnimationPlayback::Loop : AnimationPlayback::Once, blendPose);
            BlendAnimationPoses(pose, blendPose, weight);
        }

        if (animator.applyRootMotion && animator.rootMotionNode < pose.size()) {
            glm::vec3 delta = SampleRootMotion(clip, animator.rootMotionNode, animator.timeSeconds, animator.looping) -
                              SampleRootMotion(clip, animator.rootMotionNode, previousTime, animator.looping);
            if (blending) {
                const AnimationClip& blendClip = model->animations[animator.blendClipIndex];
                const glm::vec3 blendDelta =
                    SampleRootMotion(blendClip, animator.rootMotionNode, animator.blendTimeSeconds, animator.looping) -
                    SampleRootMotion(blendClip, animator.rootMotionNode, previousBlendTime, animator.looping);
                delta = glm::mix(delta, blendDelta, weight);
            }
            if (glm::length(delta) > 1.0e-7f) {
                Transform rootTransform = scene.GetTransform(root);
                // Tracks are expressed in model space while Transform::position is in
                // parent space. Convert through the current instance and parent bases.
                glm::vec3 parentSpaceDelta = glm::vec3(
                    registry.Get<WorldTransform>(root).matrix * glm::vec4(delta, 0.0f));
                const Entity parent = registry.Get<Hierarchy>(root).parent;
                if (parent != NullEntity && registry.Valid(parent)) {
                    const glm::mat4& parentWorld = registry.Get<WorldTransform>(parent).matrix;
                    const float parentDeterminant = glm::determinant(parentWorld);
                    if (std::isfinite(parentDeterminant) && std::abs(parentDeterminant) > 1.0e-8f)
                        parentSpaceDelta = glm::vec3(glm::inverse(parentWorld) * glm::vec4(parentSpaceDelta, 0.0f));
                }
                rootTransform.position += parentSpaceDelta;
                scene.SetTransform(root, rootTransform);
            }
            pose[animator.rootMotionNode].position = model->nodes[animator.rootMotionNode].local.position;
        }
        std::vector<std::uint8_t> animated(model->nodes.size(), 0);
        for (const AnimationClip& candidate : model->animations)
            for (const AnimationTrack& track : candidate.tracks)
                if (track.node < animated.size())
                    animated[track.node] = 1;
        // Unchanged poses (stopped or paused clips) are not written: a dirty transform costs BVH
        // updates and invalidates cached local shadows every frame.
        for (std::size_t i = 0; i < animated.size(); ++i)
            if (animated[i] && nodeEntities[i] != NullEntity && registry.Valid(nodeEntities[i]) &&
                scene.GetTransform(nodeEntities[i]) != pose[i])
                scene.SetTransform(nodeEntities[i], pose[i]);
    });
}

} // namespace Engine
