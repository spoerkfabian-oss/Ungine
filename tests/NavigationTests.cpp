#include "Test.h"

#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Navigation/NavigationWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <filesystem>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path NavTempDir(const char* name)
{
    const fs::path  dir = fs::temp_directory_path() / "ungine_tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

Entity Box(Scene& scene, const char* name, const glm::vec3& position, const glm::vec3& half)
{
    const Entity e = scene.CreateEntity(name);
    scene.EditTransform(e).position = position;
    Collider c;
    c.shape       = ColliderShape::Box;
    c.halfExtents = half;
    scene.GetRegistry().Emplace<Collider>(e, c);
    return e;
}

// 20 x 20 m ground (top at y = 0) inside a navmesh bounds volume.
void Ground(Scene& scene)
{
    Box(scene, "Ground", {0.0f, -0.5f, 0.0f}, {10.0f, 0.5f, 10.0f});
    const Entity bounds = scene.CreateEntity("Bounds");
    scene.GetRegistry().Emplace<NavMeshBounds>(bounds, NavMeshBounds{.halfExtents = {12.0f, 4.0f, 12.0f}});
}

void Settle(NavigationWorld& nav, Scene& scene, int maxUpdates = 400)
{
    for (int i = 0; i < maxUpdates; ++i) {
        nav.Update(scene, 1.0f / 30.0f, false);
        if (!nav.IsBuilding())
            break;
    }
}

bool Near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

} // namespace

TEST_CASE(Navigation_BuildPathsQueriesAndRebuild)
{
    Scene scene;
    Ground(scene);
    const Entity wall = Box(scene, "Wall", {0.0f, 1.0f, 0.0f}, {0.25f, 1.0f, 6.0f});

    NavigationWorld nav;
    nav.Build(scene);
    CHECK(!nav.IsBuilding());
    CHECK(nav.Stats().tiles > 0 && nav.Stats().polygons > 0 && nav.Stats().sources == 2);

    // Around the wall (6 m to either side).
    const NavPath around = nav.FindPath({-5.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f});
    CHECK(around.status == NavPathStatus::Complete);
    CHECK(around.Length() > 14.0f && around.Length() < 20.0f);
    CHECK(Near(around.points.front().x, -5.0f, 0.3f) && Near(around.points.back().x, 5.0f, 0.3f));
    for (const glm::vec3& p : around.points)
        CHECK(std::abs(p.z) < 0.1f || std::abs(p.z) > 6.0f); // corners only beyond the wall's ends

    // Raycast stops at the wall (eroded by the agent radius 0.4).
    const NavRaycastHit hit = nav.Raycast({-5.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f});
    CHECK(hit.hit && hit.point.x < -0.5f && hit.point.x > -1.0f);
    CHECK(!nav.Raycast({-5.0f, 0.0f, 8.0f}, {5.0f, 0.0f, 8.0f}).hit);

    // Projection and random points stay on the ground.
    const auto projected = nav.ProjectPoint({3.0f, 1.5f, 3.0f});
    CHECK(projected && Near(projected->y, 0.0f, 0.25f) && Near(projected->x, 3.0f, 0.01f));
    CHECK(!nav.ProjectPoint({50.0f, 0.0f, 0.0f}));
    nav.SetRandomSeed(7);
    for (int i = 0; i < 20; ++i) {
        const auto p = nav.RandomPoint();
        CHECK(p && std::abs(p->x) < 10.0f && std::abs(p->z) < 10.0f && Near(p->y, 0.0f, 0.25f));
        const auto q = nav.RandomPointInRadius({-5.0f, 0.0f, 0.0f}, 2.0f);
        CHECK(q && glm::distance(*q, glm::vec3(-5.0f, 0.0f, 0.0f)) < 3.0f);
    }

    // Moving the wall away rebuilds the touched tiles (autoRebuild): straight path.
    scene.EditTransform(wall).position = {0.0f, 1.0f, 30.0f};
    Settle(nav, scene);
    CHECK(Near(nav.FindPath({-5.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}).Length(), 10.0f, 0.2f));

    // Without autoRebuild the navmesh keeps its state until Build.
    nav.autoRebuild                    = false;
    scene.EditTransform(wall).position = {0.0f, 1.0f, 0.0f};
    Settle(nav, scene);
    CHECK(Near(nav.FindPath({-5.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}).Length(), 10.0f, 0.2f));
    nav.Build(scene);
    CHECK(nav.FindPath({-5.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}).Length() > 14.0f);
}

