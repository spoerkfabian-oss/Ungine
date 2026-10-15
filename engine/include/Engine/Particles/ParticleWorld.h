#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Particles/ParticleEffect.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

class AssetManager;
class PhysicsWorld;
class Scene;
class ThreadPool;

// One particle as drawn (world space). Mirrors Particle in particle.glsl.
struct GpuParticle {
    glm::vec3 position{0.0f};
    float     rotation = 0.0f; // sprite roll (radians); meshes: angle about the spin axis
    glm::vec4 color{1.0f};
    glm::vec3 velocity{0.0f};  // meshes not aligned to the velocity: their spin axis
    float     frame = 0.0f;    // sprite sheet frame
    glm::vec3 size{0.1f};      // sprites: xy; meshes: scale; ribbons: x = width
    float     pad = 0.0f;
};
static_assert(sizeof(GpuParticle) == 64);

// One point of a ribbon (world space). Consecutive points with the same `ribbon` form a strip.
// Mirrors RibbonPoint in particle.glsl.
struct GpuRibbonPoint {
    glm::vec3     position{0.0f};
    float         width = 0.1f;
    glm::vec4     color{1.0f};
    float         u      = 0.0f; // texture coordinate along the ribbon
    std::uint32_t ribbon = 0;
    float         pad0 = 0.0f, pad1 = 0.0f;
};
static_assert(sizeof(GpuRibbonPoint) == 48);

// A draw of one emitter instance: particles [first, first + count) (sprites / meshes) or ribbon
// points (ribbons) of ParticleRenderData.
struct ParticleBatch {
    ParticleRender render;    // the emitter's settings (paths unused by the renderer)
    TextureHandle  texture;   // null: white
    ModelHandle    mesh;      // mesh particles
    std::uint32_t  first = 0, count = 0;
    glm::vec3      boundsMin{0.0f}, boundsMax{0.0f}; // world AABB incl. particle size
};

struct ParticleRenderData {
    std::vector<GpuParticle>    particles;
    std::vector<GpuRibbonPoint> ribbonPoints;
    std::vector<ParticleBatch>  batches;
};

struct ParticleWorldStats {
    std::uint32_t instances = 0; // effects playing (components + one-shots)
    std::uint32_t oneShots  = 0;
    std::uint32_t emitters  = 0; // emitter instances
    std::uint32_t particles = 0; // alive
    std::uint32_t spawned   = 0; // last Update
    std::uint32_t died      = 0;
    std::uint32_t collisionRays = 0;
    std::uint32_t effects   = 0; // effect files loaded
    double        simulateMs = 0.0;
};

// Simulates the scene's particle effects (ParticleSystem components) and one-shot effects on the
// CPU: emitter instances run their module stacks (spawn -> initialize -> update -> events) in
// parallel jobs on the ThreadPool (the main thread works along), collision modules raycast the
// PhysicsWorld, sub emitters spawn particles of other emitters of the same effect. Update also
// builds the render data (world space) that SceneRenderer draws. Main thread.
//
// Effects load through the VFS, are cached by path and reloaded when their file changes (checked
// 1/s; running instances restart). Textures / meshes are acquired from the AssetManager (without
// one, effects simulate but have nothing to draw with). Destroy the world before the AssetManager.
class ParticleWorld {
public:
    explicit ParticleWorld(ThreadPool* jobs = nullptr, AssetManager* assets = nullptr, PhysicsWorld* physics = nullptr);
    ~ParticleWorld();

    ParticleWorld(const ParticleWorld&)            = delete;
    ParticleWorld& operator=(const ParticleWorld&) = delete;

    // Per frame (after transforms / physics): new / changed / removed components (autoPlay starts
    // them), simulation by dt (x the component's speed), render data. Paused: render data only.
    void Update(Scene& scene, float dt);
    // Drops every instance (the scene was cleared / play mode toggled); components start again on
    // the next Update.
    void Reset();
    void SetPaused(bool paused);
    [[nodiscard]] bool Paused() const;

    // Gameplay / scripts.
    bool Play(Entity entity);                         // (re)start the entity's effect
    void Stop(Entity entity, bool immediate = false); // stop spawning (immediate: clear particles)
    [[nodiscard]] bool IsActive(Entity entity) const; // spawning or particles alive
    [[nodiscard]] std::uint32_t ParticleCount(Entity entity) const;
    // Fire-and-forget effect at a point; removed once finished (looping effects: by StopOneShot).
    // 0 if the effect failed to load.
    std::uint32_t SpawnAt(const std::string& effect, const glm::vec3& position, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
                          float scale = 1.0f);
    void StopOneShot(std::uint32_t id, bool immediate = false);
    [[nodiscard]] bool IsOneShotActive(std::uint32_t id) const;

    // Loaded (cached) effect, nullptr if missing / broken (logged once per change).
    [[nodiscard]] std::shared_ptr<const ParticleEffect> Effect(const std::string& path);
    // Editor: an unsaved version of an effect file (empty: back to the file). Instances restart.
    void Provide(const std::string& path, std::shared_ptr<const ParticleEffect> effect);
    void SetRandomSeed(std::uint32_t seed); // seeds of instances started afterwards (tests)

    [[nodiscard]] const ParticleRenderData& RenderData() const;
    [[nodiscard]] const ParticleWorldStats& Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Engine
