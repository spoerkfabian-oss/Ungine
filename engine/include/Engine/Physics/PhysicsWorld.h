#pragma once
#include "Engine/ECS/Entity.h"
#include "Engine/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Engine {

class AssetManager;
class EventBus;
class Scene;
class ThreadPool;

struct PhysicsHit {
    Entity      entity = NullEntity;
    glm::vec3   point{0.0f};
    glm::vec3   normal{0.0f}; // surface normal at the hit, facing the query
    float       distance = 0.0f;
    std::string surface;      // physics material surface of the hit triangle / shape ("" without material)
};

// Where and how hard two bodies touch (the deepest contact of the pair in that step).
struct ContactInfo {
    glm::vec3   point{0.0f};
    glm::vec3   normal{0.0f, 1.0f, 0.0f}; // from a towards b
    glm::vec3   relativeVelocity{0.0f};   // velocity of b's surface relative to a's at the point
    float       approachSpeed = 0.0f;     // m/s along the normal (0: separating)
    float       impulse       = 0.0f;     // N*s, estimated: effective mass * approach speed * (1 + restitution)
    std::string surfaceA, surfaceB;       // physics material surfaces ("" without material)
};

// Published on the EventBus (EventBus::Publish, main thread) at the end of PhysicsWorld::Step
// when two bodies start / stop touching. One Begin and one End per pair, however many shapes touch.
// End is also sent when a body is removed; its entity may already be destroyed then. Characters
// report their contacts too (a = the character). End carries the last contact seen.
struct CollisionEvent {
    Entity      a       = NullEntity;
    Entity      b       = NullEntity;
    bool        begin   = true;
    bool        trigger = false; // one of them is a trigger (sensor)
    ContactInfo contact;
};

// Every step two bodies keep touching (PhysicsSettings::persistEvents), after Begin.
struct CollisionPersistEvent {
    Entity      a       = NullEntity;
    Entity      b       = NullEntity;
    bool        trigger = false;
    ContactInfo contact;
};

// A joint exceeded its break force / torque: the physics removed it and set Joint::enabled = false.
struct JointBrokenEvent {
    Entity joint = NullEntity; // the entity with the Joint component
    Entity a     = NullEntity;
    Entity b     = NullEntity; // NullEntity: the world
};

enum class BodyActivity : std::uint8_t { None, Static, Kinematic, Active, Sleeping, Character };

inline constexpr std::uint32_t kPhysicsLayers = 16;

struct PhysicsSettings {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    int       collisionSteps = 1;    // sub-steps per Step (more for fast objects at low rates)
    float     airControl     = 2.0f; // characters in the air: rate (1/s) at which the velocity approaches the input
    // Interpolate(): shown poses of moving bodies blend between the last two steps (smooth motion
    // when the frame rate differs from the fixed step).
    bool interpolate = true;
    bool persistEvents = true; // CollisionPersistEvent every step for touching pairs
    // Bit b of layerCollision[a]: layer a collides with layer b (kept symmetric by SetLayerCollision).
    std::array<std::uint16_t, kPhysicsLayers> layerCollision = MakeAllLayersCollide();

    void SetLayerCollision(std::uint32_t a, std::uint32_t b, bool collide)
    {
        if (a >= kPhysicsLayers || b >= kPhysicsLayers)
            return;
        const auto set = [&](std::uint32_t x, std::uint32_t y) {
            const std::uint16_t bit = static_cast<std::uint16_t>(std::uint16_t{1} << y);
            if (collide)
                layerCollision[x] = static_cast<std::uint16_t>(layerCollision[x] | bit);
            else
                layerCollision[x] = static_cast<std::uint16_t>(layerCollision[x] & ~bit);
        };
        set(a, b);
        set(b, a);
    }
    [[nodiscard]] bool LayersCollide(std::uint32_t a, std::uint32_t b) const
    {
        return a < kPhysicsLayers && b < kPhysicsLayers && ((layerCollision[a] >> b) & 1u) != 0;
    }

private:
    static constexpr std::array<std::uint16_t, kPhysicsLayers> MakeAllLayersCollide()
    {
        std::array<std::uint16_t, kPhysicsLayers> all{};
        all.fill(0xFFFF);
        return all;
    }
};

struct PhysicsStats {
    std::uint32_t bodies       = 0;
    std::uint32_t joints       = 0; // active constraints
    std::uint32_t pendingJoints = 0; // waiting for a body (loading mesh, other level)
    std::uint32_t activeBodies = 0;
    std::uint32_t characters   = 0;
    std::uint32_t contactPairs = 0; // touching body pairs
    std::uint32_t created      = 0; // last Sync
    std::uint32_t removed      = 0; // last Sync
    std::uint32_t pendingMeshes = 0; // mesh colliders waiting for their model
    double        syncMs = 0.0;     // last Sync
    double        stepMs = 0.0;     // last Step: characters + simulation + write-back + events
};

enum class GroundState : std::uint8_t { OnGround, OnSteepGround, NotSupported, InAir };

struct CharacterState {
    bool        onGround = false;
    GroundState ground   = GroundState::InAir;
    glm::vec3   velocity{0.0f};
    glm::vec3   groundNormal{0.0f, 1.0f, 0.0f};
    glm::vec3   groundVelocity{0.0f}; // of what it stands on (moving platforms)
    Entity      groundEntity = NullEntity;
    bool        crouching    = false;
    float       yaw          = 0.0f; // radians about +Y (rotation modes other than None)
};

