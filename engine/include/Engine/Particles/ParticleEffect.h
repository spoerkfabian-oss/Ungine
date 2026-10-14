#pragma once
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Engine {

// Particle effects (.uparticle): emitters built from stacks of modules (spawn -> initialize ->
// update -> events), simulated on the CPU by ParticleWorld and drawn as sprites, meshes or ribbons.

inline constexpr std::string_view kParticleEffectExtension = ".uparticle";
inline constexpr std::uint32_t    kMaxParticlesPerEmitter  = 100000;

// A value over normalized time (0..1, e.g. particle age / lifetime). No keys: 1.
struct ParticleCurve {
    enum class Interp : std::uint8_t { Linear, Smooth, Constant };
    struct Key {
        float time  = 0.0f;
        float value = 1.0f;
        bool operator==(const Key&) const = default;
    };
    std::vector<Key> keys{}; // sorted by time
    Interp           interp = Interp::Linear;

    [[nodiscard]] float Evaluate(float t) const;
    [[nodiscard]] static ParticleCurve Constant(float value);
    [[nodiscard]] static ParticleCurve Ramp(float from, float to);
    bool operator==(const ParticleCurve&) const = default;
};

// A linear RGBA gradient over normalized time. No keys: white.
struct ParticleGradient {
    struct Key {
        float     time = 0.0f;
        glm::vec4 color{1.0f};
        bool operator==(const Key&) const = default;
    };
    std::vector<Key> keys{}; // sorted by time

    [[nodiscard]] glm::vec4 Evaluate(float t) const;
    [[nodiscard]] static ParticleGradient Fade(const glm::vec4& from, const glm::vec4& to);
    bool operator==(const ParticleGradient&) const = default;
};

struct FloatRange {
    float min = 1.0f, max = 1.0f;
    bool operator==(const FloatRange&) const = default;
};

// --- Modules (one struct per type; units: meters, seconds, degrees) ---

// Spawn: continuous rate (per second; `scale` over the emitter's duration, or its first second of
// a loop when the duration is 0).
struct SpawnRateModule {
    float         rate = 10.0f;
    ParticleCurve scale{};
    bool operator==(const SpawnRateModule&) const = default;
};
// Spawn: `count` (random up to countMax) at `time`, repeated `cycles` times (0 = forever) every `interval`.
struct SpawnBurstModule {
    float         time     = 0.0f;
    std::uint32_t count    = 10;
    std::uint32_t countMax = 0; // > count: random in [count, countMax]
    std::uint32_t cycles   = 1;
    float         interval = 1.0f;
    bool operator==(const SpawnBurstModule&) const = default;
};
// Spawn: per meter the emitter moves (trails behind moving objects).
struct SpawnPerDistanceModule {
    float perMeter = 4.0f;
    bool operator==(const SpawnPerDistanceModule&) const = default;
};

enum class ParticleShape : std::uint8_t { Point, Sphere, Hemisphere, Box, Cone, Circle, Count };
// Initialize: start position in the emitter's space (+Y up; cone and hemisphere open along +Y,
// circle in the XZ plane). `surface`: on the shape's surface / rim instead of inside.
struct ShapeModule {
    ParticleShape shape = ParticleShape::Sphere;
    float         radius = 0.5f;
    glm::vec3     halfExtents{0.5f};
    float         angle  = 25.0f; // cone half angle
    bool          surface = false;
    glm::vec3     offset{0.0f};
    bool operator==(const ShapeModule&) const = default;
};
// Initialize: `direction` spread over a cone of `spread` degrees, speed in [min, max]; plus
// `radial` speed away from the shape center and a share of the emitter's own velocity.
struct VelocityModule {
    glm::vec3  direction{0.0f, 1.0f, 0.0f};
    float      spread = 15.0f;
    FloatRange speed{1.0f, 2.0f};
    FloatRange radial{0.0f, 0.0f};
    float      inheritVelocity = 0.0f;
    bool operator==(const VelocityModule&) const = default;
};
struct LifetimeModule {
    FloatRange seconds{1.0f, 2.0f};
    bool operator==(const LifetimeModule&) const = default;
};
// Initialize: size (sprites: width = height unless perAxis; meshes: scale per axis; ribbons: width).
struct SizeModule {
    FloatRange size{0.1f, 0.2f};
    bool       perAxis = false;
    glm::vec3  axisMin{0.1f}, axisMax{0.2f};
    bool operator==(const SizeModule&) const = default;
};
// Initialize: random color between a and b (per channel), times `intensity` (HDR emission).
struct ColorModule {
    glm::vec4 colorA{1.0f};
    glm::vec4 colorB{1.0f};
    float     intensity = 1.0f;
    bool operator==(const ColorModule&) const = default;
};
// Initialize: sprite roll / mesh rotation about a random axis, and its rate.
struct RotationModule {
    FloatRange angle{0.0f, 0.0f};
    FloatRange rate{0.0f, 0.0f};
    bool operator==(const RotationModule&) const = default;
};
// Initialize: a random start frame of the sprite sheet.
struct SpriteFrameModule {
    bool random = true;
    bool operator==(const SpriteFrameModule&) const = default;
};

struct GravityModule {
    glm::vec3 acceleration{0.0f, -9.81f, 0.0f};
    bool operator==(const GravityModule&) const = default;
};
struct DragModule {
    float coefficient = 1.0f; // velocity *= exp(-coefficient * dt)
    bool operator==(const DragModule&) const = default;
};
// Update: constant acceleration (wind), in world space or the emitter's.
struct ForceModule {
    glm::vec3 acceleration{1.0f, 0.0f, 0.0f};
    bool      local = false;
    bool operator==(const ForceModule&) const = default;
};
// Update: divergence-free turbulence.
struct CurlNoiseModule {
    float strength  = 2.0f;
    float frequency = 0.5f;
    float scroll    = 0.3f; // pattern movement per second
    bool operator==(const CurlNoiseModule&) const = default;
};
// Update: pull towards a point in the emitter's space (radius 0: everywhere, else falling off).
struct AttractorModule {
    glm::vec3 position{0.0f};
    float     strength   = 5.0f;
    float     radius     = 0.0f;
    float     killRadius = 0.0f; // particles closer die
    bool operator==(const AttractorModule&) const = default;
};
struct SpeedLimitModule {
    float maxSpeed = 5.0f;
    bool operator==(const SpeedLimitModule&) const = default;
};
struct SizeOverLifeModule {
    ParticleCurve curve = ParticleCurve::Ramp(1.0f, 0.0f);
    bool operator==(const SizeOverLifeModule&) const = default;
};
struct ColorOverLifeModule {
    ParticleGradient gradient = ParticleGradient::Fade(glm::vec4(1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.0f));
    bool operator==(const ColorOverLifeModule&) const = default;
};
// Update: sprite sheet frame over the particle's life (`cycles` times) or at a fixed rate.
struct SubUvModule {
    bool  overLife = true;
    float cycles   = 1.0f;
    float fps      = 10.0f;
    bool operator==(const SubUvModule&) const = default;
};
// Update: collision with physics geometry (PhysicsWorld raycasts along each particle's motion).
struct CollisionModule {
    float         radiusScale = 0.5f; // particle radius = size * radiusScale
    float         restitution = 0.3f;
    float         friction    = 0.2f; // tangential velocity loss per hit
    bool          kill        = false;
    std::uint32_t maxHits     = 0; // > 0: dies at the n-th hit
    std::uint16_t layerMask   = 0xFFFF;
    bool operator==(const CollisionModule&) const = default;
};
// Update: particles inside (or outside, `invert`) die. Emitter space.
struct KillVolumeModule {
    bool      sphere = false;
    glm::vec3 center{0.0f, -5.0f, 0.0f};
    glm::vec3 halfExtents{100.0f, 5.0f, 100.0f};
    float     radius = 1.0f;
    bool      invert = false;
    bool operator==(const KillVolumeModule&) const = default;
};

// Events: spawn particles of another emitter of the same effect where something happens to a
// particle of this one. Trail spawns `count` per second while the particle lives; spawned
// particles inherit the parent's position, a share of its velocity, and (trail) a ribbon id.
enum class ParticleEvent : std::uint8_t { Spawn, Death, Collision, Trail, Count };
struct SubEmitterModule {
    ParticleEvent event = ParticleEvent::Death;
    std::string   emitter{};
    std::uint32_t count           = 5;
    float         inheritVelocity = 0.0f;
    bool operator==(const SubEmitterModule&) const = default;
};

// Keep in sync with ParticleModuleType (same order).
using ParticleModuleParams =
    std::variant<SpawnRateModule, SpawnBurstModule, SpawnPerDistanceModule, ShapeModule, VelocityModule,
                 LifetimeModule, SizeModule, ColorModule, RotationModule, SpriteFrameModule, GravityModule, DragModule,
                 ForceModule, CurlNoiseModule, AttractorModule, SpeedLimitModule, SizeOverLifeModule,
                 ColorOverLifeModule, SubUvModule, CollisionModule, KillVolumeModule, SubEmitterModule>;

enum class ParticleModuleType : std::uint8_t {
    SpawnRate, SpawnBurst, SpawnPerDistance, Shape, Velocity, Lifetime, Size, Color, Rotation, SpriteFrame,
    Gravity, Drag, Force, CurlNoise, Attractor, SpeedLimit, SizeOverLife, ColorOverLife, SubUv, Collision,
    KillVolume, SubEmitter, Count
};
enum class ParticleStage : std::uint8_t { Spawn, Initialize, Update, Events, Count };

struct ParticleModule {
    ParticleModuleParams params;
    bool                 enabled = true;

    [[nodiscard]] ParticleModuleType Type() const { return static_cast<ParticleModuleType>(params.index()); }
    bool operator==(const ParticleModule&) const = default;
};

struct ParticleModuleInfo {
    const char*   name;    // file + UI ("SpawnRate")
    const char*   label;   // UI ("Spawn Rate")
    ParticleStage stage;
    const char*   description;
};
[[nodiscard]] const ParticleModuleInfo& ModuleInfo(ParticleModuleType type);
[[nodiscard]] const char* ToString(ParticleStage stage);
[[nodiscard]] const char* ToString(ParticleShape shape);
[[nodiscard]] const char* ToString(ParticleEvent event);
[[nodiscard]] ParticleModule MakeParticleModule(ParticleModuleType type); // defaults
[[nodiscard]] std::optional<ParticleModuleType> ParticleModuleTypeFromString(std::string_view name);

// --- Rendering ---

enum class ParticleRenderType : std::uint8_t { Sprite, Mesh, Ribbon, None, Count };
enum class ParticleBlend : std::uint8_t { Alpha, Additive, Count };
// Sprites: facing the camera, stretched along the velocity, or flat with a fixed world normal.
enum class ParticleFacing : std::uint8_t { Camera, Velocity, Axis, Count };
enum class RibbonUv : std::uint8_t { Stretch, Tile, Count };

[[nodiscard]] const char* ToString(ParticleRenderType type);
[[nodiscard]] const char* ToString(ParticleBlend blend);
[[nodiscard]] const char* ToString(ParticleFacing facing);
[[nodiscard]] const char* ToString(RibbonUv uv);

// Unlit: texture color x particle color (HDR, intensity > 1 glows through bloom).
struct ParticleRender {
    ParticleRenderType type  = ParticleRenderType::Sprite;
    ParticleBlend      blend = ParticleBlend::Alpha;
    std::string        texture{}; // absolute in memory, relative to the effect file on disk; empty: white
    std::uint32_t      columns = 1, rows = 1; // sprite sheet (flipbook)
    ParticleFacing     facing  = ParticleFacing::Camera;
    glm::vec3          axis{0.0f, 1.0f, 0.0f}; // Axis facing: sprite normal
    float              stretch = 0.1f;         // Velocity facing: extra length per m/s
    float              softness = 0.25f;       // fade where the sprite meets geometry (meters, 0: hard)
    float              cameraFade = 0.0f;      // fade out within this distance of the camera
    bool               sort = true;            // back to front (alpha)
    std::string        mesh{};                 // model file (absolute in memory)
    std::uint32_t      meshIndex = 0;
    bool               alignToVelocity = false; // mesh +Z along the velocity
    bool               shade = true;            // meshes: simple sun / ambient shading
    RibbonUv           ribbonUv   = RibbonUv::Stretch;
    float              ribbonTile = 1.0f;       // Tile: meters per texture repeat
    bool operator==(const ParticleRender&) const = default;
};

struct ParticleEmitter {
    std::string                 name    = "Emitter";
    bool                        enabled = true;
    std::uint32_t               maxParticles = 1000;
    bool                        localSpace   = false; // particles move with the entity
    float                       duration = 0.0f; // seconds of spawning, 0 = forever
    bool                        loop     = true;  // restart after `duration`
    float                       delay    = 0.0f;
    std::vector<ParticleModule> modules{};        // any order; executed stage by stage
    ParticleRender              render;
    bool operator==(const ParticleEmitter&) const = default;
};

struct ParticleEffect {
    std::vector<ParticleEmitter> emitters{};
    float                        warmup = 0.0f; // seconds simulated when the effect starts
    bool operator==(const ParticleEffect&) const = default;

    [[nodiscard]] const ParticleEmitter* Find(std::string_view name) const;
    [[nodiscard]] std::optional<std::size_t> IndexOf(std::string_view name) const;
};

// A starting point: one sprite emitter (rate, sphere, upward velocity, gravity, fade out).
[[nodiscard]] ParticleEffect MakeDefaultParticleEffect();
[[nodiscard]] ParticleEmitter MakeDefaultParticleEmitter(std::string name);

[[nodiscard]] bool IsParticleEffectFile(const std::filesystem::path& file); // by extension
// Through the VFS; texture / mesh paths come back absolute. Values are clamped to sane ranges.
[[nodiscard]] std::optional<ParticleEffect> LoadParticleEffect(const std::filesystem::path& file,
                                                               std::string* error = nullptr);
[[nodiscard]] std::optional<ParticleEffect> ParseParticleEffect(std::string_view json,
                                                                const std::filesystem::path& folder,
                                                                std::string* error = nullptr);
// Atomic write; paths below the effect's folder are stored relative.
bool SaveParticleEffect(const std::filesystem::path& file, const ParticleEffect& effect, std::string* error = nullptr);
[[nodiscard]] std::string ParticleEffectToJson(const ParticleEffect& effect, const std::filesystem::path& folder);
// Problems an author should know about: sub-emitter targets that do not exist / cycles, mesh
// renderers without a mesh, emitters without spawn or lifetime. One line each.
[[nodiscard]] std::vector<std::string> ValidateParticleEffect(const ParticleEffect& effect);

} // namespace Engine
