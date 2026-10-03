#pragma once

#include "Engine/Assets/Model.h"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace Engine {

class AssetManager;
class Scene;

enum class AnimationPlayback : std::uint8_t { Once, Loop };

// Evaluates a glTF clip into local node transforms. `pose` must have one element per model node.
// The caller owns the pose buffer so it can reuse it across frames.
void EvaluateAnimation(const Model& model, const AnimationClip& clip, float timeSeconds,
                       AnimationPlayback playback, std::span<Transform> pose);

// Blends two local-transform poses in place; rotations use shortest-path quaternion slerp.
void BlendAnimationPoses(std::span<Transform> source, std::span<const Transform> target, float weight);

// Unwrapped root translation at a clip time. Looping includes each completed cycle's net motion.
[[nodiscard]] glm::vec3 SampleRootMotion(const AnimationClip& clip, std::uint32_t node,
                                         float timeSeconds, bool looping);

// Conservative local-space bounds for one skinned mesh, evaluated from its indexed vertices.
[[nodiscard]] std::optional<std::pair<glm::vec3, glm::vec3>> ComputeSkinnedBounds(
    const Model& model, std::uint32_t meshIndex, std::uint32_t skinIndex,
    std::span<const glm::mat4> jointPalette);

// Advances Animator components on instantiated model roots and applies the sampled local pose
// through Scene::SetTransform so world transforms, bounds and renderer caches stay coherent.
void UpdateAnimations(Scene& scene, AssetManager& assets, float deltaSeconds);

} // namespace Engine
