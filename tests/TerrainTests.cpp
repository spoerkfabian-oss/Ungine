// Phase 32 terrain (CPU): data model, files, heightmap exchange, store change tracking, the Jolt
// height field and the navmesh source. Rendering is covered by the GPU tests.
#include "BlueprintTestUtil.h"
#include "Test.h"

#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Navigation/NavigationWorld.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Terrain/TerrainBrush.h"
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
        found = PathFromUtf8(bt.data).lexically_normal() == PathFromUtf8(path).lexically_normal() && !bt.castShadows && bt.lodDistance == 40.0f && bt.foliageDensity == 0.5f;
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

TEST_CASE(Terrain_BrushesAndUndoRegions)
{
    // 64 m, 65 samples (1 m cells), flat at 2 m; splat all on layer 0 of two.
    TerrainData t = MakeFlatTerrain(65, 64.0f, 32.0f, 2.0f);
    t.layers.push_back(TerrainLayer{.name = "Rock"});
    t.foliage.push_back(FoliageLayer{});
    SanitizeTerrain(t);
    const TerrainData original = t;
    TerrainBrush brush{.tool = TerrainTool::Raise, .radius = 4.0f, .strength = 1.0f, .falloff = 0.5f};
    CHECK(TerrainBrushWeight(brush, 0.0f) == 1.0f && TerrainBrushWeight(brush, 1.9f) == 1.0f);
    CHECK(TerrainBrushWeight(brush, 3.0f) > 0.0f && TerrainBrushWeight(brush, 3.0f) < 1.0f && TerrainBrushWeight(brush, 4.0f) == 0.0f);

    // Raise: the center goes up most, nothing outside the radius; the rect covers the dab.
    TerrainChanges c = ApplyTerrainBrush(t, brush, {32.0f, 32.0f}, 0.5f);
    CHECK(c.kinds == TerrainChangeHeights && !c.full);
    CHECK(c.rect.x >= 27.0f / 64.0f && c.rect.z <= 37.0f / 64.0f && c.rect.x < 0.5f && c.rect.z > 0.5f);
    const float center = TerrainHeight(t, {32.0f, 32.0f});
    CHECK(Near(center, 2.0f + 1.0f, 0.01f)); // strength 1 x radius / 2 m/s x 0.5 s
    CHECK(TerrainHeight(t, {34.5f, 32.0f}) > 2.05f && TerrainHeight(t, {34.5f, 32.0f}) < center);
    CHECK(Near(TerrainHeight(t, {37.0f, 32.0f}), 2.0f, 1e-3f));
    // Lower / Flatten / Smooth.
    brush.tool = TerrainTool::Lower;
    (void)ApplyTerrainBrush(t, brush, {32.0f, 32.0f}, 0.25f);
    CHECK(Near(TerrainHeight(t, {32.0f, 32.0f}), 2.5f, 0.01f));
    brush.tool   = TerrainTool::Flatten;
    brush.target = 5.0f;
    for (int i = 0; i < 40; ++i)
        (void)ApplyTerrainBrush(t, brush, {10.0f, 10.0f}, 0.1f);
    CHECK(Near(TerrainHeight(t, {10.0f, 10.0f}), 5.0f, 0.02f));
    const float edgeBefore = TerrainHeight(t, {13.0f, 10.0f});
    brush.tool = TerrainTool::Smooth;
    for (int i = 0; i < 20; ++i)
        (void)ApplyTerrainBrush(t, brush, {13.0f, 10.0f}, 0.1f);
    CHECK(TerrainHeight(t, {10.0f, 10.0f}) < 5.0f && std::abs(TerrainHeight(t, {13.0f, 10.0f}) - edgeBefore) > 0.01f);
    // Nothing touched: no change.
    brush.tool = TerrainTool::Raise;
    CHECK(ApplyTerrainBrush(t, brush, {200.0f, 200.0f}, 0.1f).kinds == 0);
    CHECK(ApplyTerrainBrush(t, brush, {32.0f, 32.0f}, 0.0f).kinds == 0);

    // Paint: layer 1 takes over, weights keep summing to 255.
    brush.tool  = TerrainTool::Paint;
    brush.layer = 1;
    for (int i = 0; i < 10; ++i)
        c = ApplyTerrainBrush(t, brush, {48.0f, 16.0f}, 0.1f);
    CHECK(c.kinds == TerrainChangeSplat);
    const auto weightsAt = [&](glm::vec2 p) {
        const auto x = static_cast<std::size_t>(p.x / t.size * static_cast<float>(t.splatResolution));
        const auto z = static_cast<std::size_t>(p.y / t.size * static_cast<float>(t.splatResolution));
        return &t.splat[(z * t.splatResolution + x) * kMaxTerrainLayers];
    };
    const std::uint8_t* w = weightsAt({48.0f, 16.0f});
    CHECK(w[1] == 255 && w[0] == 0);
    w = weightsAt({51.0f, 16.0f}); // falloff band: mixed
    CHECK(w[0] + w[1] == 255 && w[1] > 0 && w[0] > 0);
    brush.layer = 7; // no such layer
    CHECK(ApplyTerrainBrush(t, brush, {48.0f, 16.0f}, 0.1f).kinds == 0);

    // Holes: cells inside the solid part; erase fills them again.
    brush = TerrainBrush{.tool = TerrainTool::Holes, .radius = 2.0f, .falloff = 0.0f};
    c = ApplyTerrainBrush(t, brush, {20.0f, 40.0f}, 0.1f);
    CHECK(c.kinds == TerrainChangeHoles && TerrainHole(t, {20.2f, 40.2f}) && TerrainHole(t, {19.5f, 39.5f}));
    CHECK(!TerrainHole(t, {23.5f, 40.5f}));

    // Foliage density: painted, erased.
    brush = TerrainBrush{.tool = TerrainTool::Foliage, .radius = 3.0f, .strength = 1.0f, .layer = 0};
    for (int i = 0; i < 5; ++i)
        c = ApplyTerrainBrush(t, brush, {8.0f, 56.0f}, 0.1f);
    CHECK(c.kinds == TerrainChangeFoliage);
    const auto densityAt = [&](glm::vec2 p) {
        const auto x = static_cast<std::size_t>(p.x / t.size * static_cast<float>(t.splatResolution));
        const auto z = static_cast<std::size_t>(p.y / t.size * static_cast<float>(t.splatResolution));
        return t.foliage[0].densityMap[z * t.splatResolution + x];
    };
    CHECK(densityAt({8.0f, 56.0f}) == 255 && densityAt({14.0f, 56.0f}) == 0);
    brush.erase = true;
    for (int i = 0; i < 5; ++i)
        (void)ApplyTerrainBrush(t, brush, {8.0f, 56.0f}, 0.1f);
    CHECK(densityAt({8.0f, 56.0f}) == 0);

    // Undo regions: capture before, apply, restore -> identical; redo with the after capture.
    TerrainData   work   = original;
    const glm::vec4 rect{0.3f, 0.3f, 0.7f, 0.7f};
    const std::uint32_t kinds = TerrainChangeHeights | TerrainChangeSplat | TerrainChangeHoles | TerrainChangeFoliage;
    const TerrainRegion before = CaptureTerrainRegion(work, kinds, rect, 0);
    CHECK(before.holes.size() == static_cast<std::size_t>(before.sampleRect.z - before.sampleRect.x) * (before.sampleRect.w - before.sampleRect.y));
    TerrainBrush raise{.tool = TerrainTool::Raise, .radius = 6.0f, .strength = 1.0f};
    (void)ApplyTerrainBrush(work, raise, {32.0f, 32.0f}, 0.5f);
    TerrainBrush paint{.tool = TerrainTool::Paint, .radius = 6.0f, .strength = 1.0f, .layer = 1};
    (void)ApplyTerrainBrush(work, paint, {32.0f, 32.0f}, 0.5f);
    TerrainBrush hole{.tool = TerrainTool::Holes, .radius = 3.0f};
    (void)ApplyTerrainBrush(work, hole, {32.0f, 32.0f}, 0.1f);
    TerrainBrush grow{.tool = TerrainTool::Foliage, .radius = 6.0f, .strength = 1.0f};
    (void)ApplyTerrainBrush(work, grow, {32.0f, 32.0f}, 0.5f);
    const TerrainRegion after  = CaptureTerrainRegion(work, kinds, rect, 0);
    const TerrainData   edited = work;
    CHECK(RestoreTerrainRegion(work, before));
    CHECK(work.heights == original.heights && work.splat == original.splat && work.foliage == original.foliage);
    CHECK(std::ranges::all_of(work.holes, [](std::uint8_t v) { return v == 0; }));
    CHECK(RestoreTerrainRegion(work, after));
    CHECK(work.heights == edited.heights && work.splat == edited.splat && work.holes == edited.holes && work.foliage == edited.foliage);
    // A region from another resolution is refused.
    TerrainData other = MakeFlatTerrain(17, 16.0f);
    CHECK(!RestoreTerrainRegion(other, before));
}

