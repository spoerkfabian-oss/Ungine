#include "Test.h"

#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Particles/ParticleEffect.h"
#include "Engine/Particles/ParticleFields.h"
#include "Engine/Particles/ParticleWorld.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path ParticleTempDir(const char* name)
{
    const fs::path  dir = fs::temp_directory_path() / "ungine_tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool Near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

template <class M>
M& Add(ParticleEmitter& e, M module)
{
    ParticleModule m;
    m.params = std::move(module);
    e.modules.push_back(std::move(m));
    return std::get<M>(e.modules.back().params);
}

// An emitter without the default modules.
ParticleEmitter Bare(std::string name, std::uint32_t maxParticles = 1000)
{
    ParticleEmitter e;
    e.name         = std::move(name);
    e.maxParticles = maxParticles;
    return e;
}

std::shared_ptr<const ParticleEffect> Shared(ParticleEffect effect) { return std::make_shared<const ParticleEffect>(std::move(effect)); }

// Steps the world `seconds` in frames of 1/60 s.
void Run(ParticleWorld& world, Scene& scene, float seconds, float dt = 1.0f / 60.0f)
{
    const int frames = static_cast<int>(std::lround(seconds / dt));
    for (int i = 0; i < frames; ++i) {
        scene.UpdateTransforms();
        world.Update(scene, dt);
    }
}

Entity Spawner(Scene& scene, const std::string& effect, std::uint32_t seed = 7, glm::vec3 position = glm::vec3(0.0f))
{
    const Entity e = scene.CreateEntity("Particles");
    scene.EditTransform(e).position = position;
    scene.GetRegistry().Emplace<ParticleSystem>(e, ParticleSystem{.effect = effect, .seed = seed});
    scene.UpdateTransforms();
    return e;
}

} // namespace

