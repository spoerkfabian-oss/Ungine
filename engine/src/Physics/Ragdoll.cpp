#include "Engine/Physics/Ragdoll.h"
#include "Engine/Assets/Model.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

namespace Engine {

namespace {

// Node index -> entity of an instantiated model (its node entities below root).
std::unordered_map<std::int32_t, Entity> NodeEntities(const Registry& registry, Entity root)
{
    std::unordered_map<std::int32_t, Entity> nodes;
    std::function<void(Entity)>              walk = [&](Entity e) {
        if (const auto* ref = registry.TryGet<ModelNodeRef>(e); ref && FindModelInstanceRoot(registry, e) == root)
            nodes.try_emplace(static_cast<std::int32_t>(ref->node), e);
        for (const Entity child : registry.Get<Hierarchy>(e).children)
            walk(child);
    };
    walk(root);
    return nodes;
}

glm::quat RotationFromYTo(const glm::vec3& direction)
{
    return glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(direction));
}
glm::quat RotationFromXTo(const glm::vec3& direction)
{
    return glm::rotation(glm::vec3(1.0f, 0.0f, 0.0f), glm::normalize(direction));
}

} // namespace

std::size_t CreateRagdoll(Scene& scene, Entity root, const Model& model, const RagdollSettings& settings)
{
    Registry& registry = scene.GetRegistry();
    if (!registry.Valid(root))
        return 0;
    scene.UpdateTransforms();

    // The bones: every skin joint that has an entity.
    std::unordered_set<std::int32_t> jointNodes;
    for (const Skin& skin : model.skins)
        for (const SkinJoint& joint : skin.joints)
            if (joint.node >= 0 && static_cast<std::size_t>(joint.node) < model.nodes.size())
                jointNodes.insert(joint.node);
    const auto nodeEntities = NodeEntities(registry, root);
    std::erase_if(jointNodes, [&](std::int32_t n) { return !nodeEntities.contains(n); });
    if (jointNodes.empty())
        return 0;
    RemoveRagdoll(scene, root);

    const auto parentBone = [&](std::int32_t node) {
        for (std::int32_t p = model.nodes[static_cast<std::size_t>(node)].parent; p >= 0; p = model.nodes[static_cast<std::size_t>(p)].parent)
            if (jointNodes.contains(p))
                return p;
        return std::int32_t{-1};
    };
    const auto worldOf    = [&](std::int32_t node) -> const glm::mat4& { return registry.Get<WorldTransform>(nodeEntities.at(node)).matrix; };
    const auto worldScale = [&](std::int32_t node) {
        const glm::mat4& m = worldOf(node);
        return std::max({glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])), 1e-6f});
    };

    struct Bone {
        std::int32_t node = -1, parent = -1;
        glm::vec3    direction{0.0f, 1.0f, 0.0f}; // towards the child, in the bone's local space
        float        length = 0.0f;                // local units
        float        volume = 0.0f;                // world units
    };
    std::vector<Bone> bones;
    for (const std::int32_t node : jointNodes) {
        Bone bone;
        bone.node   = node;
        bone.parent = parentBone(node);
        // Towards the children's mean position; a leaf continues its parent's direction at half its length.
        const glm::mat4 inverse = glm::inverse(worldOf(node));
        glm::vec3       sum{0.0f};
        int             children = 0;
        for (const std::int32_t other : jointNodes)
            if (parentBone(other) == node) {
                sum += glm::vec3(inverse * worldOf(other)[3]);
                ++children;
            }
        if (children > 0) {
            bone.direction = sum / static_cast<float>(children);
        } else if (bone.parent >= 0) {
            const glm::vec3 fromParent = glm::vec3(inverse * worldOf(bone.parent)[3]);
            bone.direction             = -fromParent * 0.5f;
        }
        bone.length = glm::length(bone.direction);
        if (!(bone.length > 1e-5f)) { // no direction known: a small sphere-like capsule
            bone.direction = {0.0f, 1.0f, 0.0f};
            bone.length    = 0.1f / worldScale(node);
        }
        const float worldLength = bone.length * worldScale(node);
        const float radius      = worldLength * settings.radiusScale;
        bone.volume = std::numbers::pi_v<float> * radius * radius * (worldLength + 4.0f / 3.0f * radius);
        bones.push_back(bone);
    }
    float totalVolume = 0.0f;
    for (const Bone& b : bones)
        totalVolume += b.volume;

    for (const Bone& b : bones) {
        const Entity e = nodeEntities.at(b.node);
        Collider     collider;
        collider.shape      = ColliderShape::Capsule;
        collider.radius     = std::max(b.length * settings.radiusScale, 1e-4f);
        collider.halfHeight = std::max(b.length * 0.5f - collider.radius, 0.0f);
        collider.center     = glm::normalize(b.direction) * (b.length * 0.5f);
        collider.rotation   = RotationFromYTo(b.direction);
        RigidBody body;
        body.type = BodyType::Kinematic; // the Ragdoll decides (see PhysicsWorld)
        body.mass = std::max(settings.mass * (totalVolume > 0.0f ? b.volume / totalVolume : 1.0f / static_cast<float>(bones.size())), 0.01f);
        body.angularDamping = 0.2f;
        registry.EmplaceOrReplace<RigidBody>(e, body);
        registry.EmplaceOrReplace<Collider>(e, collider);
        registry.EmplaceOrReplace<RagdollBone>(e);
        if (b.parent >= 0) {
            Joint joint;
            joint.type             = JointType::SwingTwist;
            joint.connectedBody    = registry.Get<Uuid>(nodeEntities.at(b.parent)).value;
            joint.anchorRotation   = RotationFromXTo(b.direction); // twist about the bone
            joint.coneAngle        = settings.swingAngle;
            joint.planeAngle       = settings.swingAngle;
            joint.minLimit         = -settings.twistAngle;
            joint.maxLimit         = settings.twistAngle;
            joint.collideConnected = false;
            registry.EmplaceOrReplace<Joint>(e, joint);
        } else {
            registry.Remove<Joint>(e);
        }
    }
    registry.EmplaceOrReplace<Ragdoll>(root, Ragdoll{.simulate = false, .mass = settings.mass});
    return bones.size();
}

std::vector<Entity> RagdollBones(const Scene& scene, Entity root)
{
    const Registry&     registry = scene.GetRegistry();
    std::vector<Entity> bones;
    if (!registry.Valid(root))
        return bones;
    std::function<void(Entity)> walk = [&](Entity e) {
        if (registry.Has<RagdollBone>(e))
            bones.push_back(e);
        for (const Entity child : registry.Get<Hierarchy>(e).children)
            walk(child);
    };
    walk(root);
    return bones;
}

void RemoveRagdoll(Scene& scene, Entity root)
{
    Registry& registry = scene.GetRegistry();
    for (const Entity bone : RagdollBones(scene, root)) {
        registry.Remove<RagdollBone>(bone);
        registry.Remove<Joint>(bone);
        registry.Remove<Collider>(bone);
        registry.Remove<RigidBody>(bone);
    }
    if (registry.Valid(root))
        registry.Remove<Ragdoll>(root);
}

void SetRagdollSimulating(Scene& scene, Entity root, bool simulate)
{
    Registry& registry = scene.GetRegistry();
    if (registry.Valid(root))
        if (auto* ragdoll = registry.TryGet<Ragdoll>(root))
            ragdoll->simulate = simulate;
}

} // namespace Engine
