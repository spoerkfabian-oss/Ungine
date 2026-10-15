#include "Engine/Particles/ParticleWorld.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <numeric>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace Engine {

namespace {

constexpr float kEffectCheckSeconds = 1.0f;
constexpr float kWarmupStep         = 1.0f / 30.0f;
constexpr float kMaxStep            = 0.1f; // longer frames are split (stable forces / collisions)

// PCG32: small, fast, good enough; one stream per effect instance (deterministic per seed).
class Rng {
public:
    explicit Rng(std::uint64_t seed = 1) : m_State(seed * 6364136223846793005ull + 1442695040888963407ull) { Next(); }
    std::uint32_t Next()
    {
        const std::uint64_t old = m_State;
        m_State                 = old * 6364136223846793005ull + 1442695040888963407ull;
        const auto xorshifted   = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const auto rot          = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }
    float Unit() { return static_cast<float>(Next() >> 8) * (1.0f / 16777216.0f); } // [0, 1)
    float Range(float a, float b) { return a + (b - a) * Unit(); }
    float Range(const FloatRange& r) { return Range(r.min, r.max); }
    glm::vec3 Direction() // uniform on the unit sphere
    {
        const float z   = Range(-1.0f, 1.0f);
        const float phi = Unit() * glm::two_pi<float>();
        const float r   = std::sqrt(std::max(0.0f, 1.0f - z * z));
        return glm::vec3(r * std::cos(phi), z, r * std::sin(phi));
    }

private:
    std::uint64_t m_State;
};

// Random direction within `halfAngle` (radians) of `axis` (normalized).
glm::vec3 ConeDirection(Rng& rng, const glm::vec3& axis, float halfAngle)
{
    if (halfAngle <= 0.0f)
        return axis;
    const float     cosT = glm::mix(1.0f, std::cos(halfAngle), rng.Unit());
    const float     sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
    const float     phi  = rng.Unit() * glm::two_pi<float>();
    const glm::vec3 t    = std::abs(axis.y) < 0.99f ? glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)))
                                                    : glm::normalize(glm::cross(axis, glm::vec3(1, 0, 0)));
    const glm::vec3 b = glm::cross(axis, t);
    return axis * cosT + (t * std::cos(phi) + b * std::sin(phi)) * sinT;
}

// Curl of a sum-of-sines vector potential: smooth, divergence-free, cheap.
glm::vec3 CurlNoise(const glm::vec3& p, float t)
{
    glm::vec3 curl(0.0f);
    float     amplitude = 1.0f, scale = 1.0f;
    for (int octave = 0; octave < 2; ++octave) {
        const glm::vec3 q  = p * scale;
        const float     t1 = t, t2 = t * 0.73f + 1.7f, t3 = t * 1.31f + 4.1f;
        // A = (sin(a y + t1) cos(b z), sin(c z + t2) cos(d x), sin(e x + t3) cos(f y))
        constexpr float a = 1.0f, b = 1.3f, c = 1.1f, d = 0.9f, e = 1.2f, f = 1.4f;
        const float dAxdy = a * std::cos(a * q.y + t1) * std::cos(b * q.z);
        const float dAxdz = -b * std::sin(a * q.y + t1) * std::sin(b * q.z);
        const float dAydz = c * std::cos(c * q.z + t2) * std::cos(d * q.x);
        const float dAydx = -d * std::sin(c * q.z + t2) * std::sin(d * q.x);
        const float dAzdx = e * std::cos(e * q.x + t3) * std::cos(f * q.y);
        const float dAzdy = -f * std::sin(e * q.x + t3) * std::sin(f * q.y);
        curl += amplitude * glm::vec3(dAzdy - dAydz, dAxdz - dAzdx, dAydx - dAxdy);
        amplitude *= 0.5f;
        scale *= 2.1f;
    }
    return curl * 0.6f;
}

// Start position in the emitter's space + the direction "radial" speed pushes along.
std::pair<glm::vec3, glm::vec3> SampleShape(const ShapeModule& s, Rng& rng)
{
    glm::vec3 position(0.0f), radial(0.0f, 1.0f, 0.0f);
    switch (s.shape) {
    case ParticleShape::Point:
        radial = rng.Direction();
        break;
    case ParticleShape::Sphere:
    case ParticleShape::Hemisphere: {
        glm::vec3 dir = rng.Direction();
        if (s.shape == ParticleShape::Hemisphere)
            dir.y = std::abs(dir.y);
        const float r = s.surface ? s.radius : s.radius * std::cbrt(rng.Unit());
        position      = dir * r;
        radial        = dir;
        break;
    }
    case ParticleShape::Box: {
        const glm::vec3 h = s.halfExtents;
        position          = glm::vec3(rng.Range(-h.x, h.x), rng.Range(-h.y, h.y), rng.Range(-h.z, h.z));
        if (s.surface) { // a face, chosen by area
            const glm::vec3 area(h.y * h.z, h.x * h.z, h.x * h.y);
            const float     pick = rng.Unit() * (area.x + area.y + area.z);
            const int       axis = pick < area.x ? 0 : (pick < area.x + area.y ? 1 : 2);
            position[axis]       = rng.Unit() < 0.5f ? -h[axis] : h[axis];
        }
        const float len = glm::length(position);
        radial          = len > 1e-6f ? position / len : rng.Direction();
        break;
    }
    case ParticleShape::Cone:
    case ParticleShape::Circle: {
        const float phi = rng.Unit() * glm::two_pi<float>();
        const float r   = s.surface ? s.radius : s.radius * std::sqrt(rng.Unit());
        position        = glm::vec3(r * std::cos(phi), 0.0f, r * std::sin(phi));
        if (s.shape == ParticleShape::Circle) {
            radial = r > 1e-6f ? position / r : glm::vec3(std::cos(phi), 0.0f, std::sin(phi));
        } else { // through the base point from the apex below it: opens by `angle` at the rim
            const float tanAngle = std::tan(glm::radians(std::clamp(s.angle, 0.0f, 89.0f)));
            const float height   = s.radius > 0.0f && tanAngle > 1e-4f ? s.radius / tanAngle : 1.0f;
            radial               = glm::normalize(glm::vec3(position.x, height, position.z));
        }
        break;
    }
    case ParticleShape::Count:
        break;
    }
    return {position + s.offset, radial};
}