TEST_CASE(Particle_CurvesFilesAndValidation)
{
    // Curves / gradients.
    CHECK(ParticleCurve{}.Evaluate(0.3f) == 1.0f);
    const ParticleCurve ramp = ParticleCurve::Ramp(0.0f, 2.0f);
    CHECK(Near(ramp.Evaluate(0.25f), 0.5f, 1e-5f) && ramp.Evaluate(-1.0f) == 0.0f && ramp.Evaluate(5.0f) == 2.0f);
    ParticleCurve smooth = ramp;
    smooth.interp = ParticleCurve::Interp::Smooth;
    CHECK(Near(smooth.Evaluate(0.25f), 2.0f * 0.15625f, 1e-5f));
    ParticleCurve step = ramp;
    step.interp = ParticleCurve::Interp::Constant;
    CHECK(step.Evaluate(0.99f) == 0.0f && step.Evaluate(1.0f) == 2.0f);
    const ParticleGradient fade = ParticleGradient::Fade(glm::vec4(1.0f), glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    CHECK(Near(fade.Evaluate(0.5f).a, 0.5f, 1e-5f) && Near(fade.Evaluate(0.5f).b, 1.0f, 1e-5f));

    // Every module type: names, defaults, file round trip.
    ParticleEffect effect;
    effect.warmup = 0.5f;
    ParticleEmitter all = Bare("All");
    for (std::size_t t = 0; t < static_cast<std::size_t>(ParticleModuleType::Count); ++t) {
        const auto type = static_cast<ParticleModuleType>(t);
        CHECK(ParticleModuleTypeFromString(ModuleInfo(type).name) == type);
        ParticleModule m = MakeParticleModule(type);
        CHECK(m.Type() == type);
        all.modules.push_back(std::move(m));
    }
    all.modules[1].enabled = false;
    std::get<SubEmitterModule>(all.modules.back().params).emitter = "Child";
    auto& keys = std::get<ColorOverLifeModule>(all.modules[static_cast<std::size_t>(ParticleModuleType::ColorOverLife)].params).gradient.keys;
    keys.insert(keys.begin() + 1, {0.5f, glm::vec4(2.0f, 0.5f, 0.25f, 0.75f)});
    std::get<ShapeModule>(all.modules[static_cast<std::size_t>(ParticleModuleType::Shape)].params).shape = ParticleShape::Cone;
    const fs::path dir = ParticleTempDir("particle_files");
    all.render.texture = PathToUtf8((dir / "Textures" / "spark.png").lexically_normal());
    all.render.type    = ParticleRenderType::Ribbon;
    all.render.blend   = ParticleBlend::Additive;
    all.render.columns = 4;
    effect.emitters.push_back(all);
    ParticleEmitter child = MakeDefaultParticleEmitter("Child");
    child.render.type     = ParticleRenderType::Mesh;
    child.render.mesh     = PathToUtf8((dir.root_path() / "elsewhere" / "rock.glb").lexically_normal()); // "../../.." on disk
    effect.emitters.push_back(child);
    const fs::path file = dir / "Effects" / "Test.uparticle";
    std::string    error;
    CHECK(SaveParticleEffect(file, effect, &error));
    const std::string text = ParticleEffectToJson(effect, file.parent_path());
    CHECK(text.find("\"../Textures/spark.png\"") != std::string::npos); // relative, also outside the effect's folder
    const auto loaded = LoadParticleEffect(file, &error);
    CHECK(loaded.has_value());
    if (loaded)
        CHECK(*loaded == effect);
    // Below the effect's folder: stored relative, read back absolute.
    effect.emitters[0].render.texture = PathToUtf8((dir / "Effects" / "spark.png").lexically_normal());
    CHECK(ParticleEffectToJson(effect, file.parent_path()).find("\"spark.png\"") != std::string::npos);
    CHECK(SaveParticleEffect(file, effect));
    CHECK(LoadParticleEffect(file).value_or(ParticleEffect{}) == effect);

    // Clamping, unknown modules, duplicate names, broken JSON.
    const auto parsed = ParseParticleEffect(R"({"emitters": [
        {"name": "A", "maxParticles": 999999999, "modules": [
            {"type": "SpawnRate", "rate": -5}, {"type": "FromTheFuture", "x": 1},
            {"type": "Lifetime", "seconds": [3, 1]}, {"type": "Collision", "restitution": 7}]},
        {"name": "A"}]})", dir);
    CHECK(parsed.has_value());
    if (parsed) {
        const ParticleEmitter& a = parsed->emitters[0];
        CHECK(a.maxParticles == kMaxParticlesPerEmitter && a.modules.size() == 3);
        CHECK(std::get<SpawnRateModule>(a.modules[0].params).rate == 0.0f);
        const FloatRange life = std::get<LifetimeModule>(a.modules[1].params).seconds;
        CHECK(life.min == 1.0f && life.max == 3.0f);
        CHECK(std::get<CollisionModule>(a.modules[2].params).restitution == 1.0f);
        CHECK(parsed->emitters[1].name != "A");
    }
    CHECK(!ParseParticleEffect("{ nope", dir).has_value());

    // Validation: missing target, never spawns, mesh without mesh, cycle.
    ParticleEffect bad;
    ParticleEmitter a = Bare("A");
    Add(a, SpawnRateModule{});
    Add(a, SubEmitterModule{.emitter = "Ghost"});
    Add(a, SubEmitterModule{.emitter = "B"});
    ParticleEmitter b = Bare("B");
    Add(b, SubEmitterModule{.emitter = "A"});
    ParticleEmitter lonely = Bare("Lonely");
    lonely.render.type = ParticleRenderType::Mesh;
    bad.emitters = {a, b, lonely};
    const auto problems = ValidateParticleEffect(bad);
    const auto has = [&](const char* what) {
        return std::ranges::any_of(problems, [&](const std::string& p) { return p.find(what) != std::string::npos; });
    };
    CHECK(has("'Ghost' does not exist") && has("cycle") && has("'Lonely': never spawns") && has("without a mesh"));
    CHECK(ValidateParticleEffect(MakeDefaultParticleEffect()).empty());

    // Field reflection covers every module (serialization and the editor use it).
    for (std::size_t t = 0; t < static_cast<std::size_t>(ParticleModuleType::Count); ++t) {
        ParticleModule m     = MakeParticleModule(static_cast<ParticleModuleType>(t));
        int            count = 0;
        std::visit([&](auto& p) { VisitParticleFields(p, [&](const char*, auto&, const ParticleFieldHint&) { ++count; }); },
                   m.params);
        CHECK(count > 0);
    }
}

