#include "Test.h"

#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Navigation/NavigationWorld.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptGraph.h"
#include "Engine/Script/ScriptSystem.h"

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

TEST_CASE(Navigation_CrowdAgentsMoveAvoidAndDriveCharacters)
{
    ThreadPool pool{2};
    Scene      scene;
    Ground(scene);
    Box(scene, "Wall", {0.0f, 1.0f, 0.0f}, {0.25f, 1.0f, 6.0f});

    // Tiles built in jobs equal the synchronous build.
    NavigationWorld sync;
    sync.Build(scene);
    NavigationWorld nav(&pool);
    nav.Build(scene);
    CHECK(nav.Stats().tiles == sync.Stats().tiles && nav.Stats().polygons == sync.Stats().polygons);
    CHECK(Near(nav.FindPath({-5, 0, 0}, {5, 0, 0}).Length(), sync.FindPath({-5, 0, 0}, {5, 0, 0}).Length(), 1e-4f));

    // Two agents swap sides around the wall's end, avoiding each other.
    const auto agent = [&](const char* name, const glm::vec3& at) {
        const Entity e = scene.CreateEntity(name);
        scene.EditTransform(e).position = at;
        scene.GetRegistry().Emplace<NavAgent>(e, NavAgent{.maxSpeed = 4.0f});
        return e;
    };
    const Entity left  = agent("Left", {-5.0f, 0.0f, 0.0f});
    const Entity right = agent("Right", {5.0f, 0.0f, 0.0f});
    nav.MoveTo(left, {5.0f, 0.0f, 0.0f});
    nav.MoveTo(right, {-5.0f, 0.0f, 0.0f});
    CHECK(nav.MoveStatus(left) == NavMoveStatus::Moving);
    float maxZ = 0.0f, minGap = 1e9f;
    for (int i = 0; i < 600 && (nav.MoveStatus(left) == NavMoveStatus::Moving ||
                                nav.MoveStatus(right) == NavMoveStatus::Moving); ++i) {
        nav.Update(scene, 1.0f / 30.0f);
        const glm::vec3 l = scene.GetRegistry().Get<Transform>(left).position;
        const glm::vec3 r = scene.GetRegistry().Get<Transform>(right).position;
        maxZ   = std::max(maxZ, std::abs(l.z));
        minGap = std::min(minGap, glm::distance(l, r));
    }
    CHECK(nav.MoveStatus(left) == NavMoveStatus::Arrived && nav.MoveStatus(right) == NavMoveStatus::Arrived);
    CHECK(glm::distance(scene.GetRegistry().Get<Transform>(left).position, glm::vec3(5.0f, 0.0f, 0.0f)) < 0.4f);
    CHECK(maxZ > 5.5f);       // went around the wall
    CHECK(minGap > 0.5f);     // never ran through each other (radius 0.4 each)
    CHECK(nav.Stats().agents == 2);
    // Facing its movement: the left agent ended up walking +X (forward = -Z rotated to +X).
    const glm::vec3 forward = scene.GetRegistry().Get<Transform>(left).rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    CHECK(forward.x > 0.5f);

    // Unreachable / off-mesh target: Failed. Stop: Idle.
    nav.MoveTo(left, {50.0f, 0.0f, 0.0f});
    nav.Update(scene, 1.0f / 30.0f);
    CHECK(nav.MoveStatus(left) == NavMoveStatus::Failed);
    nav.MoveTo(left, {-5.0f, 0.0f, 8.0f});
    nav.Update(scene, 1.0f / 30.0f);
    CHECK(nav.MoveStatus(left) == NavMoveStatus::Moving);
    nav.StopMovement(left);
    for (int i = 0; i < 30; ++i)
        nav.Update(scene, 1.0f / 30.0f);
    CHECK(nav.MoveStatus(left) == NavMoveStatus::Idle && glm::length(nav.AgentVelocity(left)) < 0.05f);
    // A request for an entity without NavAgent fails.
    const Entity plain = scene.CreateEntity("Plain");
    nav.MoveTo(plain, {0.0f, 0.0f, 8.0f});
    nav.Update(scene, 1.0f / 30.0f);
    CHECK(nav.MoveStatus(plain) == NavMoveStatus::Failed);
    // Removing the component removes the agent; teleporting re-places it.
    scene.GetRegistry().Remove<NavAgent>(right);
    nav.Update(scene, 1.0f / 30.0f);
    CHECK(!nav.HasAgent(right) && nav.Stats().agents == 1);
    scene.EditTransform(left).position = {-6.0f, 0.0f, -6.0f};
    nav.MoveTo(left, {-6.0f, 0.0f, 6.0f});
    for (int i = 0; i < 300 && nav.MoveStatus(left) == NavMoveStatus::Moving; ++i)
        nav.Update(scene, 1.0f / 30.0f);
    CHECK(nav.MoveStatus(left) == NavMoveStatus::Arrived);
    // Edit mode (no simulation): agents leave the crowd and stay put.
    nav.Update(scene, 1.0f / 30.0f, false);
    CHECK(!nav.HasAgent(left));
}

