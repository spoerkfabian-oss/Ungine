// Phase 31 scene data (CPU): decals and fog volumes in scene files, decal lifetimes, Blueprint
// "Spawn Decal at Location". The rendering itself is covered by the GPU tests.
#include "BlueprintTestUtil.h"
#include "Test.h"

#include "Engine/Core/Platform.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Decals.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path RenderTempDir(const char* name)
{
    const fs::path  dir = fs::temp_directory_path() / "ungine_tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

} // namespace

TEST_CASE(Rendering_DecalsAndFogVolumesInSceneFiles)
{
    const fs::path dir     = RenderTempDir("decal_scene");
    const fs::path texture = dir / "Textures" / "Splat.png";
    fs::create_directories(texture.parent_path());
    std::ofstream(texture) << "not decoded here";

    Scene        scene;
    Registry&    r     = scene.GetRegistry();
    const Entity splat = scene.CreateEntity("Splat");
    r.Emplace<Decal>(splat, Decal{.halfExtents = {1.0f, 2.0f, 0.5f}, .albedo = PathToUtf8(texture), .normal = PathToUtf8(texture),
                                  .color = {1.0f, 0.5f, 0.25f, 0.8f}, .emissiveColor = {0.0f, 2.0f, 0.0f}, .roughness = 0.3f,
                                  .metallic = 0.7f, .sortOrder = 4, .lifetime = 5.0f, .fadeOut = 2.0f});
    const Entity mist = scene.CreateEntity("Mist");
    r.Emplace<FogVolume>(mist, FogVolume{.halfExtents = {3.0f, 1.0f, 4.0f}, .density = 0.4f, .albedo = {0.9f, 0.8f, 0.7f},
                                         .emissive = {0.1f, 0.0f, 0.0f}, .edgeFade = 0.5f});
    const fs::path file = dir / "Levels" / "A.scene.json";
    fs::create_directories(file.parent_path());
    SaveSceneFile(file, scene, nullptr);
    std::ifstream     in(file);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"../Textures/Splat.png\"") != std::string::npos); // relative to the scene
    CHECK(text.find("\"fogVolume\"") != std::string::npos);

    Scene loaded;
    CHECK(LoadSceneFile(file, loaded, nullptr).empty());
    const Entity backSplat = loaded.FindByUuid(r.Get<Uuid>(splat).value);
    const Entity backMist  = loaded.FindByUuid(r.Get<Uuid>(mist).value);
    const auto*  decal     = backSplat != NullEntity ? loaded.GetRegistry().TryGet<Decal>(backSplat) : nullptr;
    const auto*  fog       = backMist != NullEntity ? loaded.GetRegistry().TryGet<FogVolume>(backMist) : nullptr;
    CHECK(decal != nullptr && fog != nullptr);
    if (decal) {
        std::error_code ec;
        CHECK(fs::equivalent(PathFromUtf8(decal->albedo), texture, ec) && fs::equivalent(PathFromUtf8(decal->normal), texture, ec));
        Decal expected = r.Get<Decal>(splat), actual = *decal;
        expected.albedo = expected.normal = actual.albedo = actual.normal = {};
        CHECK(actual == expected);
    }
    if (fog)
        CHECK(*fog == r.Get<FogVolume>(mist));

    // Undo snapshots carry both components; broken values are clamped on load.
    const std::string state = SnapshotEntityState(scene, mist);
    r.Get<FogVolume>(mist).density = 9.0f;
    ApplyEntityState(scene, mist, state);
    CHECK(r.Get<FogVolume>(mist).density == 0.4f);
    {
        std::ofstream(file) << R"({"version": 1, "entities": [{"uuid": 7, "name": "Bad", "transform": {},
            "fogVolume": {"halfExtents": [-2, 0, 1], "density": -5, "albedo": [3, -1, 0.5], "edgeFade": -1},
            "decal": {"roughness": 7, "angleFade": 120, "halfExtents": [0, 0, 0]}}]})";
        Scene bad;
        CHECK(LoadSceneFile(file, bad, nullptr).empty());
        const Entity e = bad.FindByUuid(7);
        CHECK(e != NullEntity);
        if (e != NullEntity) {
            const FogVolume& f = bad.GetRegistry().Get<FogVolume>(e);
            CHECK(f.halfExtents.x == 2.0f && f.halfExtents.y > 0.0f && f.density == 0.0f && f.edgeFade == 0.0f);
            CHECK(f.albedo == glm::vec3(1.0f, 0.0f, 0.5f));
            const Decal& d = bad.GetRegistry().Get<Decal>(e);
            CHECK(d.roughness == 1.0f && d.angleFade == 89.0f && d.halfExtents.x > 0.0f);
        }
    }
}

