#pragma once

#include "Engine/Assets/Model.h"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace Engine {

class AssetManager;
class Registry;
class Scene;

enum class AnimationPlayback : std::uint8_t { Once, Loop };

// Poses hold one local transform per model node, then the morph weights of the model's morph meshes
// (Model::morphPoseFirst), three per entry in Transform::position: blending poses blends them too.
[[nodiscard]] std::size_t AnimationPoseSize(const Model& model);
// The rest pose: node locals and the meshes' default morph weights.
void RestPose(const Model& model, std::span<Transform> pose);
// Pose entry / component of a node's morph target weight; nullopt without one.
[[nodiscard]] std::optional<std::pair<std::size_t, int>> MorphPoseSlot(const Model& model, std::uint32_t node,
                                                                      std::uint32_t target);

// Evaluates a glTF clip into local node transforms (and morph weights). `pose` must have
// AnimationPoseSize elements (one per node without morph meshes). The caller owns the pose buffer
// so it can reuse it across frames.
void EvaluateAnimation(const Model& model, const AnimationClip& clip, float timeSeconds,
                       AnimationPlayback playback, std::span<Transform> pose);

// Writes the morph weights of `pose` into the MorphWeights of the node entities (nodeEntities[node])
// whose weight entries `driven` marks (one flag per pose entry); unchanged weights are not touched.
void ApplyMorphPose(Registry& registry, const Model& model, std::span<const Transform> pose,
                    std::span<const Entity> nodeEntities, std::span<const std::uint8_t> driven);
// Marks the pose entries a clip's weight tracks drive.
void MarkMorphDriven(const Model& model, const AnimationClip& clip, std::span<std::uint8_t> driven);

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