TEST_CASE(Particle_SpawnLifetimeForcesAndRenderData)
{
    Scene         scene;
    ParticleWorld world;
    world.SetRandomSeed(11);

    // Rate 120/s, exactly 1 s of life: 60 after half a second, a steady 120 later.
    ParticleEffect rate;
    ParticleEmitter e = Bare("Rate");
    Add(e, SpawnRateModule{.rate = 120.0f});
    Add(e, LifetimeModule{.seconds = {1.0f, 1.0f}});
    rate.emitters.push_back(e);
    world.Provide("rate.uparticle", Shared(rate));
    const Entity rateEntity = Spawner(scene, "rate.uparticle");
    Run(world, scene, 0.5f);
    CHECK(Near(static_cast<float>(world.ParticleCount(rateEntity)), 60.0f, 2.0f));
    Run(world, scene, 2.0f);
    CHECK(Near(static_cast<float>(world.ParticleCount(rateEntity)), 120.0f, 2.0f));
    CHECK(world.IsActive(rateEntity) && world.Stats().instances == 1);
    scene.DestroyEntity(rateEntity);

    // Gravity: one particle at rest falls y = -g t^2 / 2; drag decays velocity exponentially.
    ParticleEffect fall;
    ParticleEmitter g = Bare("Fall");
    Add(g, SpawnBurstModule{.time = 0.0f, .count = 1});
    Add(g, LifetimeModule{.seconds = {10.0f, 10.0f}});
    Add(g, GravityModule{.acceleration = {0.0f, -10.0f, 0.0f}});
    fall.emitters.push_back(g);
    ParticleEmitter d = Bare("Drag");
    Add(d, SpawnBurstModule{.time = 0.0f, .count = 1});
    Add(d, LifetimeModule{.seconds = {10.0f, 10.0f}});
    Add(d, VelocityModule{.direction = {1.0f, 0.0f, 0.0f}, .spread = 0.0f, .speed = {4.0f, 4.0f}});
    Add(d, DragModule{.coefficient = 2.0f});
    fall.emitters.push_back(d);
    world.Provide("fall.uparticle", Shared(fall));
    const Entity faller = Spawner(scene, "fall.uparticle", 3, {0.0f, 100.0f, 0.0f});
    Run(world, scene, 1.0f);
    const ParticleRenderData& data = world.RenderData();
    CHECK(data.batches.size() == 2);
    if (data.batches.size() == 2) {
        const GpuParticle& p = data.particles[data.batches[0].first];
        CHECK(Near(p.position.y, 100.0f - 5.0f, 0.15f));
        const GpuParticle& q = data.particles[data.batches[1].first];
        CHECK(Near(glm::length(q.velocity), 4.0f * std::exp(-2.0f), 0.03f));
        CHECK(Near(q.position.x, 2.0f * (1.0f - std::exp(-2.0f)), 0.05f)); // integral of 4 e^-2t
    }
    scene.DestroyEntity(faller);

    // Bursts with cycles; a non-looping duration stops spawning, the effect finishes.
    ParticleEffect bursts;
    ParticleEmitter b = Bare("Bursts");
    Add(b, SpawnBurstModule{.time = 0.0f, .count = 10, .cycles = 3, .interval = 0.5f});
    Add(b, LifetimeModule{.seconds = {5.0f, 5.0f}});
    bursts.emitters.push_back(b);
    world.Provide("bursts.uparticle", Shared(bursts));
    const Entity burster = Spawner(scene, "bursts.uparticle");
    Run(world, scene, 0.25f);
    CHECK(world.ParticleCount(burster) == 10);
    Run(world, scene, 1.25f);
    CHECK(world.ParticleCount(burster) == 30);
    scene.DestroyEntity(burster);

    ParticleEffect once;
    ParticleEmitter o = Bare("Once");
    o.duration = 0.5f;
    o.loop     = false;
    Add(o, SpawnRateModule{.rate = 100.0f});
    Add(o, LifetimeModule{.seconds = {0.25f, 0.25f}});
    once.emitters.push_back(o);
    world.Provide("once.uparticle", Shared(once));
    const Entity onceEntity = Spawner(scene, "once.uparticle");
    Run(world, scene, 0.4f);
    CHECK(world.IsActive(onceEntity) && world.ParticleCount(onceEntity) > 0);
    Run(world, scene, 0.6f);
    CHECK(!world.IsActive(onceEntity) && world.ParticleCount(onceEntity) == 0);
    CHECK(world.Play(onceEntity)); // restart
    Run(world, scene, 0.1f);
    CHECK(world.IsActive(onceEntity) && world.ParticleCount(onceEntity) > 0);
    world.Stop(onceEntity, true);
    CHECK(!world.IsActive(onceEntity) && world.ParticleCount(onceEntity) == 0);
    scene.DestroyEntity(onceEntity);

    // Render data: size / color over life, sprite sheet frames, tint and size scale, kill volume.
    ParticleEffect look;
    ParticleEmitter l = Bare("Look");
    l.render.columns = 2;
    l.render.rows    = 2;
    Add(l, SpawnBurstModule{.time = 0.0f, .count = 1});
    Add(l, LifetimeModule{.seconds = {2.0f, 2.0f}});
    Add(l, SizeModule{.size = {0.5f, 0.5f}});
    Add(l, ColorModule{.colorA = glm::vec4(1.0f, 0.5f, 0.25f, 1.0f), .colorB = glm::vec4(1.0f, 0.5f, 0.25f, 1.0f), .intensity = 4.0f});
    Add(l, SizeOverLifeModule{.curve = ParticleCurve::Ramp(1.0f, 0.0f)});
    Add(l, ColorOverLifeModule{.gradient = ParticleGradient::Fade(glm::vec4(1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.0f))});
    Add(l, SubUvModule{.overLife = true, .cycles = 1.0f});
    look.emitters.push_back(l);
    ParticleEmitter k = Bare("Killed");
    Add(k, SpawnRateModule{.rate = 60.0f});
    Add(k, VelocityModule{.direction = {0.0f, -1.0f, 0.0f}, .spread = 0.0f, .speed = {10.0f, 10.0f}});
    Add(k, KillVolumeModule{.sphere = false, .center = {0.0f, -10.0f, 0.0f}, .halfExtents = {50.0f, 5.0f, 50.0f}});
    look.emitters.push_back(k);
    world.Provide("look.uparticle", Shared(look));
    const Entity looker = Spawner(scene, "look.uparticle");
    scene.GetRegistry().Get<ParticleSystem>(looker).tint      = glm::vec4(0.5f, 1.0f, 1.0f, 1.0f);
    scene.GetRegistry().Get<ParticleSystem>(looker).sizeScale = 2.0f;
    Run(world, scene, 1.1f);
    const ParticleBatch* lookBatch = nullptr;
    for (const ParticleBatch& batch : world.RenderData().batches)
        if (batch.render.columns == 2)
            lookBatch = &batch;
    CHECK(lookBatch && lookBatch->count == 1);
    if (lookBatch) {
        const GpuParticle& p = world.RenderData().particles[lookBatch->first];
        CHECK(Near(p.color.a, 0.45f, 0.02f) && Near(p.color.r, 2.0f, 0.05f) && Near(p.color.g, 2.0f, 0.05f));
        CHECK(Near(p.size.x, 0.45f, 0.02f)); // 0.5 x 2 (scale) x 0.45 (55 % of the life)
        CHECK(p.frame == 2.0f);              // 55 % of 4 frames
        CHECK(lookBatch->boundsMin.x <= p.position.x && lookBatch->boundsMax.y >= p.position.y);
    }
    // 10 m/s down: they reach the kill box (y < -5) after 0.5 s, so about 30 live at once.
    CHECK(world.ParticleCount(looker) <= 1 + 32);
}

