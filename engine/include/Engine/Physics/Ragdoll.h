#pragma once
#include "Engine/ECS/Entity.h"

#include <cstddef>
#include <vector>

namespace Engine {

class Scene;
struct Model;

struct RagdollSettings {
    float mass        = 70.0f; // kg, spread over the bones by volume
    float radiusScale = 0.25f; // capsule radius = bone length * radiusScale
    float swingAngle  = 0.6f;  // rad: half cone angle of each joint
    float twistAngle  = 0.4f;  // rad: twist either way
};

// Ragdoll of an instantiated, skinned model (root = the ModelInstance entity, model = its model):
// every skin joint becomes a bone with a kinematic RigidBody, a capsule Collider along the bone
// (towards its child joint) and a swing-twist Joint to its parent bone (no collision between the
// two); the root gets Ragdoll. Replaces an existing ragdoll. Returns the number of bones (0: no
// skeleton, nothing changed).
std::size_t CreateRagdoll(Scene& scene, Entity root, const Model& model, const RagdollSettings& settings = {});
// Removes Ragdoll and the bones' RagdollBone / RigidBody / Collider / Joint.
void RemoveRagdoll(Scene& scene, Entity root);
// Dynamic bones (the animation pauses) or kinematic bones that follow the animation.
void SetRagdollSimulating(Scene& scene, Entity root, bool simulate);
// The bones (entities with RagdollBone) below root.
[[nodiscard]] std::vector<Entity> RagdollBones(const Scene& scene, Entity root);

} // namespace Engine
