#pragma once
#include "Engine/ECS/Entity.h"
#include "Engine/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace Engine {

class AssetManager;
class EventBus;
class Scene;
class ThreadPool;

struct PhysicsHit {
    Entity    entity = NullEntity;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f}; // surface normal at the hit, facing the query
    float     distance = 0.0f;
};

// Published on the EventBus (EventBus::Publish, main thread) at the end of PhysicsWorld::Step
// when two bodies start / stop touching. One Begin and one End per pair, however many shapes touch.
// End is also sent when a body is removed; its entity may already be destroyed then.
struct CollisionEvent {
    Entity a       = NullEntity;
    Entity b       = NullEntity;
    bool   begin   = true;
    bool   trigger = false; // one of them is a trigger (sensor)
};

enum class BodyActivity : std::uint8_t { None, Static, Kinematic, Active, Sleeping, Character };

struct PhysicsSettings {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    int       collisionSteps = 1;    // sub-steps per Step (more for fast objects at low rates)
    float     airControl     = 2.0f; // characters in the air: rate (1/s) at which the velocity approaches the input
};

struct PhysicsStats {
    std::uint32_t bodies       = 0;
    std::uint32_t activeBodies = 0;
    std::uint32_t characters   = 0;
    std::uint32_t contactPairs = 0; // touching body pairs
    std::uint32_t created      = 0; // last Sync
    std::uint32_t removed      = 0; // last Sync
    std::uint32_t pendingMeshes = 0; // mesh colliders waiting for their model
    double        syncMs = 0.0;     // last Sync
    double        stepMs = 0.0;     // last Step: characters + simulation + write-back + events
};

struct CharacterState {
    bool      onGround = false;
    glm::vec3 velocity{0.0f};
    glm::vec3 groundNormal{0.0f, 1.0f, 0.0f};
};

// World-space collider for debug drawing. transform: rotation + position of the shape center
// (no scale); extents already scaled. Mesh colliders are drawn as their world bounds (box).
struct ColliderDebugShape {
    Entity        entity = NullEntity;
    ColliderShape shape  = ColliderShape::Box;
    glm::mat4     transform{1.0f};
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
// dynamic bodies and characters back through Scene::EditTransform. Dynamic bodies should not be
// children of moving bodies (their local transform is recomputed from the parent's).
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
    // Removes every body and character without events (scene replaced / restored).
    void Reset();

    // Closest hit along dir (normalized internally), triggers ignored.
    [[nodiscard]] std::optional<PhysicsHit> Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDistance,
                                                    Entity ignore = NullEntity) const;
    [[nodiscard]] std::optional<PhysicsHit> SphereCast(const glm::vec3& origin, float radius, const glm::vec3& dir,
                                                       float maxDistance, Entity ignore = NullEntity) const;

    // Dynamic bodies only (wakes them up); no effect on others.
    void AddImpulse(Entity entity, const glm::vec3& impulse);
    void AddImpulseAt(Entity entity, const glm::vec3& impulse, const glm::vec3& worldPoint);
    void SetLinearVelocity(Entity entity, const glm::vec3& velocity);
    [[nodiscard]] glm::vec3 LinearVelocity(Entity entity) const;

    // Desired horizontal velocity (world units/s); jump is taken when the character stands.
    // Applies to the following Steps until changed.
    void SetCharacterInput(Entity entity, const glm::vec3& moveVelocity, bool jump);
    [[nodiscard]] std::optional<CharacterState> GetCharacterState(Entity entity) const;

    [[nodiscard]] BodyActivity Activity(Entity entity) const;
    [[nodiscard]] bool         HasBody(Entity entity) const; // body or character
    [[nodiscard]] const PhysicsStats& Stats() const;
    void ForEachCollider(const std::function<void(const ColliderDebugShape&)>& fn) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Engine