// A joint's current state (hinge angle, slider position, distance between the anchors).
struct JointState {
    bool      active   = false; // constraint exists (bodies present, enabled, not broken)
    float     angle    = 0.0f;  // hinge: rad
    float     position = 0.0f;  // slider: m, distance: m
    glm::vec3 force{0.0f};      // last step's constraint force (N) and torque (N*m)
    glm::vec3 torque{0.0f};
};

// World-space joint for debug drawing: frames on both bodies (equal while the joint holds).
struct JointDebugShape {
    Entity    entity = NullEntity;
    JointType type   = JointType::Hinge;
    glm::mat4 frameA{1.0f}, frameB{1.0f}; // rotation + position (X = joint axis)
    Entity    a = NullEntity, b = NullEntity;
    bool      active = false;
};

// World-space collider for debug drawing. transform: rotation + position of the shape center
// (no scale); extents already scaled. Mesh colliders are drawn as their world bounds (box).
struct ColliderDebugShape {
    Entity        entity = NullEntity;
    ColliderShape shape  = ColliderShape::Box;
    glm::mat4     transform{1.0f};   // includes Collider::rotation
    glm::vec3     halfExtents{0.0f}; // box, mesh bounds
    float         radius     = 0.0f; // sphere, capsule
    float         halfHeight = 0.0f; // capsule
    BodyActivity  activity   = BodyActivity::None;
    bool          trigger    = false;
};

// Collider of `shape` enclosing the local bounds (box: the bounds, sphere: their largest half
// extent, capsule: Y-up, radius from X/Z). Mesh keeps the default extents.
[[nodiscard]] Collider FitCollider(ColliderShape shape, const glm::vec3& boundsMin, const glm::vec3& boundsMax);

// Jolt-backed rigid body simulation of a Scene. Entities with RigidBody + Collider get a body,
// entities with CharacterController a virtual character. Main thread only; Jolt's jobs run on
// the engine ThreadPool (the main thread helps while it waits).
//
// Scene ownership: bodies follow the scene. Sync creates / recreates / removes bodies when the
// components or the world scale change and teleports bodies whose transform was changed from
// outside (kinematic bodies are moved to it instead). Step writes the transforms of moving
// dynamic bodies and characters back through Scene::EditTransform, parents before children (a
// dynamic child of a moving body gets its local transform from the parent's new pose).
class PhysicsWorld {
public:
    // assets: resolves MeshRenderer models for mesh colliders (null: mesh colliders stay pending).
    PhysicsWorld(ThreadPool& threads, EventBus& events, const AssetManager* assets = nullptr);
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&)            = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    PhysicsSettings settings;

    // Bodies <- scene (see above). Also updates the scene's world transforms first.
    void Sync(Scene& scene);
    // Sync, character movement, simulation over dt, write-back, collision events.
    void Step(Scene& scene, float dt);
    // Before rendering: shows moving bodies / characters between their last two steps (alpha 0..1
    // = time since the last step / fixed step). No effect without settings.interpolate.
    void Interpolate(Scene& scene, float alpha);
    // Removes every body and character without events (scene replaced / restored).
    void Reset();

    // Closest hit along dir (normalized internally), triggers ignored; layerMask: bit per collision layer.
    [[nodiscard]] std::optional<PhysicsHit> Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDistance,
                                                    Entity ignore = NullEntity, std::uint16_t layerMask = 0xFFFF) const;
    [[nodiscard]] std::optional<PhysicsHit> SphereCast(const glm::vec3& origin, float radius, const glm::vec3& dir,
                                                       float maxDistance, Entity ignore = NullEntity,
                                                       std::uint16_t layerMask = 0xFFFF) const;

    // Dynamic bodies only (wakes them up); no effect on others.
    void AddImpulse(Entity entity, const glm::vec3& impulse);
    void AddImpulseAt(Entity entity, const glm::vec3& impulse, const glm::vec3& worldPoint);
    void SetLinearVelocity(Entity entity, const glm::vec3& velocity);
    [[nodiscard]] glm::vec3 LinearVelocity(Entity entity) const;

    // Desired horizontal velocity (world units/s); jump is taken when the character stands.
    // Applies to the following Steps until changed.
    void SetCharacterInput(Entity entity, const glm::vec3& moveVelocity, bool jump);
    // Crouch (true) / stand up (false: as soon as there is room above). Applies until changed.
    void SetCharacterCrouch(Entity entity, bool crouch);
    // Target yaw (radians about +Y) for CharacterRotation::Script.
    void SetCharacterYaw(Entity entity, float yaw);
    [[nodiscard]] std::optional<CharacterState> GetCharacterState(Entity entity) const;

    // Joints (Joint components): current state; motor targets set on the component apply live.
    [[nodiscard]] JointState GetJointState(Entity jointEntity) const;
    void ForEachJoint(const std::function<void(const JointDebugShape&)>& fn) const;

    [[nodiscard]] BodyActivity Activity(Entity entity) const;
    [[nodiscard]] bool         HasBody(Entity entity) const; // body or character
    [[nodiscard]] const PhysicsStats& Stats() const;
    void ForEachCollider(const std::function<void(const ColliderDebugShape&)>& fn) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Engine