TEST_CASE(Rendering_DecalLifetimeAndBlueprintSpawn)
{
    // Lifetime: ages, then the entity goes; lifetime 0 stays forever.
    {
        Scene        scene;
        const Entity timed   = scene.CreateEntity("Timed");
        const Entity forever = scene.CreateEntity("Forever");
        scene.GetRegistry().Emplace<Decal>(timed, Decal{.lifetime = 1.0f, .fadeOut = 0.5f});
        scene.GetRegistry().Emplace<Decal>(forever, Decal{});
        UpdateDecals(scene, 0.6f);
        CHECK(scene.GetRegistry().Valid(timed) && std::abs(scene.GetRegistry().Get<Decal>(timed).age - 0.6f) < 1e-5f);
        UpdateDecals(scene, 0.6f);
        CHECK(!scene.GetRegistry().Valid(timed));
        CHECK(scene.GetRegistry().Valid(forever) && scene.GetRegistry().Get<Decal>(forever).age == 0.0f);
    }

    using namespace BlueprintTest;
    Runner r;
    Graph  b;
    const auto begin = b.Node("Event.BeginPlay");
    const auto spawn = b.Node("Decal.SpawnAt");
    b.Set(spawn, "Texture", std::string("Textures/Splat.png"));
    b.Set(spawn, "Location", glm::vec3(1.0f, 0.0f, 2.0f));
    b.Set(spawn, "Normal", glm::vec3(1.0f, 0.0f, 0.0f)); // a wall facing +X
    b.Set(spawn, "Size", 2.0f);
    b.Set(spawn, "Lifetime", 3.0f);
    b.Set(spawn, "Color", glm::vec3(1.0f, 0.0f, 0.0f));
    const auto bad = b.Node("Decal.SpawnAt");
    b.Set(bad, "Normal", glm::vec3(0.0f));
    const auto has   = b.Node("Entity.HasComponent", "Decal");
    const auto print = b.Node("Debug.Print");
    b.Link(begin, "Out", spawn, "In");
    b.Link(spawn, "Then", bad, "In");
    b.Link(bad, "Then", print, "In");
    b.Link(spawn, "Spawned", has, "Target");
    b.Link(has, "Result", print, "Text");
    CHECK(b.Valid());
    (void)r.Add("Painter", "painter.ugraph", b.g);
    r.scripts.Begin(r.scene);
    r.Run(0.1f);
    CHECK(r.Printed("true"));
    CHECK(r.scripts.Stats().errors >= 1); // zero normal reported
    Entity spawned = NullEntity;
    r.scene.GetRegistry().ViewOf<Decal>().Each([&](Entity e, const Decal&) { spawned = e; });
    CHECK(spawned != NullEntity);
    if (spawned != NullEntity) {
        r.scene.UpdateTransforms();
        const Decal&     d     = r.scene.GetRegistry().Get<Decal>(spawned);
        const glm::mat4& world = r.scene.GetRegistry().Get<WorldTransform>(spawned).matrix;
        CHECK(std::abs(d.halfExtents.x - 1.0f) < 1e-5f && d.lifetime == 3.0f && d.color == glm::vec4(1, 0, 0, 1));
        CHECK(PathFromUtf8(d.albedo).is_absolute());
        CHECK(glm::length(glm::vec3(world[3]) - glm::vec3(1.0f, 0.0f, 2.0f)) < 1e-4f);
        CHECK(glm::dot(glm::normalize(glm::vec3(world[2])), glm::vec3(1.0f, 0.0f, 0.0f)) > 0.999f); // +Z = the normal
    }
}