struct Particle {
    glm::vec3     position{0.0f}; // emitter space (localSpace) or world
    glm::vec3     velocity{0.0f};
    glm::vec3     size{0.1f};
    glm::vec4     color{1.0f};
    float         age = 0.0f, lifetime = 1.0f;
    float         rotation = 0.0f, rotationRate = 0.0f;
    float         frame   = 0.0f;
    float         trail   = 0.0f; // trail event accumulator
    std::uint32_t id      = 0;
    std::uint32_t ribbon  = 0;
    std::uint32_t hits    = 0;
    glm::vec3     axis{0.0f, 1.0f, 0.0f}; // mesh rotation axis
};

struct SpawnRequest {
    glm::vec3     position{0.0f}; // world
    glm::vec3     velocity{0.0f}; // world, already scaled by inheritVelocity
    std::uint32_t ribbon = 0;
    std::uint32_t count  = 0;
};

// The enabled modules of an emitter, sorted by stage.
struct CompiledEmitter {
    std::vector<const SpawnRateModule*>        rates;
    std::vector<std::pair<std::size_t, const SpawnBurstModule*>> bursts; // index into burst counters
    std::vector<const SpawnPerDistanceModule*> perDistance;
    std::vector<const ParticleModule*>         init;   // in file order
    std::vector<const ParticleModule*>         forces; // velocity / position changes, file order
    std::vector<const SizeOverLifeModule*>     sizeOverLife;
    std::vector<const ColorOverLifeModule*>    colorOverLife;
    const SubUvModule*                         subUv     = nullptr;
    const CollisionModule*                     collision = nullptr;
    std::vector<const KillVolumeModule*>       kills;
    std::vector<std::pair<std::size_t, const SubEmitterModule*>> events; // target emitter index
    std::size_t                                burstCount = 0;
};

CompiledEmitter Compile(const ParticleEffect& effect, const ParticleEmitter& e)
{
    CompiledEmitter c;
    for (const ParticleModule& m : e.modules) {
        if (!m.enabled)
            continue;
        std::visit(
            [&](const auto& p) {
                using T = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<T, SpawnRateModule>)
                    c.rates.push_back(&p);
                else if constexpr (std::is_same_v<T, SpawnBurstModule>)
                    c.bursts.emplace_back(c.burstCount++, &p);
                else if constexpr (std::is_same_v<T, SpawnPerDistanceModule>)
                    c.perDistance.push_back(&p);
                else if constexpr (std::is_same_v<T, ShapeModule> || std::is_same_v<T, VelocityModule> ||
                                   std::is_same_v<T, LifetimeModule> || std::is_same_v<T, SizeModule> ||
                                   std::is_same_v<T, ColorModule> || std::is_same_v<T, RotationModule> ||
                                   std::is_same_v<T, SpriteFrameModule>)
                    c.init.push_back(&m);
                else if constexpr (std::is_same_v<T, GravityModule> || std::is_same_v<T, DragModule> ||
                                   std::is_same_v<T, ForceModule> || std::is_same_v<T, CurlNoiseModule> ||
                                   std::is_same_v<T, AttractorModule> || std::is_same_v<T, SpeedLimitModule>)
                    c.forces.push_back(&m);
                else if constexpr (std::is_same_v<T, SizeOverLifeModule>)
                    c.sizeOverLife.push_back(&p);
                else if constexpr (std::is_same_v<T, ColorOverLifeModule>)
                    c.colorOverLife.push_back(&p);
                else if constexpr (std::is_same_v<T, SubUvModule>)
                    c.subUv = &p;
                else if constexpr (std::is_same_v<T, CollisionModule>)
                    c.collision = &p;
                else if constexpr (std::is_same_v<T, KillVolumeModule>)
                    c.kills.push_back(&p);
                else if constexpr (std::is_same_v<T, SubEmitterModule>) {
                    if (const auto target = effect.IndexOf(p.emitter))
                        c.events.emplace_back(*target, &p);
                }
            },
            m.params);
    }
    return c;
}

struct EmitterState {
    CompiledEmitter            modules;
    std::vector<Particle>      particles;
    std::vector<SpawnRequest>  pending;
    std::vector<std::uint32_t> burstsDone;
    float                      time       = 0.0f; // within the current loop
    float                      delayLeft  = 0.0f;
    float                      rateCarry  = 0.0f;
    float                      distanceCarry = 0.0f;
    bool                       spawning   = true;
    TextureHandle              texture;
    ModelHandle                mesh;
};

struct Counters {
    std::uint32_t spawned = 0, died = 0, rays = 0;
};

struct Instance {
    std::uint32_t                         oneShot = 0; // id, 0: a component's
    Entity                                entity  = NullEntity;
    std::string                           path;
    std::shared_ptr<const ParticleEffect> effect;
    std::uint64_t                         version = 0; // effect version this instance started with
    ParticleSystem                        settings;    // component copy (scales / tint / speed)
    glm::mat4                             world{1.0f};
    glm::mat4                             previousWorld{1.0f};
    glm::vec3                             emitterVelocity{0.0f};
    std::vector<EmitterState>             emitters;
    Rng                                   rng;
    std::uint32_t                         nextId   = 1;
    float                                 time     = 0.0f;
    float                                 warmupLeft = 0.0f;
    bool                                  stopping = false;
    bool                                  finished = false;
    bool                                  hasOutput = false;
    // Output of the last simulation (world space).
    std::vector<GpuParticle>    outParticles;
    std::vector<GpuRibbonPoint> outRibbon;
    std::vector<ParticleBatch>  outBatches;
    Counters                    counters;

    [[nodiscard]] std::uint32_t Alive() const
    {
        std::uint32_t n = 0;
        for (const EmitterState& s : emitters)
            n += static_cast<std::uint32_t>(s.particles.size());
        return n;
    }
};

glm::mat3 RotationOf(const glm::mat4& m)
{
    glm::mat3 r(m);
    for (int i = 0; i < 3; ++i) {
        const float len = glm::length(r[i]);
        r[i]            = len > 1e-8f ? r[i] / len : glm::vec3(0.0f);
    }
    return r;
}

// --- Simulation of one instance (job) ---

struct Simulator {
    Instance&           inst;
    const PhysicsWorld* physics;

    [[nodiscard]] const ParticleEffect& Effect() const { return *inst.effect; }

