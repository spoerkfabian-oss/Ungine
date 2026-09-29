#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

struct Name {
    std::string value;
};

// Stable identity (never 0): survives save/load and editor undo, unlike Entity handles.
struct Uuid {
    std::uint64_t value = 0;
};

// Local TRS relative to the parent.
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // w, x, y, z
    glm::vec3 scale{1.0f};

    [[nodiscard]] glm::mat4 LocalMatrix() const;
    bool operator==(const Transform&) const = default;
};

// Written by Scene::UpdateTransforms(); read-only for everyone else.
struct WorldTransform {
    glm::mat4 matrix{1.0f};
};

struct Hierarchy {
    Entity              parent = NullEntity;
    std::vector<Entity> children;
};

// Resolved through the AssetManager each frame; renders nothing until the model is Ready.
struct MeshRenderer {
    ModelHandle   model;
    std::uint32_t meshIndex = 0;
};

// Root entity of an instantiated model (InstantiateModel). Its node entities carry ModelNodeRef;
// RefreshModelInstances re-syncs them when the model is reloaded with a different structure.
struct ModelInstance {
    ModelHandle model;
};

struct ModelNodeRef {
    std::uint32_t node = 0; // index into Model::nodes
};

enum class LightType : std::uint8_t { Point, Spot };

// Punctual light with KHR_lights_punctual semantics. Position and direction (local -Z) come from
// the WorldTransform. Intensity is in candela on the same scale as the sun's illuminance
// (SkySettings::sunIntensity): at distance d the illuminance is intensity / d^2.
struct Light {
    LightType type           = LightType::Point;
    glm::vec3 color{1.0f};
    float     intensity      = 10.0f;
    float     range          = 0.0f; // world units; 0 = derived from the intensity (see EffectiveRange)
    float     innerConeAngle = 0.0f; // spot, radians from the axis: full intensity inside
    float     outerConeAngle = glm::quarter_pi<float>(); // spot: zero outside
    bool      castShadows    = true; // candidate for the shadow atlas (the renderer picks the most important)
};

// --- Physics (simulated by PhysicsWorld; a body needs RigidBody + Collider) ---

enum class BodyType : std::uint8_t { Static, Kinematic, Dynamic };

// Static: never moves (moving it by hand teleports it). Kinematic: follows its Transform (animation,
// gizmo) and pushes dynamic bodies. Dynamic: simulated, the physics writes its Transform.
struct RigidBody {
    BodyType type           = BodyType::Dynamic;
    float    mass           = 1.0f; // kg, dynamic only
    float    linearDamping  = 0.05f;
    float    angularDamping = 0.05f;
    float    gravityFactor  = 1.0f;
    bool     allowSleeping  = true;
    bool     continuous     = false; // dynamic: swept collision (no tunneling of fast bodies), costs more

    bool operator==(const RigidBody&) const = default;
};

enum class ColliderShape : std::uint8_t { Box, Sphere, Capsule, Mesh };

// Collision shape in the entity's local space, scaled by its world scale (box and mesh per axis,
// sphere and capsule by the largest axis). Capsules are Y-up. Mesh uses the triangles of the
// entity's MeshRenderer and is static or kinematic only (dynamic falls back to static).
struct Collider {
    ColliderShape shape = ColliderShape::Box;
    glm::vec3     halfExtents{0.5f};  // box
    float         radius     = 0.5f;  // sphere, capsule
    float         halfHeight = 0.5f;  // capsule: half the cylinder part
    glm::vec3     center{0.0f};       // offset in local space
    float         friction    = 0.5f;
    float         restitution = 0.0f;
    bool          trigger     = false; // sensor: reports CollisionEvents, no collision response
    std::uint8_t  layer       = 0;     // collision layer 0..15 (PhysicsSettings::layerCollision)

    bool operator==(const Collider&) const = default;
};

// Kinematic character (capsule, feet at the entity's position) moved by PhysicsWorld::SetCharacterInput:
// walks up steps and ramps up to maxSlope, sticks to the floor, jumps, pushes dynamic bodies.
// Takes precedence over RigidBody / Collider on the same entity.
struct CharacterController {
    float radius     = 0.3f;
    float height     = 1.8f;  // total, incl. the hemispheres
    float maxSlope   = 0.87266463f; // radians (50 degrees)
    float stepHeight = 0.35f;
    float jumpSpeed  = 5.0f;  // m/s

    bool operator==(const CharacterController&) const = default;
};

// Game camera: UnginePlayer renders through the first primary one (looking down local -Z).
struct CameraComponent {
    float fovY      = 1.0471976f; // radians (60 degrees)
    float nearPlane = 0.05f;
    bool  primary   = true;

    bool operator==(const CameraComponent&) const = default;
};

// Visual script (.ugraph, see Script/ScriptGraph.h) run by ScriptSystem while playing. graph: file
// path (absolute or relative to the working directory at runtime; scene files store it relative
// to the scene file).
struct ScriptComponent {
    std::string graph;

    bool operator==(const ScriptComponent&) const = default;
};

// Illuminance below which a light without explicit range is cut off (lights need a finite range
// for clustering).
inline constexpr float kLightCutoffIlluminance = 0.005f;

[[nodiscard]] inline float EffectiveRange(const Light& light)
{
    if (light.range > 0.0f)
        return light.range;
    const float peak = light.intensity * std::max({light.color.r, light.color.g, light.color.b});
    return std::sqrt(std::max(peak, 0.0f) / kLightCutoffIlluminance);
}

} // namespace Engine
