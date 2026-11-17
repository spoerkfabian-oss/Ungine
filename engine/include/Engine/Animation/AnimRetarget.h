#pragma once
#include "Engine/Assets/Model.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Engine {

// Retargeting: plays the clips of another model (an animation library with the same bone names)
// on a model. Bones map by name (case-insensitive, without a "namespace:" prefix such as
// "mixamorig:"). Rotations apply relative to the rest poses (target rest * inverse(source rest) *
// source), so both rigs must use the same bone axes; translations only move the mapped root bones,
// scaled by the ratio of their rest offsets (taller characters take longer steps). Unmapped
// target bones keep their rest pose.
struct AnimRetarget {
    const Model*              source = nullptr;
    std::vector<int>          sourceOf; // per target node: the source node, -1 when unmapped
    std::vector<std::uint8_t> root;     // per target node: a mapped bone without mapped ancestors
    std::vector<float>        scale;    // per target node: translation scale (roots)

    [[nodiscard]] std::size_t Mapped() const;
};

[[nodiscard]] AnimRetarget BuildRetarget(const Model& target, const Model& source);
// sourcePose: AnimationPoseSize(source); targetPose: AnimationPoseSize(target) (rest-filled here).
void RetargetPose(const AnimRetarget& map, const Model& target, std::span<const Transform> sourcePose,
                  std::span<Transform> targetPose);
// A CPU-only model (nodes, meshes for the pose layout, clips) of loaded model data: an animation
// source does not need GPU resources.
[[nodiscard]] Model AnimationSourceModel(ModelData&& data);

} // namespace Engine