TEST_CASE(Particle_SubEmittersRibbonsAndLocalSpace)
{
    Scene         scene;
    ParticleWorld world;

    // Death events: 3 parents, each spawns 4 children where it dies.
    ParticleEffect effect;
    ParticleEmitter parent = Bare("Parent");
    Add(parent, SpawnBurstModule{.time = 0.0f, .count = 3});
    Add(parent, LifetimeModule{.seconds = {0.5f, 0.5f}});
    Add(parent, VelocityModule{.direction = {0.0f, 1.0f, 0.0f}, .spread = 0.0f, .speed = {2.0f, 2.0f}});
    Add(parent, SubEmitterModule{.event = ParticleEvent::Death, .emitter = "Child", .count = 4});
    Add(parent, SubEmitterModule{.event = ParticleEvent::Trail, .emitter = "Trail", .count = 60});
    effect.emitters.push_back(parent);
    ParticleEmitter child = Bare("Child");
    Add(child, LifetimeModule{.seconds = {5.0f, 5.0f}});
    effect.emitters.push_back(child);
    ParticleEmitter trail = Bare("Trail");
    trail.render.type     = ParticleRenderType::Ribbon;
    trail.render.ribbonUv = RibbonUv::Tile;
    Add(trail, LifetimeModule{.seconds = {5.0f, 5.0f}});
    effect.emitters.push_back(trail);
    CHECK(ValidateParticleEffect(effect).empty());
    world.Provide("burst.uparticle", Shared(effect));
    const Entity e = Spawner(scene, "burst.uparticle", 5, {1.0f, 0.0f, 0.0f});
    Run(world, scene, 0.4f);
    const ParticleRenderData& data = world.RenderData();
    // Three ribbons (one per parent), points in spawn order along +Y, u growing with distance.
    std::map<std::uint32_t, std::vector<GpuRibbonPoint>> ribbons;
    for (const ParticleBatch& b : data.batches)
        if (b.render.type == ParticleRenderType::Ribbon)
            for (std::uint32_t i = 0; i < b.count; ++i)
                ribbons[data.ribbonPoints[b.first + i].ribbon].push_back(data.ribbonPoints[b.first + i]);
    CHECK(ribbons.size() == 3);
    for (const auto& [id, points] : ribbons) {
        CHECK(points.size() >= 20);
        for (std::size_t i = 1; i < points.size(); ++i)
            CHECK(points[i].position.y > points[i - 1].position.y && points[i].u > points[i - 1].u);
    }
    Run(world, scene, 0.2f);
    std::uint32_t children = 0;
    glm::vec3     childPosition(0.0f);
    for (const ParticleBatch& b : world.RenderData().batches)
        if (b.render.type == ParticleRenderType::Sprite && b.count == 12) {
            children      = b.count;
            childPosition = world.RenderData().particles[b.first].position;
        }
    CHECK(children == 12);
    CHECK(Near(childPosition.x, 1.0f, 1e-3f) && Near(childPosition.y, 1.0f, 0.05f)); // where the parents died (2 m/s x 0.5 s)
    scene.DestroyEntity(e);

    // Local space follows the entity; world space stays behind. Spawn per distance.
    ParticleEffect follow;
    ParticleEmitter local = Bare("Local");
    local.localSpace = true;
    Add(local, SpawnBurstModule{.time = 0.0f, .count = 1});
    Add(local, LifetimeModule{.seconds = {10.0f, 10.0f}});
    follow.emitters.push_back(local);
    ParticleEmitter worldSpace = Bare("World");
    Add(worldSpace, SpawnBurstModule{.time = 0.0f, .count = 1});
    Add(worldSpace, LifetimeModule{.seconds = {10.0f, 10.0f}});
    follow.emitters.push_back(worldSpace);
    ParticleEmitter trailing = Bare("Distance");
    Add(trailing, SpawnPerDistanceModule{.perMeter = 5.0f});
    Add(trailing, LifetimeModule{.seconds = {10.0f, 10.0f}});
    follow.emitters.push_back(trailing);
    world.Provide("follow.uparticle", Shared(follow));
    const Entity mover = Spawner(scene, "follow.uparticle");
    Run(world, scene, 0.1f);
    scene.EditTransform(mover).position = {2.0f, 0.0f, 0.0f};
    Run(world, scene, 1.0f / 60.0f);
    const auto& moved = world.RenderData();
    CHECK(moved.batches.size() == 3);
    if (moved.batches.size() == 3) {
        CHECK(Near(moved.particles[moved.batches[0].first].position.x, 2.0f, 1e-4f)); // local: moved along
        CHECK(Near(moved.particles[moved.batches[1].first].position.x, 0.0f, 1e-4f)); // world: stayed
        CHECK(moved.batches[2].count == 10);                                          // 2 m x 5 per meter
        // Spread along the path the entity moved this frame.
        float lo = 1e9f, hi = -1e9f;
        for (std::uint32_t i = 0; i < moved.batches[2].count; ++i) {
            lo = std::min(lo, moved.particles[moved.batches[2].first + i].position.x);
            hi = std::max(hi, moved.particles[moved.batches[2].first + i].position.x);
        }
        CHECK(lo < 0.5f && hi > 1.5f);
    }
}