    void Spawn(std::size_t index, std::uint32_t count, const glm::mat4& emitterWorld, const glm::vec3* origin,
               const glm::vec3& inheritedVelocity, std::uint32_t ribbon, float ageOffset)
    {
        const ParticleEmitter& e = Effect().emitters[index];
        EmitterState&          s = inst.emitters[index];
        const ParticleSystem&  cfg = inst.settings;
        const glm::mat3        rotation = RotationOf(emitterWorld);
        const glm::mat4        toLocal  = e.localSpace ? glm::inverse(inst.world) : glm::mat4(1.0f);
        for (std::uint32_t n = 0; n < count && s.particles.size() < e.maxParticles; ++n) {
            Particle p;
            p.id     = inst.nextId++;
            p.ribbon = ribbon;
            glm::vec3 position(0.0f), velocity(0.0f), radial(0.0f, 1.0f, 0.0f);
            float     radialSpeed = 0.0f;
            for (const ParticleModule* m : s.modules.init) {
                std::visit(
                    [&](const auto& mod) {
                        using T = std::decay_t<decltype(mod)>;
                        if constexpr (std::is_same_v<T, ShapeModule>) {
                            std::tie(position, radial) = SampleShape(mod, inst.rng);
                        } else if constexpr (std::is_same_v<T, VelocityModule>) {
                            const float     len  = glm::length(mod.direction);
                            const glm::vec3 axis = len > 1e-6f ? mod.direction / len : glm::vec3(0, 1, 0);
                            velocity += ConeDirection(inst.rng, axis, glm::radians(mod.spread)) * inst.rng.Range(mod.speed);
                            radialSpeed += inst.rng.Range(mod.radial);
                            if (!e.localSpace)
                                velocity += glm::inverse(rotation) * inst.emitterVelocity * mod.inheritVelocity;
                        } else if constexpr (std::is_same_v<T, LifetimeModule>) {
                            p.lifetime = inst.rng.Range(mod.seconds);
                        } else if constexpr (std::is_same_v<T, SizeModule>) {
                            if (mod.perAxis)
                                p.size = glm::vec3(inst.rng.Range(mod.axisMin.x, mod.axisMax.x),
                                                   inst.rng.Range(mod.axisMin.y, mod.axisMax.y),
                                                   inst.rng.Range(mod.axisMin.z, mod.axisMax.z));
                            else
                                p.size = glm::vec3(inst.rng.Range(mod.size));
                        } else if constexpr (std::is_same_v<T, ColorModule>) {
                            const glm::vec4 t(inst.rng.Unit(), inst.rng.Unit(), inst.rng.Unit(), inst.rng.Unit());
                            p.color = glm::mix(mod.colorA, mod.colorB, t) * glm::vec4(glm::vec3(mod.intensity), 1.0f);
                        } else if constexpr (std::is_same_v<T, RotationModule>) {
                            p.rotation     = glm::radians(inst.rng.Range(mod.angle));
                            p.rotationRate = glm::radians(inst.rng.Range(mod.rate));
                            p.axis         = inst.rng.Direction();
                        } else if constexpr (std::is_same_v<T, SpriteFrameModule>) {
                            if (mod.random) {
                                const auto frames = std::max(1u, e.render.columns * e.render.rows);
                                p.frame           = static_cast<float>(inst.rng.Next() % frames);
                            }
                        }
                    },
                    m->params);
            }
            velocity += radial * radialSpeed;
            p.lifetime = std::max(p.lifetime * std::max(cfg.lifetimeScale, 0.0f), 1e-3f);
            // Emitter space -> world (entity transform; events: around the parent particle).
            glm::vec3 worldPosition = origin ? *origin + rotation * position : glm::vec3(emitterWorld * glm::vec4(position, 1.0f));
            glm::vec3 worldVelocity = rotation * velocity * cfg.velocityScale + inheritedVelocity;
            if (e.localSpace) {
                p.position = glm::vec3(toLocal * glm::vec4(worldPosition, 1.0f));
                p.velocity = glm::mat3(toLocal) * worldVelocity;
            } else {
                p.position = worldPosition;
                p.velocity = worldVelocity;
            }
            // Spawned during the frame: advance by the part of the frame already gone.
            p.age = std::min(ageOffset, p.lifetime * 0.99f);
            p.position += p.velocity * p.age;
            s.particles.push_back(p);
            ++inst.counters.spawned;
            for (const auto& [target, sub] : s.modules.events)
                if (sub->event == ParticleEvent::Spawn)
                    Request(target, *sub, p, e.localSpace);
        }
    }

    [[nodiscard]] glm::vec3 WorldPosition(const ParticleEmitter& e, const glm::vec3& p) const
    {
        return e.localSpace ? glm::vec3(inst.world * glm::vec4(p, 1.0f)) : p;
    }
    [[nodiscard]] glm::vec3 WorldVector(const ParticleEmitter& e, const glm::vec3& v) const
    {
        return e.localSpace ? glm::mat3(inst.world) * v : v;
    }

    void Request(std::size_t target, const SubEmitterModule& sub, const Particle& p, bool localSpace, std::uint32_t count = 0)
    {
        if (!Effect().emitters[target].enabled)
            return;
        SpawnRequest r;
        r.position = localSpace ? glm::vec3(inst.world * glm::vec4(p.position, 1.0f)) : p.position;
        r.velocity = (localSpace ? glm::mat3(inst.world) * p.velocity : p.velocity) * sub.inheritVelocity;
        r.ribbon   = sub.event == ParticleEvent::Trail ? p.id : 0u;
        r.count    = count > 0 ? count : sub.count;
        if (r.count > 0)
            inst.emitters[target].pending.push_back(r);
    }

