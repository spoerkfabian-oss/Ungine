#pragma once

#include "Engine/Assets/Model.h"

#include <cstdint>
#include <span>

namespace Engine {

class AssetManager;
class Scene;

enum class AnimationPlayback : std::uint8_t { Once, Loop };

// Evaluates a glTF clip into local node transforms. `pose` must have one element per model node.
// The caller owns the pose buffer so it can reuse it across frames.
void EvaluateAnimation(const Model& model, const AnimationClip& clip, float timeSeconds,
                       AnimationPlayback playback, std::span<Transform> pose);

// Advances Animator components on instantiated model roots and applies the sampled local pose
// through Scene::SetTransform so world transforms, bounds and renderer caches stay coherent.
void UpdateAnimations(Scene& scene, AssetManager& assets, float deltaSeconds);

} // namespace Engine
