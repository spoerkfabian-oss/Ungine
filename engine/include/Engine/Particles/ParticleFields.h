#pragma once
#include "Engine/Particles/ParticleEffect.h"

#include <limits>
#include <type_traits>

namespace Engine {

// Field reflection of the particle modules: serialization and the editor's generic module UI walk
// the same list. Visitor: v(name, field, hint) with field types float, std::uint32_t,
// std::uint16_t, bool, glm::vec3, glm::vec4 (hint.color: a color), FloatRange, ParticleCurve,
// ParticleGradient, ParticleShape, ParticleEvent, std::string (hint.emitter: an emitter name).
struct ParticleFieldHint {
    float min   = -std::numeric_limits<float>::infinity();
    float max   = std::numeric_limits<float>::infinity();
    float speed = 0.01f;
    bool  color   = false;
    bool  emitter = false;
    bool  degrees = false;
};

namespace ParticleHints {
inline constexpr ParticleFieldHint Any{};
inline constexpr ParticleFieldHint Positive{.min = 0.0f};
inline constexpr ParticleFieldHint Unit{.min = 0.0f, .max = 1.0f, .speed = 0.005f};
inline constexpr ParticleFieldHint Angle{.min = 0.0f, .max = 180.0f, .speed = 0.5f, .degrees = true};
inline constexpr ParticleFieldHint AnyAngle{.speed = 0.5f, .degrees = true};
inline constexpr ParticleFieldHint Color{.min = 0.0f, .color = true};
inline constexpr ParticleFieldHint Count{.min = 0.0f, .max = 100000.0f, .speed = 0.2f};
inline constexpr ParticleFieldHint Emitter{.emitter = true};
} // namespace ParticleHints

template <class M, class V>
void VisitParticleFields(M& m, V&& v)
{
    using namespace ParticleHints;
    using T = std::remove_const_t<M>;
    if constexpr (std::is_same_v<T, SpawnRateModule>) {
        v("rate", m.rate, Positive);
        v("scale", m.scale, Any);
    } else if constexpr (std::is_same_v<T, SpawnBurstModule>) {
        v("time", m.time, Positive);
        v("count", m.count, Count);
        v("countMax", m.countMax, Count);
        v("cycles", m.cycles, Count);
        v("interval", m.interval, ParticleFieldHint{.min = 0.01f});
    } else if constexpr (std::is_same_v<T, SpawnPerDistanceModule>) {
        v("perMeter", m.perMeter, Positive);
    } else if constexpr (std::is_same_v<T, ShapeModule>) {
        v("shape", m.shape, Any);
        v("radius", m.radius, Positive);
        v("halfExtents", m.halfExtents, Positive);
        v("angle", m.angle, ParticleFieldHint{.min = 0.0f, .max = 90.0f, .speed = 0.5f, .degrees = true});
        v("surface", m.surface, Any);
        v("offset", m.offset, Any);
    } else if constexpr (std::is_same_v<T, VelocityModule>) {
        v("direction", m.direction, Any);
        v("spread", m.spread, Angle);
        v("speed", m.speed, Any);
        v("radial", m.radial, Any);
        v("inheritVelocity", m.inheritVelocity, Any);
    } else if constexpr (std::is_same_v<T, LifetimeModule>) {
        v("seconds", m.seconds, ParticleFieldHint{.min = 0.01f});
    } else if constexpr (std::is_same_v<T, SizeModule>) {
        v("size", m.size, Positive);
        v("perAxis", m.perAxis, Any);
        v("axisMin", m.axisMin, Positive);
        v("axisMax", m.axisMax, Positive);
    } else if constexpr (std::is_same_v<T, ColorModule>) {
        v("colorA", m.colorA, Color);
        v("colorB", m.colorB, Color);
        v("intensity", m.intensity, Positive);
    } else if constexpr (std::is_same_v<T, RotationModule>) {
        v("angle", m.angle, AnyAngle);
        v("rate", m.rate, AnyAngle);
    } else if constexpr (std::is_same_v<T, SpriteFrameModule>) {
        v("random", m.random, Any);
    } else if constexpr (std::is_same_v<T, GravityModule>) {
        v("acceleration", m.acceleration, Any);
    } else if constexpr (std::is_same_v<T, DragModule>) {
        v("coefficient", m.coefficient, Positive);
    } else if constexpr (std::is_same_v<T, ForceModule>) {
        v("acceleration", m.acceleration, Any);
        v("local", m.local, Any);
    } else if constexpr (std::is_same_v<T, CurlNoiseModule>) {
        v("strength", m.strength, Positive);
        v("frequency", m.frequency, ParticleFieldHint{.min = 0.001f});
        v("scroll", m.scroll, Any);
    } else if constexpr (std::is_same_v<T, AttractorModule>) {
        v("position", m.position, Any);
        v("strength", m.strength, Any);
        v("radius", m.radius, Positive);
        v("killRadius", m.killRadius, Positive);
    } else if constexpr (std::is_same_v<T, SpeedLimitModule>) {
        v("maxSpeed", m.maxSpeed, Positive);
    } else if constexpr (std::is_same_v<T, SizeOverLifeModule>) {
        v("curve", m.curve, Any);
    } else if constexpr (std::is_same_v<T, ColorOverLifeModule>) {
        v("gradient", m.gradient, Any);
    } else if constexpr (std::is_same_v<T, SubUvModule>) {
        v("overLife", m.overLife, Any);
        v("cycles", m.cycles, Positive);
        v("fps", m.fps, Positive);
    } else if constexpr (std::is_same_v<T, CollisionModule>) {
        v("radiusScale", m.radiusScale, Positive);
        v("restitution", m.restitution, Unit);
        v("friction", m.friction, Unit);
        v("kill", m.kill, Any);
        v("maxHits", m.maxHits, Count);
        v("layerMask", m.layerMask, Any);
    } else if constexpr (std::is_same_v<T, KillVolumeModule>) {
        v("sphere", m.sphere, Any);
        v("center", m.center, Any);
        v("halfExtents", m.halfExtents, Positive);
        v("radius", m.radius, Positive);
        v("invert", m.invert, Any);
    } else if constexpr (std::is_same_v<T, SubEmitterModule>) {
        v("event", m.event, Any);
        v("emitter", m.emitter, Emitter);
        v("count", m.count, Count);
        v("inheritVelocity", m.inheritVelocity, Any);
    } else {
        static_assert(sizeof(T) == 0, "particle module without a field list");
    }
}

} // namespace Engine