TEST_CASE(Particle_CollisionWithPhysics)
{
    ThreadPool   pool(2);
    EventBus     bus;
    PhysicsWorld physics(pool, bus);
    Scene        scene;
    Registry&    r      = scene.GetRegistry();
    const Entity ground = scene.CreateEntity("Ground");
    scene.EditTransform(ground).position = {0.0f, -0.5f, 0.0f};
    r.Emplace<RigidBody>(ground, RigidBody{.type = BodyType::Static});
    r.Emplace<Collider>(ground, Collider{.halfExtents = {20.0f, 0.5f, 20.0f}});
    physics.Sync(scene);

    ParticleWorld  world(&pool, nullptr, &physics);
    ParticleEffect effect;
    ParticleEmitter bounce = Bare("Bounce");
    Add(bounce, SpawnBurstModule{.time = 0.0f, .count = 50});
    Add(bounce, ShapeModule{.shape = ParticleShape::Box, .halfExtents = {2.0f, 0.1f, 2.0f}});
    Add(bounce, LifetimeModule{.seconds = {10.0f, 10.0f}});
    Add(bounce, SizeModule{.size = {0.1f, 0.1f}});
    Add(bounce, GravityModule{});
    Add(bounce, CollisionModule{.radiusScale = 0.5f, .restitution = 0.5f, .friction = 0.1f});
    effect.emitters.push_back(bounce);
    ParticleEmitter shatter = Bare("Shatter");
    Add(shatter, SpawnBurstModule{.time = 0.0f, .count = 20});
    Add(shatter, ShapeModule{.shape = ParticleShape::Box, .halfExtents = {2.0f, 0.1f, 2.0f}});
    Add(shatter, LifetimeModule{.seconds = {10.0f, 10.0f}});
    Add(shatter, GravityModule{});
    Add(shatter, CollisionModule{.kill = true});
    Add(shatter, SubEmitterModule{.event = ParticleEvent::Collision, .emitter = "Dust", .count = 2});
    effect.emitters.push_back(shatter);
    ParticleEmitter dust = Bare("Dust");
    Add(dust, LifetimeModule{.seconds = {10.0f, 10.0f}});
    effect.emitters.push_back(dust);
    world.Provide("collide.uparticle", Shared(effect));
    const Entity e = Spawner(scene, "collide.uparticle", 9, {0.0f, 2.0f, 0.0f});
    bool sawRays = false;
    for (int frame = 0; frame < 150; ++frame) {
        world.Update(scene, 1.0f / 60.0f);
        sawRays = sawRays || world.Stats().collisionRays > 0;
    }
    CHECK(sawRays);
    const ParticleRenderData& data = world.RenderData();
    std::map<std::uint32_t, const ParticleBatch*> bySize;
    for (const ParticleBatch& b : data.batches)
        bySize[b.count] = &b;
    CHECK(bySize.contains(50) && !bySize.contains(20) && bySize.contains(40)); // shattered on impact, 2 dust each
    if (bySize.contains(50)) {
        const ParticleBatch& b = *bySize[50];
        float lowest = 1e9f, fastest = 0.0f;
        for (std::uint32_t i = 0; i < b.count; ++i) {
            lowest  = std::min(lowest, data.particles[b.first + i].position.y);
            fastest = std::max(fastest, std::abs(data.particles[b.first + i].velocity.y));
        }
        CHECK(lowest > 0.0f && lowest < 0.2f); // resting on the ground (radius 0.05), not through it
        CHECK(fastest < 1.0f);
    }
    if (bySize.contains(40)) {
        const ParticleBatch& b = *bySize[40];
        CHECK(Near(data.particles[b.first].position.y, 0.0f, 0.1f)); // dust where they hit
    }
    CHECK(world.ParticleCount(e) == 90);
}

