#pragma once

#include "Engine/Assets/Model.h"

#include <cstdint>
#include <span>

namespace Engine {

enum class AnimationPlayback : std::uint8_t { Once, Loop };

// Evaluates a glTF clip into local node transforms. `pose` must have one element per model node.
// The caller owns the pose buffer so it can reuse it across frames.
void EvaluateAnimation(const Model& model, const AnimationClip& clip, float timeSeconds,
                       AnimationPlayback playback, std::span<Transform> pose);

} // namespace Engine