TEST_CASE(Navigation_AreasObstaclesLinksAndBakedFiles)
{
    Scene scene;
    Ground(scene);
    NavSettings settings;
    settings.areas.push_back(NavAreaType{.name = "Mud", .cost = 4.0f, .color = {0.4f, 0.3f, 0.1f}});
    NavigationWorld nav;
    nav.SetSettings(settings);
    const std::uint32_t mud = 1u << *nav.Settings().Area("Mud");

    // A mud strip across the middle (x in [-1, 1], z in [-8, 8]): costs more, can be excluded.
    const Entity strip = scene.CreateEntity("Mud");
    scene.EditTransform(strip).position = {0.0f, 0.0f, 0.0f};
    scene.GetRegistry().Emplace<NavArea>(strip, NavArea{.halfExtents = {1.0f, 1.0f, 8.0f}, .area = "Mud"});
    nav.Build(scene);
    const glm::vec3 a{-5.0f, 0.0f, 0.0f}, b{5.0f, 0.0f, 0.0f};
    CHECK(nav.FindPath(a, b).status == NavPathStatus::Complete);
    const NavPath avoid = nav.FindPath(a, b, 0, ~mud);
    CHECK(avoid.status == NavPathStatus::Complete && avoid.Length() > 16.0f); // around the strip's ends
    bool sawMud = false;
    nav.ForEachTriangle(0, [&](const glm::vec3&, const glm::vec3&, const glm::vec3&, std::uint32_t area) {
        sawMud = sawMud || (1u << area) == mud;
    });
    CHECK(sawMud);

    // Blocked: the strip removes the surface for everyone.
    scene.GetRegistry().Get<NavArea>(strip).area = std::string(kNavAreaBlocked);
    scene.GetRegistry().Get<NavArea>(strip).halfExtents.z = 12.0f;
    Settle(nav, scene);
    CHECK(nav.FindPath(a, b).status == NavPathStatus::Partial);

    // Off-mesh link over the gap: the path uses it.
    const Entity link = scene.CreateEntity("Jump");
    scene.GetRegistry().Emplace<NavLink>(link, NavLink{.start = {-1.8f, 0.0f, 0.0f}, .end = {1.8f, 0.0f, 0.0f}});
    Settle(nav, scene);
    const NavPath jump = nav.FindPath(a, b);
    CHECK(jump.status == NavPathStatus::Complete && Near(jump.Length(), 10.0f, 0.5f));
    scene.GetRegistry().Get<NavLink>(link).bidirectional = false;
    Settle(nav, scene);
    CHECK(nav.FindPath(a, b).status == NavPathStatus::Complete);
    CHECK(nav.FindPath(b, a).status == NavPathStatus::Partial); // one way only
    scene.DestroyEntity(link);
    scene.DestroyEntity(strip);
    Settle(nav, scene);
    CHECK(Near(nav.FindPath(a, b).Length(), 10.0f, 0.2f));

    // Obstacle: cut out without rebuilding geometry; moves with its entity; disabled = gone.
    const std::uint64_t built    = nav.Stats().tilesBuilt;
    const Entity        obstacle = scene.CreateEntity("Crate");
    scene.EditTransform(obstacle).position = {0.0f, 0.0f, 0.0f};
    scene.GetRegistry().Emplace<NavObstacle>(obstacle, NavObstacle{.halfExtents = {0.5f, 1.0f, 9.0f}});
    Settle(nav, scene);
    CHECK(nav.Stats().obstacles == 1 && nav.Stats().tilesBuilt == built);
    CHECK(nav.FindPath(a, b).Length() > 18.0f);
    scene.EditTransform(obstacle).position = {0.0f, 0.0f, 30.0f};
    Settle(nav, scene);
    CHECK(Near(nav.FindPath(a, b).Length(), 10.0f, 0.2f));
    scene.EditTransform(obstacle).position = {0.0f, 0.0f, 0.0f};
    Settle(nav, scene);
    CHECK(nav.FindPath(a, b).Length() > 18.0f);
    scene.GetRegistry().Get<NavObstacle>(obstacle).enabled = false;
    Settle(nav, scene);
    CHECK(Near(nav.FindPath(a, b).Length(), 10.0f, 0.2f));
    scene.DestroyEntity(obstacle);

    // Baked file: a new world loads the tiles and rebuilds nothing for the same scene.
    const fs::path dir  = NavTempDir("navigation_baked");
    const fs::path file = NavMeshFileFor(dir / "Level.scene.json");
    CHECK(file.filename() == "Level.unav");
    const Entity wall  = Box(scene, "Wall", {0.0f, 1.0f, 0.0f}, {0.25f, 1.0f, 6.0f});
    const Entity crate = Box(scene, "Crate", {7.0f, 0.5f, 7.0f}, {0.5f, 0.5f, 0.5f});
    const std::uint64_t before = nav.Stats().tilesBuilt;
    nav.Build(scene);
    const std::uint64_t fullBuild = nav.Stats().tilesBuilt - before;
    std::string error;
    CHECK(nav.SaveBaked(file, &error));
    NavigationWorld loaded;
    loaded.SetSettings(settings);
    CHECK(loaded.LoadBaked(file, &error));
    CHECK(loaded.FindPath(a, b).Length() > 14.0f && loaded.Stats().tilesBuilt == 0);
    Settle(loaded, scene);
    CHECK(loaded.Stats().tilesBuilt == 0 && loaded.FindPath(a, b).Length() > 14.0f);
    // Only the tiles of a changed source are rebuilt.
    scene.EditTransform(crate).position = {7.0f, 0.5f, 6.5f};
    Settle(loaded, scene);
    CHECK(loaded.Stats().tilesBuilt > 0 && loaded.Stats().tilesBuilt * 2 < fullBuild);
    scene.EditTransform(wall).position = {0.0f, 1.0f, 30.0f};
    Settle(loaded, scene);
    CHECK(Near(loaded.FindPath(a, b).Length(), 10.0f, 0.2f));
    // Other settings: refused.
    NavigationWorld other;
    NavSettings     coarse = settings;
    coarse.cellSize        = 0.3f;
    other.SetSettings(coarse);
    CHECK(!other.LoadBaked(file, &error) && !error.empty());
    CHECK(!other.LoadBaked(dir / "missing.unav", &error));
}