TEST_CASE(Particle_ComponentsJobsFilesAndDeterminism)
{
    const fs::path dir = ParticleTempDir("particle_components");
    ParticleEffect effect = MakeDefaultParticleEffect();
    Add(effect.emitters[0], CurlNoiseModule{});
    const fs::path file = dir / "Smoke.uparticle";
    CHECK(SaveParticleEffect(file, effect));
    const std::string path = PathToUtf8(file);

    // Same seeds, with and without jobs: identical particles.
    const auto simulate = [&](ThreadPool* pool) {
        Scene         scene;
        ParticleWorld world(pool);
        for (int i = 0; i < 16; ++i)
            Spawner(scene, path, static_cast<std::uint32_t>(100 + i), {static_cast<float>(i), 0.0f, 0.0f});
        Run(world, scene, 1.0f);
        return world.RenderData().particles;
    };
    ThreadPool pool(4);
    const auto serial   = simulate(nullptr);
    const auto parallel = simulate(&pool);
    CHECK(!serial.empty() && serial.size() == parallel.size());
    bool same = serial.size() == parallel.size();
    for (std::size_t i = 0; same && i < serial.size(); ++i)
        same = serial[i].position == parallel[i].position && serial[i].color == parallel[i].color;
    CHECK(same);

    Scene         scene;
    ParticleWorld world(&pool);
    // autoPlay off: nothing until Play.
    const Entity quiet = Spawner(scene, path);
    scene.GetRegistry().Get<ParticleSystem>(quiet).autoPlay = false;
    const Entity loud = Spawner(scene, path);
    Run(world, scene, 0.5f);
    CHECK(world.ParticleCount(quiet) == 0 && world.ParticleCount(loud) > 0 && world.Stats().instances == 1);
    CHECK(world.Play(quiet) && world.IsActive(quiet));
    Run(world, scene, 0.5f);
    CHECK(world.ParticleCount(quiet) > 0 && world.Stats().instances == 2);
    // Removing the component removes the instance; a missing effect plays nothing.
    scene.GetRegistry().Remove<ParticleSystem>(quiet);
    Run(world, scene, 0.1f);
    CHECK(world.Stats().instances == 1 && !world.IsActive(quiet));
    scene.GetRegistry().Get<ParticleSystem>(loud).effect = PathToUtf8(dir / "Missing.uparticle");
    Run(world, scene, 0.1f);
    CHECK(!world.IsActive(loud) && world.RenderData().batches.empty());

    // Hot reload: a changed file restarts its instances with the new content.
    scene.GetRegistry().Get<ParticleSystem>(loud).effect = path;
    Run(world, scene, 0.5f);
    const std::uint32_t before = world.ParticleCount(loud);
    std::get<SpawnRateModule>(effect.emitters[0].modules[0].params).rate = 200.0f;
    CHECK(SaveParticleEffect(file, effect));
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(2));
    Run(world, scene, 1.5f);
    CHECK(world.ParticleCount(loud) > before * 3);

    // One-shots: removed when done; looping ones by StopOneShot; missing effects fail.
    ParticleEffect puff;
    ParticleEmitter p = Bare("Puff");
    Add(p, SpawnBurstModule{.time = 0.0f, .count = 5});
    Add(p, LifetimeModule{.seconds = {0.3f, 0.3f}});
    puff.emitters.push_back(p);
    world.Provide("puff.uparticle", Shared(puff));
    const std::uint32_t shot = world.SpawnAt("puff.uparticle", glm::vec3(3.0f));
    CHECK(shot != 0 && world.IsOneShotActive(shot));
    Run(world, scene, 0.1f);
    CHECK(world.Stats().oneShots == 1);
    Run(world, scene, 0.5f);
    CHECK(!world.IsOneShotActive(shot) && world.Stats().oneShots == 0);
    const std::uint32_t looping = world.SpawnAt(path, glm::vec3(0.0f));
    Run(world, scene, 0.2f);
    CHECK(world.IsOneShotActive(looping));
    world.StopOneShot(looping, true);
    Run(world, scene, 0.1f);
    CHECK(!world.IsOneShotActive(looping));
    CHECK(world.SpawnAt(PathToUtf8(dir / "Nope.uparticle"), glm::vec3(0.0f)) == 0);
    // Paused: nothing moves.
    world.SetPaused(true);
    const auto frozen = world.RenderData().particles;
    Run(world, scene, 0.2f);
    CHECK(world.RenderData().particles.size() == frozen.size() &&
          (frozen.empty() || world.RenderData().particles[0].position == frozen[0].position));
    world.SetPaused(false);
    world.Reset();
    CHECK(world.Stats().instances == 0 && world.RenderData().particles.empty());

    // Component serialization: entity state, scene file (relative path).
    const Entity saved = Spawner(scene, path, 42);
    ParticleSystem& config = scene.GetRegistry().Get<ParticleSystem>(saved);
    config.autoPlay  = false;
    config.speed     = 2.0f;
    config.sizeScale = 3.0f;
    config.tint      = glm::vec4(2.0f, 1.0f, 0.5f, 0.25f);
    const ParticleSystem expected = config;
    const std::string state = SnapshotEntityState(scene, saved);
    scene.GetRegistry().Remove<ParticleSystem>(saved);
    ApplyEntityState(scene, saved, state);
    CHECK(scene.GetRegistry().TryGet<ParticleSystem>(saved) && scene.GetRegistry().Get<ParticleSystem>(saved) == expected);
    const fs::path sceneFile = dir / "Level.scene.json";
    SaveSceneFile(sceneFile, scene, nullptr);
    std::ifstream in(sceneFile);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"effect\": \"Smoke.uparticle\"") != std::string::npos);
    Scene reloaded;
    CHECK(LoadSceneFile(sceneFile, reloaded, nullptr).empty());
    bool found = false;
    reloaded.GetRegistry().ViewOf<ParticleSystem>().Each([&](Entity, ParticleSystem& s) {
        std::error_code ec; // Windows: loader yields the long, canonical form of a short temp path
        found = fs::equivalent(PathFromUtf8(s.effect), file, ec) && s.speed == 2.0f && s.tint == expected.tint &&
                !s.autoPlay;
    });
    CHECK(found);
}