    void UpdateParticles(std::size_t index, float dt)
    {
        const ParticleEmitter& e = Effect().emitters[index];
        EmitterState&          s = inst.emitters[index];
        const CompiledEmitter& c = s.modules;
        const glm::mat3        worldToLocal = e.localSpace ? glm::inverse(glm::mat3(inst.world)) : glm::mat3(1.0f);
        const glm::mat4        worldInverse = glm::inverse(inst.world);
        const glm::mat3        entityRotation = RotationOf(inst.world);
        for (std::size_t i = 0; i < s.particles.size();) {
            Particle& p = s.particles[i];
            p.age += dt;
            bool dies = p.age >= p.lifetime;
            if (!dies) {
                for (const ParticleModule* m : c.forces) {
                    std::visit(
                        [&](const auto& mod) {
                            using T = std::decay_t<decltype(mod)>;
                            if constexpr (std::is_same_v<T, GravityModule>) {
                                p.velocity += worldToLocal * mod.acceleration * dt;
                            } else if constexpr (std::is_same_v<T, DragModule>) {
                                p.velocity *= std::exp(-mod.coefficient * dt);
                            } else if constexpr (std::is_same_v<T, ForceModule>) {
                                const glm::vec3 a = mod.local ? (e.localSpace ? mod.acceleration : entityRotation * mod.acceleration)
                                                              : worldToLocal * mod.acceleration;
                                p.velocity += a * dt;
                            } else if constexpr (std::is_same_v<T, CurlNoiseModule>) {
                                const glm::vec3 q = WorldPosition(e, p.position) * mod.frequency;
                                p.velocity += worldToLocal * CurlNoise(q, inst.time * mod.scroll) * mod.strength * dt;
                            } else if constexpr (std::is_same_v<T, AttractorModule>) {
                                const glm::vec3 target = e.localSpace ? mod.position : glm::vec3(inst.world * glm::vec4(mod.position, 1.0f));
                                const glm::vec3 delta  = target - p.position;
                                const float     dist   = glm::length(delta);
                                if (mod.killRadius > 0.0f && dist < mod.killRadius)
                                    dies = true;
                                else if (dist > 1e-5f) {
                                    float falloff = 1.0f;
                                    if (mod.radius > 0.0f)
                                        falloff = std::max(0.0f, 1.0f - dist / mod.radius);
                                    p.velocity += delta / dist * mod.strength * falloff * dt;
                                }
                            } else if constexpr (std::is_same_v<T, SpeedLimitModule>) {
                                const float speed = glm::length(p.velocity);
                                if (speed > mod.maxSpeed && speed > 0.0f)
                                    p.velocity *= mod.maxSpeed / speed;
                            }
                        },
                        m->params);
                }
            }
            if (!dies) {
                const glm::vec3 from = p.position;
                p.position += p.velocity * dt;
                if (c.collision && physics)
                    dies = Collide(e, s, p, from, *c.collision);
            }
            if (!dies) {
                for (const KillVolumeModule* k : c.kills) {
                    const glm::vec3 local = e.localSpace ? p.position : glm::vec3(worldInverse * glm::vec4(p.position, 1.0f));
                    const glm::vec3 d     = local - k->center;
                    const bool inside = k->sphere ? glm::dot(d, d) <= k->radius * k->radius
                                                  : glm::all(glm::lessThanEqual(glm::abs(d), k->halfExtents));
                    if (inside != k->invert) {
                        dies = true;
                        break;
                    }
                }
            }
            p.rotation += p.rotationRate * dt;
            if (!dies) {
                for (const auto& [target, sub] : c.events) {
                    if (sub->event != ParticleEvent::Trail)
                        continue;
                    p.trail += static_cast<float>(sub->count) * dt;
                    const auto whole = static_cast<std::uint32_t>(p.trail);
                    if (whole > 0) {
                        p.trail -= static_cast<float>(whole);
                        Request(target, *sub, p, e.localSpace, whole);
                    }
                }
                ++i;
                continue;
            }
            for (const auto& [target, sub] : c.events)
                if (sub->event == ParticleEvent::Death)
                    Request(target, *sub, p, e.localSpace);
            ++inst.counters.died;
            p = s.particles.back(); // swap-remove (ribbons re-sort by id)
            s.particles.pop_back();
        }
    }

    // True: the particle dies.
    bool Collide(const ParticleEmitter& e, EmitterState& s, Particle& p, const glm::vec3& fromLocal, const CollisionModule& c)
    {
        const glm::vec3 from  = WorldPosition(e, fromLocal);
        const glm::vec3 to    = WorldPosition(e, p.position);
        const glm::vec3 delta = to - from;
        const float     len   = glm::length(delta);
        if (len < 1e-6f)
            return false;
        const float radius = std::max(p.size.x * inst.settings.sizeScale * c.radiusScale, 0.0f);
        const glm::vec3 dir = delta / len;
        ++inst.counters.rays;
        const auto hit = physics->Raycast(from, dir, len + radius, inst.entity, c.layerMask);
        if (!hit)
            return false;
        glm::vec3       velocity = WorldVector(e, p.velocity);
        const glm::vec3 n        = hit->normal;
        const glm::vec3 position = from + dir * std::max(hit->distance - radius, 0.0f) + n * 1e-3f;
        const float     vn       = glm::dot(velocity, n);
        if (vn < 0.0f) {
            const glm::vec3 normal  = vn * n;
            const glm::vec3 tangent = velocity - normal;
            velocity = tangent * (1.0f - c.friction) - normal * c.restitution;
        }
        if (e.localSpace) {
            const glm::mat4 toLocal = glm::inverse(inst.world);
            p.position = glm::vec3(toLocal * glm::vec4(position, 1.0f));
            p.velocity = glm::mat3(toLocal) * velocity;
        } else {
            p.position = position;
            p.velocity = velocity;
        }
        ++p.hits;
        for (const auto& [target, sub] : s.modules.events)
            if (sub->event == ParticleEvent::Collision)
                Request(target, *sub, p, e.localSpace);
        return c.kill || (c.maxHits > 0 && p.hits >= c.maxHits);
    }