TEST_CASE(Terrain_BlueprintGetHeight)
{
    TerrainStore& store = TerrainStore::Instance();
    store.Clear();
    auto data = std::make_shared<TerrainData>(MakeFlatTerrain(33, 32.0f, 16.0f, 2.0f));
    PaintHoles(*data, 0, 0, 4, 4); // the -X/-Z corner (world -16..-12)
    const std::string path = PathToUtf8(TerrainTempDir("terrain_blueprint") / "Bp.uterrain");
    store.Provide(path, data);

    using namespace BlueprintTest;
    Runner r;
    const Entity terrain = PlaceTerrain(r.scene, path, {0.0f, 1.0f, 0.0f});
    Graph b;
    b.g.variables.push_back({"Height", PinType::Float, 0.0f, false});
    b.g.variables.push_back({"Normal", PinType::Vec3, glm::vec3(0.0f), false});
    b.g.variables.push_back({"Hit", PinType::Entity, NullEntity, false});
    b.g.variables.push_back({"InHole", PinType::Bool, true, false});
    b.g.variables.push_back({"Outside", PinType::Bool, true, false});
    const auto begin = b.Node("Event.BeginPlay");
    const auto on    = b.Node("Terrain.GetHeight");
    b.Set(on, "Location", glm::vec3(3.0f, 50.0f, -2.0f));
    const auto hole = b.Node("Terrain.GetHeight");
    b.Set(hole, "Location", glm::vec3(-14.0f, 0.0f, -14.0f));
    const auto outside = b.Node("Terrain.GetHeight");
    b.Set(outside, "Location", glm::vec3(40.0f, 0.0f, 0.0f));
    const auto setHeight  = b.Node("Variable.Set", "Height");
    const auto setNormal  = b.Node("Variable.Set", "Normal");
    const auto setHit     = b.Node("Variable.Set", "Hit");
    const auto setHole    = b.Node("Variable.Set", "InHole");
    const auto setOutside = b.Node("Variable.Set", "Outside");
    b.Link(begin, "Out", setHeight, "In");
    b.Link(setHeight, "Then", setNormal, "In");
    b.Link(setNormal, "Then", setHit, "In");
    b.Link(setHit, "Then", setHole, "In");
    b.Link(setHole, "Then", setOutside, "In");
    b.Link(on, "Height", setHeight, "Value");
    b.Link(on, "Normal", setNormal, "Value");
    b.Link(on, "Terrain", setHit, "Value");
    b.Link(hole, "Found", setHole, "Value");
    b.Link(outside, "Found", setOutside, "Value");
    const auto has = b.Node("Entity.HasComponent", "Terrain");
    const auto print = b.Node("Debug.Print");
    b.Link(setOutside, "Then", print, "In");
    b.Link(on, "Terrain", has, "Target");
    b.Link(has, "Result", print, "Text");
    CHECK(b.Valid());
    const Entity actor = r.Add("Probe", "probe.ugraph", b.g);
    r.scene.UpdateTransforms();
    r.scripts.Begin(r.scene);
    r.Run(0.1f);
    const ScriptValue height = r.Var("probe.ugraph", actor, "Height");
    CHECK(std::holds_alternative<float>(height) && Near(std::get<float>(height), 3.0f, 0.01f)); // 1 m entity + 2 m ground
    const ScriptValue normal = r.Var("probe.ugraph", actor, "Normal");
    CHECK(std::holds_alternative<glm::vec3>(normal) && Near(std::get<glm::vec3>(normal).y, 1.0f, 1e-4f));
    const ScriptValue hit = r.Var("probe.ugraph", actor, "Hit");
    CHECK(std::holds_alternative<Entity>(hit) && std::get<Entity>(hit) == terrain);
    CHECK(r.Var("probe.ugraph", actor, "InHole") == ScriptValue(false));
    CHECK(r.Var("probe.ugraph", actor, "Outside") == ScriptValue(false));
    CHECK(r.Printed("true"));
    store.Clear();
}
