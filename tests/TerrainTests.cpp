// Phase 32 terrain (CPU): data model, files, heightmap exchange, store change tracking, the Jolt
// height field and the navmesh source. Rendering is covered by the GPU tests.
#include "Test.h"

#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Navigation/NavigationWorld.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Terrain/TerrainData.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path TerrainTempDir(const char* name)
{
    const fs::path  dir = fs::temp_directory_path() / "ungine_tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool Near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

void SetHeight(TerrainData& t, std::uint32_t x, std::uint32_t z, float meters)
{
    t.heights[static_cast<std::size_t>(z) * t.resolution + x] =
        static_cast<std::uint16_t>(std::clamp(meters / t.heightScale, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

// Holes in the cells [x0, x1) x [z0, z1).
void PaintHoles(TerrainData& t, std::uint32_t x0, std::uint32_t z0, std::uint32_t x1, std::uint32_t z1)
{
    t.holes.assign(static_cast<std::size_t>(t.resolution) * t.resolution, 0);
    for (std::uint32_t z = z0; z < z1; ++z)
        for (std::uint32_t x = x0; x < x1; ++x)
            t.holes[static_cast<std::size_t>(z) * t.resolution + x] = 1;
}

Entity PlaceTerrain(Scene& scene, const std::string& path, const glm::vec3& position)
{
    const Entity e = scene.CreateEntity("Terrain");
    scene.EditTransform(e).position = position;
    scene.GetRegistry().Emplace<Terrain>(e, Terrain{.data = path});
    return e;
}

} // namespace

TEST_CASE(Terrain_DataQueriesFilesAndHeightmaps)
{
    // Flat terrain: 33 samples over 32 m (1 m cells), 2 m high.
    TerrainData t = MakeFlatTerrain(33, 32.0f, 16.0f, 2.0f);
    CHECK(t.heights.size() == 33u * 33u && t.layers.size() == 1);
    CHECK(t.splat.size() == static_cast<std::size_t>(t.splatResolution) * t.splatResolution * kMaxTerrainLayers);
    CHECK(Near(TerrainHeight(t, {10.3f, 7.9f}), 2.0f, 0.001f));
    CHECK(Near(TerrainNormal(t, {10.0f, 10.0f}).y, 1.0f, 1e-4f));

    // Ramp along x (0.25 m per meter): bilinear height, tilted normal.
    for (std::uint32_t z = 0; z < 33; ++z)
        for (std::uint32_t x = 0; x < 33; ++x)
            SetHeight(t, x, z, 0.25f * static_cast<float>(x));
    CHECK(Near(TerrainHeight(t, {10.5f, 3.0f}), 2.625f, 0.002f));
    const glm::vec3 n = TerrainNormal(t, {10.0f, 10.0f});
    CHECK(n.x < -0.2f && Near(n.z, 0.0f, 1e-3f) && Near(glm::length(n), 1.0f, 1e-3f));

    // Holes are per cell (to the +x/+z of their sample).
    PaintHoles(t, 4, 4, 6, 6);
    CHECK(TerrainHole(t, {4.5f, 4.5f}) && TerrainHole(t, {5.9f, 5.1f}));
    CHECK(!TerrainHole(t, {3.9f, 4.5f}) && !TerrainHole(t, {6.1f, 5.0f}));

    // Raycast: from above onto the ramp, through the hole, from the side, starting below.
    const auto down = RaycastTerrain(t, {12.0f, 10.0f, 12.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
    CHECK(down && Near(*down, 10.0f - 3.0f, 0.01f));
    CHECK(!RaycastTerrain(t, {5.0f, 10.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, 20.0f));
    CHECK(!RaycastTerrain(t, {12.0f, 10.0f, 12.0f}, {0.0f, -1.0f, 0.0f}, 5.0f)); // too short
    const auto side = RaycastTerrain(t, {-5.0f, 3.0f, 20.0f}, {1.0f, 0.0f, 0.0f}, 100.0f);
    CHECK(side && Near(*side, 5.0f + 12.0f, 0.05f)); // height 3 at x = 12
    CHECK(!RaycastTerrain(t, {-5.0f, 3.0f, 40.0f}, {1.0f, 0.0f, 0.0f}, 100.0f));    // misses the box

    // File round trip: layer/foliage paths relative to the file, arrays exact.
    const fs::path dir = TerrainTempDir("terrain_files");
    t.layers[0].albedo = PathToUtf8(dir / "Textures" / "Grass.png");
    t.layers.push_back(TerrainLayer{.name = "Rock", .normal = PathToUtf8(dir.parent_path() / "Shared" / "Rock_n.png"), .tiling = 2.0f});
    t.splat[3 * kMaxTerrainLayers + 1] = 200;
    FoliageLayer grass;
    grass.texture = PathToUtf8(dir / "Textures" / "Blade.png");
    grass.densityMap.assign(static_cast<std::size_t>(t.splatResolution) * t.splatResolution, 0);
    grass.densityMap[17] = 255;
    t.foliage.push_back(grass);
    t.triplanarSlope = 35.0f;
    const fs::path file = dir / "Island.uterrain";
    std::string    error;
    CHECK(SaveTerrain(file, t, &error));
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.rfind("UTRN", 0) == 0);
    CHECK(text.find("Textures/Grass.png") != std::string::npos && text.find("../Shared/Rock_n.png") != std::string::npos);
    CHECK(text.find(PathToUtf8(dir)) == std::string::npos); // nothing absolute
    const auto loaded = LoadTerrain(file, &error);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK(loaded->heights == t.heights && loaded->splat == t.splat && loaded->holes == t.holes);
        CHECK(loaded->layers == t.layers && loaded->foliage == t.foliage);
        CHECK(loaded->resolution == 33 && loaded->size == 32.0f && loaded->triplanarSlope == 35.0f);
    }
    // Broken files are refused with a message.
    std::ofstream(dir / "Broken.uterrain", std::ios::binary) << "UTRN garbage";
    error.clear();
    CHECK(!LoadTerrain(dir / "Broken.uterrain", &error) && !error.empty());
    CHECK(!LoadTerrain(dir / "Missing.uterrain", &error));

    // Sanitize: out-of-range values are clamped, arrays resized.
    TerrainData wild = t;
    wild.resolution  = 5;
    wild.layers.resize(12);
    wild.heights.resize(3);
    SanitizeTerrain(wild);
    CHECK(wild.resolution == kMinTerrainResolution && wild.layers.size() == kMaxTerrainLayers);
    CHECK(wild.heights.size() == static_cast<std::size_t>(wild.resolution) * wild.resolution);

    // Heightmaps: 16-bit PNG and RAW are exact at the same resolution, resampled otherwise.
    for (const char* name : {"height.png", "height.r16"}) {
        CHECK(ExportHeightmap(t, dir / name, &error));
        TerrainData other = MakeFlatTerrain(33, 32.0f, 16.0f, 0.0f);
        CHECK(ImportHeightmap(other, dir / name, &error));
        CHECK(other.heights == t.heights);
        TerrainData big = MakeFlatTerrain(65, 32.0f, 16.0f, 0.0f);
        CHECK(ImportHeightmap(big, dir / name, &error));
        CHECK(big.heights.size() == 65u * 65u);
        CHECK(Near(TerrainHeight(big, {10.5f, 3.0f}), 2.625f, 0.01f)); // same ramp, finer grid
        CHECK(big.heights[64] == t.heights[32]);                       // corners kept
    }
    std::ofstream(dir / "odd.raw", std::ios::binary) << "abc";
    CHECK(!ImportHeightmap(t, dir / "odd.raw", &error) && !error.empty());
    CHECK(!ImportHeightmap(t, dir / "nothing.png", &error));
}

TEST_CASE(Terrain_StoreTracksEditsAndFiles)
{
    TerrainStore& store = TerrainStore::Instance();
    store.Clear();
    const fs::path    dir  = TerrainTempDir("terrain_store");
    const std::string path = PathToUtf8(dir / "Field.uterrain");
    CHECK(!store.Get(path) && store.Revision(path) == 0); // missing

    auto data = std::make_shared<TerrainData>(MakeFlatTerrain(33, 32.0f, 16.0f, 1.0f));
    store.Provide(path, data);
    CHECK(store.Get(path) == data && store.Dirty(path));
    const std::uint64_t r0 = store.Revision(path);
    CHECK(r0 > 0 && store.ChangesSince(path, r0).kinds == 0);
    CHECK(store.ChangesSince(path, 0).full);

    // Partial edits: union of the rectangles, kinds combined.
    store.MarkDirty(path, TerrainChangeHeights, {0.1f, 0.2f, 0.3f, 0.4f});
    const std::uint64_t r1 = store.Revision(path);
    store.MarkDirty(path, TerrainChangeSplat, {0.5f, 0.1f, 0.6f, 0.25f});
    TerrainChanges c = store.ChangesSince(path, r0);
    CHECK(!c.full && c.kinds == (TerrainChangeHeights | TerrainChangeSplat));
    CHECK(c.rect == glm::vec4(0.1f, 0.1f, 0.6f, 0.4f));
    c = store.ChangesSince(path, r1);
    CHECK(c.kinds == TerrainChangeSplat && c.rect == glm::vec4(0.5f, 0.1f, 0.6f, 0.25f));
    // Settings changes and too many edits are full changes.
    store.MarkDirty(path, TerrainChangeSettings);
    CHECK(store.ChangesSince(path, r1).full);
    const std::uint64_t r2 = store.Revision(path);
    for (int i = 0; i < 300; ++i)
        store.MarkDirty(path, TerrainChangeHoles, {0.0f, 0.0f, 0.01f, 0.01f});
    CHECK(store.ChangesSince(path, r2).full);

    // Save clears the dirty flag; the file is picked up by a fresh store state.
    std::string error;
    CHECK(store.Save(path, &error) && !store.Dirty(path) && fs::exists(dir / "Field.uterrain"));
    data->heights[0] = 999;
    store.MarkDirty(path, TerrainChangeHeights, {0.0f, 0.0f, 0.05f, 0.05f});
    CHECK(store.Dirty(path));
    store.Reload(path); // drops the edit
    const auto reloaded = store.Get(path);
    CHECK(reloaded && reloaded != data && reloaded->heights[0] != 999 && !store.Dirty(path));
    store.Clear();
    CHECK(store.Get(path) && store.Get(path)->resolution == 33);
    CHECK(!store.Save(PathToUtf8(dir / "Unknown.uterrain"), &error) && !error.empty());

    // Scene files keep the terrain path relative to the scene.
    Scene        scene;
    const Entity e = PlaceTerrain(scene, path, {1.0f, 2.0f, 3.0f});
    Terrain&     t = scene.GetRegistry().Get<Terrain>(e);
    t.castShadows = false, t.lodDistance = 40.0f, t.foliageDensity = 0.5f;
    const fs::path sceneFile = dir / "Level.scene.json";
    SaveSceneFile(sceneFile, scene, nullptr);
    std::ifstream     in(sceneFile);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"Field.uterrain\"") != std::string::npos);
    Scene back;
    CHECK(LoadSceneFile(sceneFile, back, nullptr).empty());
    bool found = false;
    back.GetRegistry().ViewOf<Terrain>().Each([&](Entity, Terrain& bt) {
        found = bt.data == path && !bt.castShadows && bt.lodDistance == 40.0f && bt.foliageDensity == 0.5f;
    });
    CHECK(found);
    store.Clear();
}

TEST_CASE(Terrain_PhysicsHeightField)
{
    TerrainStore& store = TerrainStore::Instance();
    store.Clear();
    // 32 x 32 m centered on (0, 0), base height 1 m, a 3 m hill at (+8, +8) and a 4 x 4-cell hole
    // around (-8, -8) (cells 4..8 from the -X/-Z corner at -16).
    auto data = std::make_shared<TerrainData>(MakeFlatTerrain(33, 32.0f, 16.0f, 1.0f));
    for (std::uint32_t z = 0; z < 33; ++z)
        for (std::uint32_t x = 0; x < 33; ++x) {
            const float d = glm::length(glm::vec2(static_cast<float>(x) - 24.0f, static_cast<float>(z) - 24.0f));
            SetHeight(*data, x, z, 1.0f + 3.0f * std::max(0.0f, 1.0f - d / 5.0f));
        }
    PaintHoles(*data, 6, 6, 10, 10);
    const std::string path = PathToUtf8(TerrainTempDir("terrain_physics") / "Physics.uterrain");
    store.Provide(path, data);

    ThreadPool   pool{2};
    EventBus     bus;
    Scene        scene;
    PhysicsWorld physics{pool, bus};
    const Entity terrain = PlaceTerrain(scene, path, {0.0f, 0.5f, 0.0f});
    physics.Sync(scene);
    CHECK(physics.HasBody(terrain));

    const auto flat = physics.Raycast({0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
    CHECK(flat && flat->entity == terrain && Near(flat->point.y, 1.5f, 0.02f) && flat->normal.y > 0.99f);
    const auto hill = physics.Raycast({8.0f, 10.0f, 8.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
    CHECK(hill && Near(hill->point.y, 4.5f, 0.05f));
    CHECK(!physics.Raycast({-9.0f, 10.0f, -9.0f}, {0.0f, -1.0f, 0.0f}, 20.0f)); // inside the hole
    CHECK(physics.Raycast({-11.5f, 10.0f, -9.0f}, {0.0f, -1.0f, 0.0f}, 20.0f)); // next to it

    // A ball comes to rest on the ground, one dropped into the hole falls through.
    const auto ball = [&](const char* name, glm::vec3 position) {
        const Entity e = scene.CreateEntity(name);
        scene.EditTransform(e).position = position;
        scene.GetRegistry().Emplace<RigidBody>(e, RigidBody{.type = BodyType::Dynamic});
        scene.GetRegistry().Emplace<Collider>(e, Collider{.shape = ColliderShape::Sphere, .radius = 0.25f});
        return e;
    };
    const Entity resting = ball("Resting", {2.0f, 4.0f, 2.0f});
    const Entity falling = ball("Falling", {-8.0f, 4.0f, -8.0f});
    for (int i = 0; i < 180; ++i)
        physics.Step(scene, 1.0f / 60.0f);
    CHECK(Near(scene.GetRegistry().Get<WorldTransform>(resting).matrix[3].y, 1.75f, 0.05f));
    CHECK(scene.GetRegistry().Get<WorldTransform>(falling).matrix[3].y < -2.0f);

    // A sculpt (store edit) rebuilds the field; moving the entity moves it.
    SetHeight(*data, 16, 16, 6.0f);
    store.MarkDirty(path, TerrainChangeHeights, {0.45f, 0.45f, 0.55f, 0.55f});
    physics.Step(scene, 1.0f / 60.0f);
    const auto bump = physics.Raycast({0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
    CHECK(bump && Near(bump->point.y, 6.5f, 0.05f));
    scene.EditTransform(terrain).position = {0.0f, 2.5f, 0.0f};
    physics.Step(scene, 1.0f / 60.0f);
    const auto moved = physics.Raycast({-4.0f, 10.0f, 4.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
    CHECK(moved && Near(moved->point.y, 3.5f, 0.02f));

    // Collision off / component removed: the body goes.
    scene.GetRegistry().Get<Terrain>(terrain).collision = false;
    physics.Sync(scene);
    CHECK(!physics.HasBody(terrain));
    scene.GetRegistry().Get<Terrain>(terrain).collision = true;
    physics.Sync(scene);
    CHECK(physics.HasBody(terrain));
    scene.GetRegistry().Remove<Terrain>(terrain);
    physics.Sync(scene);
    CHECK(!physics.HasBody(terrain));
    store.Clear();
}

TEST_CASE(Terrain_NavigationSource)
{
    TerrainStore& store = TerrainStore::Instance();
    store.Clear();
    // 24 x 24 m flat terrain with a 2 m wide hole strip across x in [-1, 1] (cells 11..13), open
    // only at the +Z end (the last 3 m).
    auto data = std::make_shared<TerrainData>(MakeFlatTerrain(25, 24.0f, 8.0f, 0.0f));
    PaintHoles(*data, 11, 0, 13, 21);
    const fs::path    dir  = TerrainTempDir("terrain_navigation");
    const std::string path = PathToUtf8(dir / "Nav.uterrain");
    store.Provide(path, data);

    Scene        scene;
    const Entity terrain = PlaceTerrain(scene, path, {0.0f, 0.0f, 0.0f});
    const Entity bounds  = scene.CreateEntity("Bounds");
    scene.GetRegistry().Emplace<NavMeshBounds>(bounds, NavMeshBounds{.halfExtents = {12.0f, 3.0f, 12.0f}});
    NavigationWorld nav;
    nav.Build(scene);
    CHECK(nav.Stats().polygons > 0);
    const glm::vec3 a{-5.0f, 0.0f, -6.0f}, b{5.0f, 0.0f, -6.0f};
    const NavPath   around = nav.FindPath(a, b);
    CHECK(around.status == NavPathStatus::Complete && around.Length() > 30.0f); // via the +Z gap
    CHECK(!nav.ProjectPoint({0.0f, 0.0f, -6.0f}).has_value() ||
          std::abs(nav.ProjectPoint({0.0f, 0.0f, -6.0f})->x) > 0.9f); // nothing inside the hole

    // Filling the hole (store edit) rebuilds only the touched tiles.
    std::ranges::fill(data->holes, std::uint8_t{0});
    store.MarkDirty(path, TerrainChangeHoles, {0.4f, 0.0f, 0.6f, 0.9f});
    const std::uint64_t before = nav.Stats().tilesBuilt;
    for (int i = 0; i < 400; ++i) {
        nav.Update(scene, 1.0f / 30.0f, false);
        if (!nav.IsBuilding() && nav.Stats().tilesBuilt > before)
            break;
    }
    // Edits are checked at most twice a second: flush after waiting out the throttle.
    for (int i = 0; i < 100 && nav.Stats().tilesBuilt == before; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        nav.Flush(scene);
    }
    CHECK(Near(nav.FindPath(a, b).Length(), 10.0f, 0.2f));

    // Heights follow the terrain (a raised plateau), the entity position too.
    for (std::uint32_t z = 0; z < 25; ++z)
        for (std::uint32_t x = 0; x < 25; ++x)
            SetHeight(*data, x, z, 1.0f);
    store.MarkDirty(path, TerrainChangeHeights);
    scene.EditTransform(terrain).position = {0.0f, 0.5f, 0.0f};
    nav.Build(scene);
    const auto projected = nav.ProjectPoint({3.0f, 5.0f, 3.0f});
    CHECK(projected && Near(projected->y, 1.5f, 0.25f));

    // Baked file: a new world loads the tiles and rebuilds nothing for the unchanged terrain.
    const fs::path file = dir / "Nav.unav";
    std::string    error;
    CHECK(nav.SaveBaked(file, &error));
    NavigationWorld loaded;
    CHECK(loaded.LoadBaked(file, &error));
    for (int i = 0; i < 30; ++i)
        loaded.Update(scene, 1.0f / 30.0f, false);
    CHECK(loaded.Stats().tilesBuilt == 0 && Near(loaded.FindPath(a + glm::vec3(0, 1.5f, 0), b + glm::vec3(0, 1.5f, 0)).Length(), 10.0f, 0.2f));

    // Navigation off: the terrain no longer contributes.
    scene.GetRegistry().Get<Terrain>(terrain).navigation = false;
    nav.Build(scene);
    CHECK(nav.Stats().polygons == 0);
    store.Clear();
}