    void SpawnEmitter(std::size_t index, float dt)
    {
        const ParticleEmitter& e = Effect().emitters[index];
        EmitterState&          s = inst.emitters[index];
        const CompiledEmitter& c = s.modules;
        // Sub-emitter requests (from earlier emitters this frame or later ones last frame).
        for (const SpawnRequest& r : s.pending)
            Spawn(index, r.count, inst.world, &r.position, r.velocity, r.ribbon, 0.0f);
        s.pending.clear();
        if (!s.spawning || inst.stopping)
            return;
        float window = dt;
        if (s.delayLeft > 0.0f) {
            s.delayLeft -= dt;
            if (s.delayLeft > 0.0f)
                return;
            window = -s.delayLeft;
            s.delayLeft = 0.0f;
        }
        const float t0     = s.time;
        const float t1     = t0 + window;
        float       count  = 0.0f;
        const float curveT = e.duration > 0.0f ? std::clamp(t0 / e.duration, 0.0f, 1.0f) : 0.0f;
        for (const SpawnRateModule* rate : c.rates)
            count += rate->rate * rate->scale.Evaluate(curveT) * window;
        const glm::vec3 moved = glm::vec3(inst.world[3]) - glm::vec3(inst.previousWorld[3]);
        for (const SpawnPerDistanceModule* d : c.perDistance)
            s.distanceCarry += glm::length(moved) * d->perMeter;
        count *= std::max(inst.settings.spawnScale, 0.0f);
        s.rateCarry += count;
        auto continuous = static_cast<std::uint32_t>(s.rateCarry);
        s.rateCarry -= static_cast<float>(continuous);
        const auto distanceCount = static_cast<std::uint32_t>(s.distanceCarry);
        s.distanceCarry -= static_cast<float>(distanceCount);
        continuous += distanceCount;
        // Continuous spawns are spread along the emitter's motion and over the frame.
        for (std::uint32_t n = 0; n < continuous; ++n) {
            const float f = (static_cast<float>(n) + 1.0f) / static_cast<float>(continuous);
            glm::mat4   at = inst.world;
            at[3]          = glm::mix(inst.previousWorld[3], inst.world[3], f);
            Spawn(index, 1, at, nullptr, glm::vec3(0.0f), 0, (1.0f - f) * window);
        }
        for (const auto& [slot, burst] : c.bursts) {
            std::uint32_t& done = s.burstsDone[slot];
            while (burst->cycles == 0 || done < burst->cycles) {
                const float at = burst->time + static_cast<float>(done) * burst->interval;
                if (at >= t1 || (burst->cycles == 0 && burst->interval <= 0.0f))
                    break;
                std::uint32_t n = burst->count;
                if (burst->countMax > burst->count)
                    n += inst.rng.Next() % (burst->countMax - burst->count + 1);
                n = static_cast<std::uint32_t>(static_cast<float>(n) * std::max(inst.settings.spawnScale, 0.0f) + 0.5f);
                Spawn(index, n, inst.world, nullptr, glm::vec3(0.0f), 0, std::max(t1 - at, 0.0f));
                ++done;
            }
        }
        s.time = t1;
        if (e.duration > 0.0f && s.time >= e.duration) {
            if (e.loop) {
                s.time = std::fmod(s.time - e.duration, e.duration);
                std::ranges::fill(s.burstsDone, 0u);
            } else
                s.spawning = false;
        }
    }

    void Step(float dt)
    {
        inst.time += dt;
        const std::size_t count = Effect().emitters.size();
        for (std::size_t i = 0; i < count; ++i) {
            if (!Effect().emitters[i].enabled)
                continue;
            UpdateParticles(i, dt);
            SpawnEmitter(i, dt);
        }
    }

    void Simulate(float dt)
    {
        while (inst.warmupLeft > 0.0f) {
            const float step = std::min(inst.warmupLeft, kWarmupStep);
            inst.warmupLeft -= step;
            inst.previousWorld = inst.world;
            Step(step);
        }
        // Long frames in steps (the emitter is interpolated over the whole frame by Spawn).
        const int steps = std::max(1, static_cast<int>(std::ceil(dt / kMaxStep)));
        for (int i = 0; i < steps; ++i)
            Step(dt / static_cast<float>(steps));
        // Finished: nothing alive, nothing queued, no emitter that can still spawn.
        bool done = true;
        for (std::size_t i = 0; i < inst.emitters.size() && done; ++i) {
            const EmitterState&    s = inst.emitters[i];
            const ParticleEmitter& e = Effect().emitters[i];
            if (!s.particles.empty() || !s.pending.empty())
                done = false;
            else if (e.enabled && s.spawning && !inst.stopping) {
                const bool loops = e.loop && e.duration > 0.0f; // bursts come again with the next loop
                bool       more  = !s.modules.rates.empty() || !s.modules.perDistance.empty() || (loops && !s.modules.bursts.empty());
                for (const auto& [slot, burst] : s.modules.bursts)
                    more = more || burst->cycles == 0 || s.burstsDone[slot] < burst->cycles;
                done = !more;
            }
        }
        inst.finished = done;
    }

    void BuildOutput()
    {
        inst.outParticles.clear();
        inst.outRibbon.clear();
        inst.outBatches.clear();
        const ParticleSystem& cfg = inst.settings;
        for (std::size_t index = 0; index < inst.emitters.size(); ++index) {
            const ParticleEmitter& e = Effect().emitters[index];
            const EmitterState&    s = inst.emitters[index];
            if (!e.enabled || e.render.type == ParticleRenderType::None || s.particles.empty())
                continue;
            const CompiledEmitter& c = s.modules;
            ParticleBatch          batch;
            batch.render = e.render;
            batch.render.texture.clear();
            batch.render.mesh.clear();
            batch.texture = s.texture;
            batch.mesh    = s.mesh;
            const bool ribbon = e.render.type == ParticleRenderType::Ribbon;
            batch.first       = static_cast<std::uint32_t>(ribbon ? inst.outRibbon.size() : inst.outParticles.size());
            glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
            const float frames = static_cast<float>(std::max(1u, e.render.columns * e.render.rows));
            const auto  shade  = [&](const Particle& p) {
                GpuParticle g;
                const float t = std::clamp(p.age / p.lifetime, 0.0f, 1.0f);
                g.position    = WorldPosition(e, p.position);
                g.velocity    = WorldVector(e, p.velocity);
                g.rotation    = p.rotation;
                glm::vec4 color = p.color * cfg.tint;
                for (const ColorOverLifeModule* m : c.colorOverLife)
                    color *= m->gradient.Evaluate(t);
                glm::vec3 size = p.size * cfg.sizeScale;
                for (const SizeOverLifeModule* m : c.sizeOverLife)
                    size *= m->curve.Evaluate(t);
                if (e.localSpace) // the entity's scale applies to local effects
                    size *= glm::length(glm::vec3(inst.world[0]));
                g.color = color;
                g.size  = size;
                float frame = p.frame;
                if (c.subUv)
                    frame += c.subUv->overLife ? std::floor(t * c.subUv->cycles * frames) : std::floor(p.age * c.subUv->fps);
                g.frame = std::fmod(frame, frames);
                const float extent = std::max({size.x, size.y, size.z}) * (e.render.facing == ParticleFacing::Velocity ? 1.0f + e.render.stretch * glm::length(g.velocity) : 1.0f);
                lo = glm::min(lo, g.position - glm::vec3(extent));
                hi = glm::max(hi, g.position + glm::vec3(extent));
                return std::pair{g, p.axis};
            };
            if (!ribbon) {
                for (const Particle& p : s.particles) {
                    auto [g, axis] = shade(p);
                    // Meshes not aligned to the velocity spin about a random axis: it rides in `velocity`.
                    if (e.render.type == ParticleRenderType::Mesh && !e.render.alignToVelocity)
                        g.velocity = axis;
                    inst.outParticles.push_back(g);
                }
                batch.count = static_cast<std::uint32_t>(inst.outParticles.size()) - batch.first;
            } else {
                // Ribbons: per ribbon id, oldest first (spawn order).
                std::vector<std::uint32_t> order(s.particles.size());
                std::iota(order.begin(), order.end(), 0u);
                std::ranges::sort(order, [&](std::uint32_t a, std::uint32_t b) {
                    const Particle& pa = s.particles[a];
                    const Particle& pb = s.particles[b];
                    return pa.ribbon != pb.ribbon ? pa.ribbon < pb.ribbon : pa.id < pb.id;
                });
                for (std::size_t i = 0; i < order.size();) {
                    std::size_t end = i;
                    while (end < order.size() && s.particles[order[end]].ribbon == s.particles[order[i]].ribbon)
                        ++end;
                    if (end - i >= 2) {
                        float distance = 0.0f;
                        glm::vec3 last(0.0f);
                        for (std::size_t k = i; k < end; ++k) {
                            const auto [g, axis] = shade(s.particles[order[k]]);
                            if (k > i)
                                distance += glm::length(g.position - last);
                            last = g.position;
                            GpuRibbonPoint point;
                            point.position = g.position;
                            point.width    = g.size.x;
                            point.color    = g.color;
                            point.u        = e.render.ribbonUv == RibbonUv::Tile
                                                 ? distance / e.render.ribbonTile
                                                 : static_cast<float>(k - i) / static_cast<float>(end - i - 1);
                            point.ribbon   = s.particles[order[i]].ribbon + static_cast<std::uint32_t>(index << 24);
                            inst.outRibbon.push_back(point);
                        }
                    }
                    i = end;
                }
                batch.count = static_cast<std::uint32_t>(inst.outRibbon.size()) - batch.first;
            }
            if (batch.count == 0)
                continue;
            batch.boundsMin = lo;
            batch.boundsMax = hi;
            inst.outBatches.push_back(std::move(batch));
        }
        inst.hasOutput = true;
    }
};