TEST_CASE(Navigation_AgentDrivesCharacterController)
{
    ThreadPool pool{2};
    EventBus   bus;
    Scene      scene;
    const Entity ground = Box(scene, "Ground", {0.0f, -0.5f, 0.0f}, {10.0f, 0.5f, 10.0f});
    scene.GetRegistry().Emplace<RigidBody>(ground, RigidBody{.type = BodyType::Static});
    const Entity bounds = scene.CreateEntity("Bounds");
    scene.GetRegistry().Emplace<NavMeshBounds>(bounds, NavMeshBounds{.halfExtents = {12.0f, 4.0f, 12.0f}});
    const Entity pillar = Box(scene, "Pillar", {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
    scene.GetRegistry().Emplace<RigidBody>(pillar, RigidBody{.type = BodyType::Static});

    PhysicsWorld    physics(pool, bus);
    NavigationWorld nav(&pool, nullptr, &physics);
    nav.Build(scene);
    CHECK(nav.Stats().sources == 2);

    const Entity walker = scene.CreateEntity("Walker");
    scene.EditTransform(walker).position = {-5.0f, 0.0f, 0.0f};
    scene.GetRegistry().Emplace<CharacterController>(walker, CharacterController{.radius = 0.3f, .height = 1.8f});
    scene.GetRegistry().Emplace<NavAgent>(walker);
    physics.Sync(scene);
    nav.MoveTo(walker, {5.0f, 0.0f, 0.0f});
    float maxZ = 0.0f;
    for (int i = 0; i < 600 && nav.MoveStatus(walker) == NavMoveStatus::Moving; ++i) {
        nav.Update(scene, 1.0f / 60.0f);
        physics.Step(scene, 1.0f / 60.0f);
        maxZ = std::max(maxZ, std::abs(scene.GetRegistry().Get<Transform>(walker).position.z));
    }
    CHECK(nav.MoveStatus(walker) == NavMoveStatus::Arrived);
    const glm::vec3 end = scene.GetRegistry().Get<Transform>(walker).position;
    CHECK(glm::distance(glm::vec2(end.x, end.z), glm::vec2(5.0f, 0.0f)) < 0.5f && Near(end.y, 0.0f, 0.1f));
    CHECK(maxZ > 1.0f); // around the pillar, moved by the physics

    // Over a link: two platforms with a 2 m gap (z in [4, 6]) the character cannot walk across;
    // the crowd carries it over the link and it walks on to the target.
    Scene gapScene;
    for (const float z : {-1.0f, 11.0f}) {
        const Entity platform = Box(gapScene, "Platform", {0.0f, -0.5f, z}, {6.0f, 0.5f, 5.0f});
        gapScene.GetRegistry().Emplace<RigidBody>(platform, RigidBody{.type = BodyType::Static});
    }
    const Entity gapBounds = gapScene.CreateEntity("Bounds");
    gapScene.GetRegistry().Emplace<NavMeshBounds>(gapBounds, NavMeshBounds{.halfExtents = {8.0f, 4.0f, 18.0f}});
    const Entity link = gapScene.CreateEntity("Link");
    gapScene.GetRegistry().Emplace<NavLink>(link, NavLink{.start = {0.0f, 0.0f, 3.5f}, .end = {0.0f, 0.0f, 6.5f}});
    PhysicsWorld    gapPhysics(pool, bus);
    NavigationWorld gapNav(&pool, nullptr, &gapPhysics);
    gapNav.Build(gapScene);
    const Entity jumper = gapScene.CreateEntity("Jumper");
    gapScene.EditTransform(jumper).position = {0.0f, 0.0f, 0.0f};
    gapScene.GetRegistry().Emplace<CharacterController>(jumper, CharacterController{.radius = 0.3f, .height = 1.8f});
    gapScene.GetRegistry().Emplace<NavAgent>(jumper);
    gapPhysics.Sync(gapScene);
    gapNav.MoveTo(jumper, {0.0f, 0.0f, 10.0f});
    float minY = 0.0f;
    for (int i = 0; i < 900 && gapNav.MoveStatus(jumper) == NavMoveStatus::Moving; ++i) {
        gapNav.Update(gapScene, 1.0f / 60.0f);
        gapPhysics.Step(gapScene, 1.0f / 60.0f);
        minY = std::min(minY, gapScene.GetRegistry().Get<Transform>(jumper).position.y);
    }
    CHECK(gapNav.MoveStatus(jumper) == NavMoveStatus::Arrived);
    const glm::vec3 landed = gapScene.GetRegistry().Get<Transform>(jumper).position;
    CHECK(landed.z > 9.0f && Near(landed.y, 0.0f, 0.1f) && minY > -0.5f); // never fell into the gap
}

TEST_CASE(Navigation_BlueprintNodesAndSerialization)
{
    Scene scene;
    Ground(scene);
    Box(scene, "Wall", {0.0f, 1.0f, 0.0f}, {0.25f, 1.0f, 6.0f});
    NavigationWorld nav;
    nav.Build(scene);

    EventBus     events;
    ScriptSystem scripts(events);
    scripts.SetNavigationWorld(&nav);
    ScriptGraph graph;
    const auto  node = [&](const char* type) { return graph.AddNode(type, {}, {}); };
    const auto  link = [&](std::uint32_t a, const char* pa, std::uint32_t b, const char* pb) {
        CHECK(graph.Connect(a, pa, b, pb).empty());
    };
    // BeginPlay: print the path length, move to (5, 0, 0); on Success move to an unreachable point;
    // on its Failed print "failed".
    const std::uint32_t begin = node("Event.BeginPlay");
    const std::uint32_t path  = node("AI.FindPath");
    graph.FindNode(path)->defaults["Start"] = glm::vec3(-5.0f, 0.0f, 0.0f);
    graph.FindNode(path)->defaults["End"]   = glm::vec3(5.0f, 0.0f, 0.0f);
    const std::uint32_t printLength = node("Debug.Print");
    const std::uint32_t move        = node("AI.MoveTo");
    graph.FindNode(move)->defaults["Location"] = glm::vec3(5.0f, 0.0f, 0.0f);
    const std::uint32_t arrived = node("Debug.Print");
    graph.FindNode(arrived)->defaults["Text"] = std::string("arrived");
    const std::uint32_t away = node("AI.MoveTo");
    graph.FindNode(away)->defaults["Location"] = glm::vec3(50.0f, 0.0f, 0.0f);
    const std::uint32_t failed = node("Debug.Print");
    graph.FindNode(failed)->defaults["Text"] = std::string("failed");
    link(begin, "Out", printLength, "In");
    link(path, "Length", printLength, "Text");
    link(printLength, "Then", move, "In");
    link(move, "Success", arrived, "In");
    link(arrived, "Then", away, "In");
    link(away, "Failed", failed, "In");
    // Pure queries on Tick: raycast hits the wall, random point found.
    const std::uint32_t tick = node("Event.Tick");
    const std::uint32_t ray  = node("AI.Raycast");
    graph.FindNode(ray)->defaults["Start"] = glm::vec3(-5.0f, 0.0f, 3.0f);
    graph.FindNode(ray)->defaults["End"]   = glm::vec3(5.0f, 0.0f, 3.0f);
    const std::uint32_t random = node("AI.RandomPoint");
    const std::uint32_t both   = node("Logic.And");
    const std::uint32_t check  = node("Flow.Branch");
    const std::uint32_t ok     = node("Debug.Print");
    graph.FindNode(ok)->defaults["Text"] = std::string("queries ok");
    link(ray, "Hit", both, "A");
    link(random, "Found", both, "B");
    link(tick, "Out", check, "In");
    link(both, "Result", check, "Condition");
    link(check, "True", ok, "In");
    CHECK(ValidateScriptGraph(graph).empty());

    const Entity actor = scene.CreateEntity("Actor");
    scene.EditTransform(actor).position = {-5.0f, 0.0f, 0.0f};
    scene.GetRegistry().Emplace<NavAgent>(actor, NavAgent{.maxSpeed = 5.0f});
    scene.GetRegistry().Emplace<ScriptComponent>(actor, ScriptComponent{"ai.ugraph"});
    scripts.Provide("ai.ugraph", graph);
    scripts.Begin(scene);
    CHECK(scripts.Stats().errors == 0);
    CHECK(!scripts.Messages().empty() && std::stof(scripts.Messages().front().text) > 14.0f);
    bool sawArrived = false, sawFailed = false, sawQueries = false;
    for (int i = 0; i < 600 && !sawFailed; ++i) {
        nav.Update(scene, 1.0f / 30.0f);
        scripts.Update(scene, 1.0f / 30.0f);
        for (const auto& m : scripts.Messages()) {
            sawArrived = sawArrived || m.text == "arrived";
            sawFailed  = sawFailed || m.text == "failed";
            sawQueries = sawQueries || m.text == "queries ok";
        }
    }
    CHECK(sawArrived && sawFailed && sawQueries);
    CHECK(glm::distance(scene.GetRegistry().Get<Transform>(actor).position, glm::vec3(5.0f, 0.0f, 0.0f)) < 0.5f);
    scripts.End(scene);

    // Components round-trip through scene files and entity state.
    const fs::path dir   = NavTempDir("navigation_scene");
    const Entity   extra = scene.CreateEntity("Extras");
    scene.GetRegistry().Emplace<NavArea>(extra, NavArea{.halfExtents = {1.0f, 2.0f, 3.0f}, .area = "Water"});
    scene.GetRegistry().Emplace<NavLink>(extra, NavLink{.start = {1, 0, 0}, .end = {4, 1, 0}, .radius = 0.3f,
                                                         .bidirectional = false, .area = "Rough", .profiles = 2u});
    scene.GetRegistry().Emplace<NavObstacle>(extra, NavObstacle{.shape = NavObstacleShape::Cylinder, .radius = 0.7f,
                                                                .height = 2.0f, .enabled = false});
    scene.GetRegistry().Get<NavAgent>(actor) =
        NavAgent{.profile = "Big", .maxSpeed = 2.0f, .avoidance = NavAvoidance::High, .faceMovement = false,
                 .areaMask = 5u};
    SaveSceneFile(dir / "Nav.scene.json", scene, nullptr);
    Scene loaded;
    (void)LoadSceneFile(dir / "Nav.scene.json", loaded, nullptr);
    const Entity actor2 = loaded.FindByUuid(scene.GetRegistry().Get<Uuid>(actor).value);
    const Entity extra2 = loaded.FindByUuid(scene.GetRegistry().Get<Uuid>(extra).value);
    CHECK(loaded.GetRegistry().Get<NavAgent>(actor2) == scene.GetRegistry().Get<NavAgent>(actor));
    CHECK(loaded.GetRegistry().Get<NavArea>(extra2) == scene.GetRegistry().Get<NavArea>(extra));
    CHECK(loaded.GetRegistry().Get<NavLink>(extra2) == scene.GetRegistry().Get<NavLink>(extra));
    CHECK(loaded.GetRegistry().Get<NavObstacle>(extra2) == scene.GetRegistry().Get<NavObstacle>(extra));
    bool hasBounds = false;
    loaded.GetRegistry().ViewOf<NavMeshBounds>().Each([&](Entity, NavMeshBounds& b) {
        hasBounds = b.halfExtents == glm::vec3(12.0f, 4.0f, 12.0f);
    });
    CHECK(hasBounds);
    const std::string state = SnapshotEntityState(scene, extra);
    scene.GetRegistry().Remove<NavLink>(extra);
    ApplyEntityState(scene, extra, state);
    CHECK(scene.GetRegistry().Has<NavLink>(extra));

    // Settings sanitizing: names unique, "Default" area first, values clamped.
    NavSettings settings;
    settings.profiles = {NavAgentProfile{.name = "A", .radius = -1.0f}, NavAgentProfile{.name = "A"}};
    settings.areas    = {NavAreaType{.name = "Mud", .cost = 0.5f}, NavAreaType{.name = std::string(kNavAreaBlocked)}};
    SanitizeNavSettings(settings);
    CHECK(settings.profiles[0].radius > 0.0f && settings.profiles[1].name == "A 2");
    CHECK(settings.areas.size() == 2 && settings.areas[0].name == "Default" && settings.areas[1].cost == 1.0f);
}

TEST_CASE(Navigation_PhysicsTemplateWanderer)
{
    // The Physics template: a navmesh over the ground (ramp included), box obstacles, a Nav Agent
    // with Wander.ugraph walking between random reachable points.
    const fs::path root = fs::path(ENGINE_TEST_SOURCE_DIR) / "templates" / "Physics";
    Scene          scene;
    (void)LoadSceneFile(root / "Content" / "Scenes" / "Main.scene.json", scene, nullptr);
    NavigationWorld nav;
    nav.Build(scene);
    CHECK(nav.Stats().tiles > 0 && nav.Stats().obstacles == 3);

    EventBus     events;
    ScriptSystem scripts(events);
    scripts.SetNavigationWorld(&nav);
    nav.SetRandomSeed(3);
    std::error_code ec;
    const fs::path  previous = fs::current_path(ec);
    fs::current_path(root, ec); // script paths are relative to the project root
    scripts.Begin(scene);
    CHECK(scripts.Stats().errors == 0 && scripts.Stats().instances == 3);
    Entity wanderer = NullEntity;
    scene.GetRegistry().ViewOf<NavAgent>().Each([&](Entity e, NavAgent&) { wanderer = e; });
    CHECK(wanderer != NullEntity);
    const glm::vec3 start    = scene.GetRegistry().Get<Transform>(wanderer).position;
    float           traveled = 0.0f;
    glm::vec3       last     = start;
    for (int i = 0; i < 300; ++i) {
        nav.Update(scene, 1.0f / 30.0f);
        scripts.Update(scene, 1.0f / 30.0f);
        const glm::vec3 p = scene.GetRegistry().Get<Transform>(wanderer).position;
        traveled += glm::distance(p, last);
        last = p;
        CHECK(std::abs(p.x) < 10.5f && std::abs(p.z) < 10.5f); // stays on the ground
    }
    scripts.End(scene);
    fs::current_path(previous, ec);
    CHECK(scripts.Stats().errors == 0);
    CHECK(traveled > 5.0f);
}