TEST_CASE(Particle_BlueprintNodes)
{
    Scene          scene;
    EventBus       events;
    ParticleWorld  world;
    ParticleEffect effect;
    ParticleEmitter e = Bare("Sparks");
    Add(e, SpawnRateModule{.rate = 60.0f});
    Add(e, LifetimeModule{.seconds = {2.0f, 2.0f}});
    effect.emitters.push_back(e);
    world.Provide("sparks.uparticle", Shared(effect));

    ScriptSystem scripts(events);
    scripts.SetParticleWorld(&world);
    ScriptGraph graph;
    const auto  node = [&](const char* type) { return graph.AddNode(type, {}, {}); };
    const auto  link = [&](std::uint32_t a, const char* pa, std::uint32_t b, const char* pb) {
        CHECK(graph.Connect(a, pa, b, pb).empty());
    };
    // BeginPlay: spawn a one-shot, set the spawn scale to 0 and the tint, play the own system.
    const std::uint32_t begin = node("Event.BeginPlay");
    const std::uint32_t spawn = node("Particles.SpawnAt");
    graph.FindNode(spawn)->defaults["Effect"] = std::string("sparks.uparticle");
    const std::uint32_t scale = node("Particles.SetScale");
    graph.FindNode(scale)->param             = "size";
    graph.FindNode(scale)->defaults["Scale"] = 4.0f;
    const std::uint32_t tint = node("Particles.SetTint");
    graph.FindNode(tint)->defaults["Color"] = glm::vec3(2.0f, 0.0f, 0.0f);
    const std::uint32_t play = node("Particles.Play");
    const std::uint32_t print = node("Debug.Print");
    link(begin, "Out", spawn, "In");
    link(spawn, "Then", scale, "In");
    link(scale, "Then", tint, "In");
    link(tint, "Then", play, "In");
    link(play, "Then", print, "In");
    link(spawn, "Handle", print, "Text");
    // Tick: print the particle count; key-less stop through a second event is not needed here.
    const std::uint32_t tick   = node("Event.Tick");
    const std::uint32_t active = node("Particles.IsActive");
    const std::uint32_t count  = node("Debug.Print");
    link(tick, "Out", count, "In");
    link(active, "Count", count, "Text");
    CHECK(ValidateScriptGraph(graph).empty());

    const Entity actor = scene.CreateEntity("Actor");
    scene.GetRegistry().Emplace<ParticleSystem>(actor, ParticleSystem{.effect = "sparks.uparticle", .autoPlay = false});
    scene.GetRegistry().Emplace<ScriptComponent>(actor, ScriptComponent{"fx.ugraph"});
    scripts.Provide("fx.ugraph", graph);
    scripts.Begin(scene);
    CHECK(scripts.Stats().errors == 0);
    const ParticleSystem& config = scene.GetRegistry().Get<ParticleSystem>(actor);
    CHECK(config.sizeScale == 4.0f && config.tint == glm::vec4(2.0f, 0.0f, 0.0f, 1.0f));
    CHECK(world.IsActive(actor) && !scripts.Messages().empty() && scripts.Messages().back().text == "1");
    for (int i = 0; i < 30; ++i) {
        world.Update(scene, 1.0f / 60.0f);
        scripts.Update(scene, 1.0f / 60.0f);
    }
    CHECK(world.Stats().oneShots == 1 && world.ParticleCount(actor) > 20);
    CHECK(std::stoi(scripts.Messages().back().text) == static_cast<int>(world.ParticleCount(actor)));
    scripts.End(scene);

    // Without a particle world the nodes report errors.
    ScriptSystem bare(events);
    bare.Provide("fx.ugraph", graph);
    bare.Begin(scene);
    CHECK(bare.Stats().errors >= 1);
    bare.End(scene);
}