std::uint64_t Id(Entity e) { return static_cast<std::uint64_t>(e); }

std::string Key(const std::string& path)
{
    return path.empty() ? path : PathToUtf8(PathFromUtf8(path).lexically_normal());
}

// Runs `work(i)` for i in [0, count) on the pool and the calling thread; returns when all are done.
// Late pool jobs find nothing left and only touch the shared context.
void ParallelFor(ThreadPool* pool, std::size_t count, const std::function<void(std::size_t)>& work)
{
    if (count == 0)
        return;
    if (!pool || pool->ThreadCount() == 0 || count == 1) {
        for (std::size_t i = 0; i < count; ++i)
            work(i);
        return;
    }
    struct Context {
        std::atomic<std::size_t>               next{0};
        std::atomic<std::size_t>               done{0};
        std::size_t                            count = 0;
        const std::function<void(std::size_t)>* work = nullptr;
        std::mutex                             mutex;
        std::condition_variable                finished;
        std::exception_ptr                     error;
        void Run()
        {
            for (std::size_t i; (i = next.fetch_add(1)) < count;) {
                try {
                    (*work)(i);
                } catch (...) {
                    const std::lock_guard lock(mutex);
                    if (!error)
                        error = std::current_exception();
                }
                if (done.fetch_add(1) + 1 == count) {
                    const std::lock_guard lock(mutex);
                    finished.notify_all();
                }
            }
        }
    };
    auto ctx   = std::make_shared<Context>();
    ctx->count = count;
    ctx->work  = &work;
    const std::size_t helpers = std::min<std::size_t>(pool->ThreadCount(), count - 1);
    for (std::size_t h = 0; h < helpers; ++h)
        pool->Enqueue([ctx] { ctx->Run(); });
    ctx->Run();
    std::unique_lock lock(ctx->mutex);
    ctx->finished.wait(lock, [&] { return ctx->done.load() == ctx->count; });
    ctx->work = nullptr; // late helpers never call it (next >= count)
    if (ctx->error)
        std::rethrow_exception(ctx->error);
}

} // namespace

struct ParticleWorld::Impl {
    ThreadPool*   jobs;
    AssetManager* assets;
    PhysicsWorld* physics;

    struct EffectRef {
        std::shared_ptr<const ParticleEffect> effect;
        std::shared_ptr<const ParticleEffect> provided; // editor override
        std::filesystem::file_time_type       time{};
        std::uint64_t                         version = 1;
        bool                                  failed  = false;
    };
    std::unordered_map<std::string, EffectRef>     effects;
    std::unordered_map<std::string, TextureHandle> textures;
    std::unordered_map<std::string, ModelHandle>   models;
    std::vector<std::unique_ptr<Instance>>         instances;
    std::unordered_map<std::uint64_t, std::size_t> byEntity; // entity value -> instance index (rebuilt per Update)
    std::unordered_set<std::uint64_t>              playRequests;
    std::uint32_t                                  nextOneShot = 1;
    std::uint64_t                                  seedCounter = 0;
    float                                          clock = 0.0f, nextCheck = 0.0f;
    bool                                           paused = false;
    ParticleRenderData                             render;
    ParticleWorldStats                             stats;

    Impl(ThreadPool* j, AssetManager* a, PhysicsWorld* p) : jobs(j), assets(a), physics(p)
    {
        seedCounter = std::random_device{}();
    }

    ~Impl()
    {
        if (assets) {
            for (auto& [path, handle] : textures)
                if (handle)
                    assets->Release(handle);
            for (auto& [path, handle] : models)
                if (handle)
                    assets->Release(handle);
        }
    }

    void Load(const std::string& key, EffectRef& ref)
    {
        const std::filesystem::path file = PathFromUtf8(key);
        ref.time = Vfs::ModifiedTime(file);
        std::string error;
        if (auto effect = LoadParticleEffect(file, &error)) {
            ref.failed = false;
            if (ref.effect && *ref.effect == *effect)
                return; // touched, not changed (e.g. saved from the editor): nothing restarts
            ref.effect = std::make_shared<const ParticleEffect>(std::move(*effect));
            if (!ref.provided) // an editor version overrides the file: what plays did not change
                ++ref.version;
        } else {
            if (!ref.failed)
                ENGINE_WARN("Particles: {}", error);
            ref.failed = true; // a broken edit keeps the last good version
        }
    }

