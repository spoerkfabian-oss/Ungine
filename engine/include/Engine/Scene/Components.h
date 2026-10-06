#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Audio/AudioTypes.h"
#include "Engine/Core/FlatMap.h"
#include "Engine/Script/ScriptValue.h"
#include "Engine/ECS/Entity.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
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

// Playback state attached to an instantiated model root. Newly instantiated animated models play
// their first clip by default; editor controls can change these fields at runtime.
struct Animator {
    std::uint32_t clipIndex = 0;
    std::uint32_t sampledClip = ~std::uint32_t{0};
    std::uint32_t blendClipIndex = ~std::uint32_t{0};
    std::uint32_t sampledBlendClip = ~std::uint32_t{0};
    std::uint32_t rootMotionNode = 0;
    float         timeSeconds = 0.0f;
    float         blendTimeSeconds = 0.0f;
    float         blendWeight = 0.0f;
    float         speed = 1.0f;
    bool          looping = true;
    bool          playing = true;
    bool          applyRootMotion = false;
};

// Screen-space UI canvas. Canvases are independent of the 3D scene camera and are ordered by
// sortOrder; scaleWithViewport preserves a design resolution while adapting to the game window.
struct UiCanvas {
    glm::vec2 designSize{1920.0f, 1080.0f};
    std::int32_t sortOrder = 0;
    bool scaleWithViewport = true;
    bool visible = true;
};

enum class UiWidgetType : std::uint8_t { Text, Image, Panel, Button, Checkbox, Slider, ProgressBar };

// Anchors and offsets are relative to the parent widget's content rectangle (or the canvas).
// A zero-size anchor range is positioned by pivot; a stretched range uses both offset edges.
struct UiWidget {
    UiWidgetType type = UiWidgetType::Panel;
    glm::vec2 anchorMin{0.0f};
    glm::vec2 anchorMax{0.0f};
    glm::vec2 offsetMin{0.0f};
    glm::vec2 offsetMax{160.0f, 48.0f};
    glm::vec2 pivot{0.0f};
    glm::vec4 color{1.0f};
    glm::vec4 background{0.12f, 0.14f, 0.18f, 0.95f};
    std::string text{};
    std::string image{};
    float value = 0.0f;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float fontSize = 24.0f;
    bool checked = false;
    bool visible = true;
    bool enabled = true;
    bool interactable = true;
};

struct ModelNodeRef {
    std::uint32_t node = 0; // index into Model::nodes
    Entity        instanceRoot = NullEntity;
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
struct ScriptVariableOverride {
    ScriptValue   value = 0.0f;
    std::uint64_t entityUuid = 0; // entity variables: the referenced entity (0: none)

    bool operator==(const ScriptVariableOverride& o) const { return entityUuid == o.entityUuid && ValuesEqual(value, o.value); }
};

struct ScriptComponent {
    ScriptComponent() = default;
    ScriptComponent(std::string graphFile) : graph(std::move(graphFile)) {} // NOLINT(google-explicit-constructor)

    std::string graph;
    // Values of the graph's exposed ("instance editable") variables for this entity, by name.
    // (Components must move without throwing: no std::map, see FlatMap.)
    FlatMap<std::string, ScriptVariableOverride> variables;

    bool operator==(const ScriptComponent&) const = default;
};

// Free-form labels (Blueprint nodes find entities by tag).
struct Tags {
    std::vector<std::string> values;

    [[nodiscard]] bool Has(std::string_view tag) const { return std::ranges::find(values, tag) != values.end(); }
    bool operator==(const Tags&) const = default;
};

// Sound emitter played by AudioSystem while playing (play mode, player). sound: file path like
// ScriptComponent::graph. Spatial sources follow the entity (doppler from its motion) and are
// low-passed when physics geometry blocks the line to the listener (occlusion).
struct AudioSource {
    std::string sound;
    AudioBus    bus         = AudioBus::World;
    float       volume      = 1.0f;
    float       pitch       = 1.0f;
    bool        loop        = false;
    bool        playOnStart = true;
    bool        stream      = false; // always stream from the file (long files stream anyway)
    float       fadeIn      = 0.0f;  // seconds
    bool        spatial     = true;
    Attenuation attenuation = Attenuation::Inverse;
    float       minDistance = 1.0f;
    float       maxDistance = 50.0f;
    float       rolloff     = 1.0f;
    float       doppler     = 1.0f;
    bool        occlusion   = true;

    bool operator==(const AudioSource&) const = default;
};

// Where the scene is heard from (the first one; without, the primary camera, else the view).
struct AudioListener {
    bool operator==(const AudioListener&) const = default;
};

// Box (entity transform, halfExtents in local space) with its own room reverb. The listener
// blends the zones it is in (full weight inside, fading out over blendDistance around the box).
struct ReverbZone {
    glm::vec3    halfExtents{5.0f};
    float        blendDistance = 2.0f;
    ReverbParams reverb{.roomSize = 0.7f, .damping = 0.5f, .wet = 0.5f, .width = 1.0f};

    bool operator==(const ReverbZone&) const = default;
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

struct PrefabAsset; // parsed prefab file (Prefab.cpp)

// Root of a prefab instance (see Prefab.h). prefab: the .uprefab file (absolute; scene files
// store it relative like ScriptComponent::graph).
struct PrefabInstance {
    PrefabInstance() = default;
    explicit PrefabInstance(std::string file) : prefab(std::move(file)) {}

    std::string prefab;
    // Runtime only: the prefab content the instance was built from (overrides are computed
    // against it), and the scene-file data of an instance whose prefab could not be read
    // (written back unchanged when the scene is saved).
    std::shared_ptr<const PrefabAsset> built;
    std::shared_ptr<const std::string> unresolved;

    bool operator==(const PrefabInstance& o) const { return prefab == o.prefab; }
};

// Member of a prefab instance: the instance root's UUID and the entity's UUID in the prefab file.
struct PrefabLink {
    std::uint64_t instance = 0;
    std::uint64_t source   = 0;

    bool operator==(const PrefabLink&) const = default;
};

// Level streaming (see Scene/LevelStreaming.h). A box in the entity's space: while a streaming
// source (an entity with StreamingSource, else the camera) is inside the box grown by loadMargin,
// the sub-level is loaded additively; it is unloaded once every source is outside the box grown
// by loadMargin + unloadMargin (hysteresis against flickering at the border).
struct LevelStreamingVolume {
    std::string level{};          // scene file (absolute in memory, relative to the scene in files)
    glm::vec3   halfExtents{10.0f};
    float       loadMargin   = 0.0f;
    float       unloadMargin = 5.0f;

    bool operator==(const LevelStreamingVolume&) const = default;
};

// Streaming volumes follow this entity (the player character); without any, the camera.
struct StreamingSource {
    bool operator==(const StreamingSource&) const = default;
};

// Root entity of a streamed sub-level (runtime only: not saved, not in snapshots). The whole
// subtree belongs to the level and goes when it is unloaded.
struct StreamedLevel {
    std::string level{}; // normalized scene file

    bool operator==(const StreamedLevel&) const = default;
};

// Made by the construction script of another entity (the owner's UUID): rebuilt whenever it runs,
// not saved in scene files (kept in memory snapshots: undo, play).
struct ConstructionOwned {
    std::uint64_t owner = 0;

    bool operator==(const ConstructionOwned&) const = default;
};

} // namespace Engine