    EffectRef* Find(const std::string& key)
    {
        if (key.empty())
            return nullptr;
        auto [it, inserted] = effects.try_emplace(key);
        if (inserted)
            Load(key, it->second);
        EffectRef& ref = it->second;
        return ref.provided || ref.effect ? &ref : nullptr;
    }

    static std::shared_ptr<const ParticleEffect> Current(const EffectRef& ref)
    {
        return ref.provided ? ref.provided : ref.effect;
    }

    void CheckFiles()
    {
        if (clock < nextCheck)
            return;
        nextCheck = clock + kEffectCheckSeconds;
        for (auto& [key, ref] : effects)
            if (Vfs::ModifiedTime(PathFromUtf8(key)) != ref.time)
                Load(key, ref);
    }

    TextureHandle Texture(const std::string& path)
    {
        if (!assets || path.empty())
            return {};
        auto [it, inserted] = textures.try_emplace(Key(path));
        if (inserted)
            it->second = assets->LoadTexture(PathFromUtf8(it->first), TextureKind::Color);
        return it->second;
    }

    ModelHandle Model(const std::string& path)
    {
        if (!assets || path.empty())
            return {};
        auto [it, inserted] = models.try_emplace(Key(path));
        if (inserted)
            it->second = assets->LoadModel(PathFromUtf8(it->first));
        return it->second;
    }

    std::uint32_t NextSeed(std::uint32_t requested)
    {
        if (requested != 0)
            return requested;
        seedCounter = seedCounter * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(seedCounter >> 33) | 1u;
    }

    // (Re)starts an instance with its current effect version.
    bool Start(Instance& inst)
    {
        EffectRef* ref = Find(inst.path);
        if (!ref)
            return false;
        inst.effect   = Current(*ref);
        inst.version  = ref->version;
        inst.rng      = Rng(NextSeed(inst.settings.seed));
        inst.nextId   = 1;
        inst.time     = 0.0f;
        inst.stopping = false;
        inst.finished = false;
        inst.warmupLeft = inst.effect->warmup;
        inst.previousWorld = inst.world;
        inst.emitterVelocity = glm::vec3(0.0f);
        inst.emitters.clear();
        inst.outParticles.clear();
        inst.outRibbon.clear();
        inst.outBatches.clear();
        inst.hasOutput = false;
        for (const ParticleEmitter& e : inst.effect->emitters) {
            EmitterState s;
            s.modules    = Compile(*inst.effect, e);
            s.burstsDone.assign(s.modules.burstCount, 0u);
            s.delayLeft  = e.delay;
            s.particles.reserve(std::min<std::uint32_t>(e.maxParticles, 256u));
            s.texture    = Texture(e.render.texture);
            if (e.render.type == ParticleRenderType::Mesh)
                s.mesh = Model(e.render.mesh);
            inst.emitters.push_back(std::move(s));
        }
        return true;
    }

    Instance* ByEntity(Entity e)
    {
        const auto it = byEntity.find(Id(e));
        return it == byEntity.end() ? nullptr : instances[it->second].get();
    }
    const Instance* ByEntity(Entity e) const
    {
        const auto it = byEntity.find(Id(e));
        return it == byEntity.end() ? nullptr : instances[it->second].get();
    }

    void SyncComponents(Scene& scene)
    {
        Registry&                          r = scene.GetRegistry();
        std::unordered_set<std::uint64_t>  seen;
        std::vector<std::unique_ptr<Instance>> created;
        r.ViewOf<ParticleSystem>().Each([&](Entity e, ParticleSystem& config) {
            seen.insert(Id(e));
            const WorldTransform* world = r.TryGet<WorldTransform>(e);
            Instance*             inst  = ByEntity(e);
            const std::string     key   = Key(config.effect);
            const bool requested = playRequests.erase(Id(e)) > 0;
            if (!inst) {
                if (key.empty() || (!config.autoPlay && !requested))
                    return;
                auto fresh      = std::make_unique<Instance>();
                fresh->entity   = e;
                fresh->path     = key;
                fresh->settings = config;
                fresh->world    = world ? world->matrix : glm::mat4(1.0f);
                if (Start(*fresh))
                    created.push_back(std::move(fresh));
                return;
            }
            const bool restart = inst->path != key || inst->settings.seed != config.seed || requested;
            inst->settings = config;
            if (world)
                inst->world = world->matrix;
            if (restart) {
                inst->path = key;
                if (!Start(*inst)) {
                    inst->effect.reset();
                    inst->emitters.clear();
                    inst->outParticles.clear();
                    inst->outRibbon.clear();
                    inst->outBatches.clear();
                    inst->hasOutput = false;
                    inst->finished  = true;
                }
            } else if (EffectRef* ref = Find(inst->path); ref && ref->version != inst->version && !inst->finished) {
                Start(*inst); // the file changed: restart with it
            }
        });
        // Removed components / entities (one-shots stay).
        std::erase_if(instances, [&](const std::unique_ptr<Instance>& inst) {
            return inst->oneShot == 0 && !seen.contains(Id(inst->entity));
        });
        for (auto& inst : created)
            instances.push_back(std::move(inst));
        RebuildIndex();
        playRequests.clear();
    }

    void RebuildIndex()
    {
        byEntity.clear();
        for (std::size_t i = 0; i < instances.size(); ++i)
            if (instances[i]->oneShot == 0)
                byEntity[Id(instances[i]->entity)] = i;
    }

    void Update(Scene& scene, float dt)
    {
        clock += dt;
        CheckFiles();
        SyncComponents(scene);
        stats = {};
        const auto start = std::chrono::steady_clock::now();
        if (!paused && dt > 0.0f) {
            std::vector<Instance*> active;
            for (auto& inst : instances) {
                if (!inst->effect)
                    continue;
                const float step = dt * std::max(inst->settings.speed, 0.0f);
                inst->emitterVelocity = step > 0.0f ? (glm::vec3(inst->world[3]) - glm::vec3(inst->previousWorld[3])) / step
                                                    : glm::vec3(0.0f);
                inst->counters = {};
                active.push_back(inst.get());
            }
            const PhysicsWorld* phys = physics;
            ParallelFor(jobs, active.size(), [&](std::size_t i) {
                Instance&   inst = *active[i];
                const float step = dt * std::max(inst.settings.speed, 0.0f);
                Simulator   sim{inst, phys};
                if (step > 0.0f && !inst.finished)
                    sim.Simulate(step);
                sim.BuildOutput();
            });
            for (Instance* inst : active) {
                inst->previousWorld = inst->world;
                stats.spawned += inst->counters.spawned;
                stats.died += inst->counters.died;
                stats.collisionRays += inst->counters.rays;
            }
            // Finished one-shots go away.
            std::erase_if(instances, [](const std::unique_ptr<Instance>& inst) { return inst->oneShot != 0 && inst->finished; });
            RebuildIndex();
        }
        stats.simulateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        Gather();
    }

    void Gather()
    {
        render.particles.clear();
        render.ribbonPoints.clear();
        render.batches.clear();
        for (const auto& inst : instances) {
            stats.instances += 1;
            stats.oneShots += inst->oneShot != 0 ? 1u : 0u;
            stats.emitters += static_cast<std::uint32_t>(inst->emitters.size());
            stats.particles += inst->Alive();
            if (!inst->hasOutput)
                continue;
            const auto particleBase = static_cast<std::uint32_t>(render.particles.size());
            const auto ribbonBase   = static_cast<std::uint32_t>(render.ribbonPoints.size());
            render.particles.insert(render.particles.end(), inst->outParticles.begin(), inst->outParticles.end());
            render.ribbonPoints.insert(render.ribbonPoints.end(), inst->outRibbon.begin(), inst->outRibbon.end());
            for (ParticleBatch batch : inst->outBatches) {
                batch.first += batch.render.type == ParticleRenderType::Ribbon ? ribbonBase : particleBase;
                render.batches.push_back(batch);
            }
        }
        stats.effects = static_cast<std::uint32_t>(std::ranges::count_if(effects, [](const auto& e) { return e.second.effect != nullptr; }));
    }
};

ParticleWorld::ParticleWorld(ThreadPool* jobs, AssetManager* assets, PhysicsWorld* physics)
    : m_Impl(std::make_unique<Impl>(jobs, assets, physics))
{
}

ParticleWorld::~ParticleWorld() = default;

void ParticleWorld::Update(Scene& scene, float dt) { m_Impl->Update(scene, dt); }

void ParticleWorld::Reset()
{
    m_Impl->instances.clear();
    m_Impl->byEntity.clear();
    m_Impl->playRequests.clear();
    m_Impl->stats = {};
    m_Impl->Gather();
}

void ParticleWorld::SetPaused(bool paused) { m_Impl->paused = paused; }
bool ParticleWorld::Paused() const { return m_Impl->paused; }

bool ParticleWorld::Play(Entity entity)
{
    if (Instance* inst = m_Impl->ByEntity(entity)) {
        if (!m_Impl->Start(*inst))
            return false;
        return true;
    }
    m_Impl->playRequests.insert(Id(entity)); // created by the next Update
    return true;
}

void ParticleWorld::Stop(Entity entity, bool immediate)
{
    m_Impl->playRequests.erase(Id(entity));
    Instance* inst = m_Impl->ByEntity(entity);
    if (!inst)
        return;
    inst->stopping = true;
    if (immediate) {
        for (EmitterState& s : inst->emitters) {
            s.particles.clear();
            s.pending.clear();
        }
        inst->outParticles.clear();
        inst->outRibbon.clear();
        inst->outBatches.clear();
        inst->finished = true;
    }
}

bool ParticleWorld::IsActive(Entity entity) const
{
    if (m_Impl->playRequests.contains(Id(entity)))
        return true;
    const Instance* inst = m_Impl->ByEntity(entity);
    return inst && inst->effect && !inst->finished;
}

std::uint32_t ParticleWorld::ParticleCount(Entity entity) const
{
    const Instance* inst = m_Impl->ByEntity(entity);
    return inst ? inst->Alive() : 0u;
}

std::uint32_t ParticleWorld::SpawnAt(const std::string& effect, const glm::vec3& position, const glm::quat& rotation,
                                     float scale)
{
    auto inst     = std::make_unique<Instance>();
    inst->path    = Key(effect);
    inst->oneShot = m_Impl->nextOneShot++;
    if (m_Impl->nextOneShot == 0)
        m_Impl->nextOneShot = 1;
    inst->world = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) *
                  glm::scale(glm::mat4(1.0f), glm::vec3(std::max(scale, 1e-4f)));
    if (!m_Impl->Start(*inst))
        return 0;
    const std::uint32_t id = inst->oneShot;
    m_Impl->instances.push_back(std::move(inst));
    return id;
}

void ParticleWorld::StopOneShot(std::uint32_t id, bool immediate)
{
    for (auto& inst : m_Impl->instances) {
        if (inst->oneShot != id)
            continue;
        inst->stopping = true;
        if (immediate) {
            for (EmitterState& s : inst->emitters)
                s.particles.clear();
            inst->finished = true;
        }
    }
}

bool ParticleWorld::IsOneShotActive(std::uint32_t id) const
{
    return id != 0 && std::ranges::any_of(m_Impl->instances, [&](const auto& inst) { return inst->oneShot == id && !inst->finished; });
}

std::shared_ptr<const ParticleEffect> ParticleWorld::Effect(const std::string& path)
{
    Impl::EffectRef* ref = m_Impl->Find(Key(path));
    return ref ? Impl::Current(*ref) : nullptr;
}

void ParticleWorld::Provide(const std::string& path, std::shared_ptr<const ParticleEffect> effect)
{
    const std::string key = Key(path);
    if (key.empty())
        return;
    auto [it, inserted] = m_Impl->effects.try_emplace(key);
    if (inserted) // the file may not exist yet (new, unsaved effect)
        it->second.time = Vfs::ModifiedTime(PathFromUtf8(key));
    const bool same = effect && it->second.provided ? *effect == *it->second.provided
                    : !effect && it->second.provided && it->second.effect ? *it->second.provided == *it->second.effect
                                                                            : false;
    it->second.provided = std::move(effect);
    if (!same)
        ++it->second.version;
}

void ParticleWorld::SetRandomSeed(std::uint32_t seed) { m_Impl->seedCounter = seed; }

const ParticleRenderData& ParticleWorld::RenderData() const { return m_Impl->render; }
const ParticleWorldStats& ParticleWorld::Stats() const { return m_Impl->stats; }

} // namespace Engine
