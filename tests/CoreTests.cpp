#include "Test.h"

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/GltfLoader.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Renderer/ShadowAtlas.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/AabbTree.h"
#include "Engine/Scene/Frustum.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <atomic>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <latch>
#include <numeric>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <vector>

using namespace Engine;
using namespace std::chrono_literals;

TEST_CASE(ThreadPool_SubmitReturnsResults)
{
    ThreadPool pool(4);
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 100; ++i)
        futures.push_back(pool.Submit([i] { return i * i; }));
    int sum = 0;
    for (auto& f : futures)
        sum += f.get();
    CHECK(sum == 328350); // sum of i^2 for i < 100
}

TEST_CASE(ThreadPool_ExceptionPropagatesThroughFuture)
{
    ThreadPool pool(1);
    auto future = pool.Submit([]() -> int { throw std::runtime_error("boom"); });
    bool caught = false;
    try {
        (void)future.get();
    } catch (const std::runtime_error& e) {
        caught = std::string_view{e.what()} == "boom";
    }
    CHECK(caught);
}

TEST_CASE(ThreadPool_WorkerSurvivesThrowingEnqueue)
{
    ThreadPool pool(1);
    pool.Enqueue([] { throw std::runtime_error("logged, not fatal"); });
    auto after = pool.Submit([] { return 7; });
    CHECK(after.wait_for(5s) == std::future_status::ready);
    CHECK(after.get() == 7);
}

TEST_CASE(ThreadPool_DestructorDrainsQueue)
{
    std::atomic<int> count{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 1000; ++i)
            pool.Enqueue([&count] { count.fetch_add(1, std::memory_order_relaxed); });
    }
    CHECK(count.load() == 1000);
}

TEST_CASE(ThreadPool_JobsMayEnqueueJobs)
{
    std::atomic<int> count{0};
    {
        ThreadPool pool(1);
        pool.Enqueue([&] {
            count.fetch_add(1);
            pool.Enqueue([&] { count.fetch_add(1); }); // runs even though shutdown may have begun
        });
    }
    CHECK(count.load() == 2);
}

TEST_CASE(ThreadPool_RunsJobsConcurrently)
{
    constexpr int kThreads = 4;
    ThreadPool    pool(kThreads);
    std::latch    allStarted{kThreads}; // deadlocks unless all jobs run at the same time
    std::vector<std::future<void>> futures;
    for (int i = 0; i < kThreads; ++i)
        futures.push_back(pool.Submit([&] { allStarted.arrive_and_wait(); }));
    for (auto& f : futures)
        CHECK(f.wait_for(10s) == std::future_status::ready);
}

TEST_CASE(ThreadPool_DefaultThreadCountIsPositive)
{
    CHECK(ThreadPool::DefaultThreadCount() >= 1);
    ThreadPool pool;
    CHECK(pool.ThreadCount() == ThreadPool::DefaultThreadCount());
}

TEST_CASE(AssetHandle_NullEqualityHash)
{
    constexpr ModelHandle null;
    CHECK(!null);
    const ModelHandle a{.index = 3, .generation = 1};
    const ModelHandle b{.index = 3, .generation = 2}; // same slot, reused
    CHECK(a && b);
    CHECK(a != b);
    CHECK(a == (ModelHandle{3, 1}));

    std::unordered_set<ModelHandle> set{a, b, a};
    CHECK(set.size() == 2);
}

namespace {
Frustum TestFrustum() // camera at the origin looking down -Z, 90 degree FOV, reverse-Z infinite far
{
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    return Frustum::FromViewProjection(PerspectiveReverseZ(glm::radians(90.0f), 1.0f, 0.1f) * view);
}

Aabb Box(glm::vec3 center, float halfSize)
{
    return {center - glm::vec3(halfSize), center + glm::vec3(halfSize)};
}
} // namespace

TEST_CASE(Frustum_CullsOutsideKeepsInside)
{
    const Frustum f = TestFrustum();
    CHECK(f.Intersects(Box({0.0f, 0.0f, -5.0f}, 0.5f)));     // straight ahead
    CHECK(!f.Intersects(Box({0.0f, 0.0f, 5.0f}, 0.5f)));     // behind the camera
    CHECK(!f.Intersects(Box({100.0f, 0.0f, -5.0f}, 0.5f)));  // far right
    CHECK(!f.Intersects(Box({0.0f, -100.0f, -5.0f}, 0.5f))); // far below
    CHECK(f.Intersects(Box({0.0f, 0.0f, -1.0e6f}, 1.0f)));   // infinite far plane
    CHECK(f.Intersects(Box({0.0f, 0.0f, 0.0f}, 0.5f)));      // straddles the near plane
    CHECK(f.Intersects(Box({5.4f, 0.0f, -5.0f}, 0.5f)));     // pokes into the right edge (x <= -z)
    CHECK(!f.Intersects(Box({6.1f, 0.0f, -5.0f}, 0.5f)));    // just outside it (nearest corner x=5.6 > -z=5.5)
}

TEST_CASE(Frustum_TransformAabb)
{
    const Aabb unit = Box(glm::vec3(0.0f), 0.5f);

    const Aabb moved = TransformAabb(unit, glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f)));
    CHECK(glm::all(glm::epsilonEqual(moved.min, glm::vec3(0.5f, 1.5f, 2.5f), 1e-5f)));
    CHECK(glm::all(glm::epsilonEqual(moved.max, glm::vec3(1.5f, 2.5f, 3.5f), 1e-5f)));

    // 45 degrees around Y: x/z extents grow to sqrt(2)/2, y unchanged.
    const Aabb rotated = TransformAabb(unit, glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0, 1, 0)));
    const float h = std::sqrt(2.0f) * 0.5f;
    CHECK(glm::all(glm::epsilonEqual(rotated.max, glm::vec3(h, 0.5f, h), 1e-5f)));

    // Mirroring must not produce an inverted box.
    const Aabb mirrored = TransformAabb(unit, glm::scale(glm::mat4(1.0f), glm::vec3(-2.0f, 1.0f, 1.0f)));
    CHECK(mirrored.min.x == -1.0f && mirrored.max.x == 1.0f);
}

TEST_CASE(Cascades_SplitDistribution)
{
    const auto uniform = ComputeCascadeSplits(1.0f, 101.0f, 4, 0.0f);
    CHECK(std::abs(uniform[0] - 26.0f) < 1e-3f && std::abs(uniform[1] - 51.0f) < 1e-3f);
    const auto logarithmic = ComputeCascadeSplits(1.0f, 10000.0f, 4, 1.0f);
    CHECK(std::abs(logarithmic[0] - 10.0f) < 1e-2f && std::abs(logarithmic[2] - 1000.0f) < 1.0f);

    const auto practical = ComputeCascadeSplits(0.1f, 60.0f, 4, 0.8f);
    CHECK(practical[0] > 0.1f && practical[0] < practical[1] && practical[1] < practical[2]);
    CHECK(practical[3] == 60.0f);
    CHECK(ComputeCascadeSplits(0.1f, 60.0f, 2, 0.5f)[1] == 60.0f);
}

TEST_CASE(Cascades_OrthoReverseZ)
{
    const glm::mat4 p    = OrthoReverseZ(-2.0f, 2.0f, -1.0f, 1.0f, 0.5f, 10.0f);
    const glm::vec4 near = p * glm::vec4(2.0f, -1.0f, -0.5f, 1.0f);
    const glm::vec4 far  = p * glm::vec4(-2.0f, 1.0f, -10.0f, 1.0f);
    CHECK(std::abs(near.x - 1.0f) < 1e-6f && std::abs(near.y + 1.0f) < 1e-6f && std::abs(near.z - 1.0f) < 1e-6f);
    CHECK(std::abs(far.x + 1.0f) < 1e-6f && std::abs(far.y - 1.0f) < 1e-6f && std::abs(far.z) < 1e-6f);
}

namespace {
CameraData TestCamera(glm::vec3 position, float yaw)
{
    const glm::vec3 forward{std::sin(yaw), -0.2f, -std::cos(yaw)};
    return {.view       = glm::lookAt(position, position + forward, glm::vec3(0.0f, 1.0f, 0.0f)),
            .projection = PerspectiveReverseZ(glm::radians(60.0f), 16.0f / 9.0f, 0.1f),
            .position   = position,
            .nearPlane  = 0.1f};
}
} // namespace

TEST_CASE(Cascades_CoverTheirFrustumSlice)
{
    const ShadowSettings settings{.resolution = 2048, .maxDistance = 80.0f};
    const glm::vec3      light{-0.4f, -0.8f, -0.3f};
    const CameraData     camera   = TestCamera({3.0f, 2.0f, 5.0f}, 0.7f);
    const auto           cascades = ComputeCascades(camera, light, settings);

    // Every point of every slice must land inside its cascade (xy in [-1, 1], depth in [0, 1]).
    const glm::mat4 world = glm::inverse(camera.view);
    const float     tanX  = 1.0f / camera.projection[0][0];
    const float     tanY  = 1.0f / camera.projection[1][1];
    float           near  = camera.nearPlane;
    for (std::uint32_t c = 0; c < settings.cascadeCount; ++c) {
        for (const float d : {near, cascades[c].splitFar})
            for (const float sx : {-1.0f, 1.0f})
                for (const float sy : {-1.0f, 1.0f}) {
                    const glm::vec4 p = world * glm::vec4(sx * d * tanX, sy * d * tanY, -d, 1.0f);
                    const glm::vec4 q = cascades[c].viewProj * p;
                    CHECK(std::abs(q.x) <= 1.0f && std::abs(q.y) <= 1.0f && q.z >= 0.0f && q.z <= 1.0f);
                }
        near = cascades[c].splitFar;
    }
}

TEST_CASE(Cascades_StableUnderCameraMotion)
{
    const ShadowSettings settings{.resolution = 2048, .maxDistance = 80.0f};
    const glm::vec3      light{-0.4f, -0.8f, -0.3f};

    // Rotation only: same slice sizes -> identical projection scale in every cascade.
    const auto a = ComputeCascades(TestCamera({0.0f, 2.0f, 0.0f}, 0.1f), light, settings);
    const auto b = ComputeCascades(TestCamera({0.0f, 2.0f, 0.0f}, 2.3f), light, settings);
    for (std::uint32_t c = 0; c < 4; ++c)
        CHECK(a[c].viewProj[0][0] == b[c].viewProj[0][0] && a[c].texelWorldSize == b[c].texelWorldSize);

    // Small translation: a fixed world point moves by whole shadow texels only (no shimmering).
    const auto      moved = ComputeCascades(TestCamera({0.013f, 2.0f, 0.021f}, 0.1f), light, settings);
    const glm::vec4 point{1.0f, 0.0f, -4.0f, 1.0f};
    for (std::uint32_t c = 0; c < 4; ++c) {
        const glm::vec2 ta = glm::vec2(a[c].viewProj * point) * (0.5f * settings.resolution);
        const glm::vec2 tb = glm::vec2(moved[c].viewProj * point) * (0.5f * settings.resolution);
        const glm::vec2 d  = ta - tb;
        CHECK(std::abs(d.x - std::round(d.x)) < 1e-2f && std::abs(d.y - std::round(d.y)) < 1e-2f);
    }
}

int main(int argc, char** argv)
{
    return Test::RunAll(argc > 1 ? argv[1] : "");
}

TEST_CASE(Gltf_LightsPunctual)
{
    // Point + spot are imported per node, directional lights are skipped.
    const auto path = std::filesystem::temp_directory_path() / "engine_lights_test.gltf";
    {
        std::ofstream file(path);
        file << R"({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["KHR_lights_punctual"],
  "extensions": {"KHR_lights_punctual": {"lights": [
    {"type": "point", "color": [1.0, 0.5, 0.25], "intensity": 20.0, "range": 5.0},
    {"type": "spot", "intensity": 100.0, "spot": {"innerConeAngle": 0.2, "outerConeAngle": 0.5}},
    {"type": "directional"}]}},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "Root", "children": [1, 2, 3]},
    {"name": "Lamp", "translation": [1.0, 2.0, 3.0], "extensions": {"KHR_lights_punctual": {"light": 0}}},
    {"name": "Spot", "extensions": {"KHR_lights_punctual": {"light": 1}}},
    {"name": "Sun", "extensions": {"KHR_lights_punctual": {"light": 2}}}]
})";
    }
    const ModelData data = LoadGltf(path);
    std::filesystem::remove(path);

    CHECK(data.nodes.size() == 4);
    const auto find = [&](const char* name) -> const ModelNode* {
        for (const ModelNode& n : data.nodes)
            if (n.name == name)
                return &n;
        return nullptr;
    };
    const ModelNode* lamp = find("Lamp");
    const ModelNode* spot = find("Spot");
    const ModelNode* sun  = find("Sun");
    CHECK(lamp && spot && sun);
    if (!lamp || !spot || !sun)
        return;
    CHECK(lamp->light && lamp->light->type == LightType::Point);
    CHECK(lamp->light && lamp->light->intensity == 20.0f && lamp->light->range == 5.0f);
    CHECK(lamp->light && lamp->light->color == glm::vec3(1.0f, 0.5f, 0.25f));
    CHECK(lamp->local.position == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(spot->light && spot->light->type == LightType::Spot && spot->light->range == 0.0f);
    CHECK(spot->light && std::abs(spot->light->innerConeAngle - 0.2f) < 1e-6f &&
          std::abs(spot->light->outerConeAngle - 0.5f) < 1e-6f);
    CHECK(!sun->light);

    // Unbounded lights get a finite range where the illuminance falls below the cutoff.
    const float range = EffectiveRange(*spot->light);
    CHECK(std::abs(100.0f / (range * range) - kLightCutoffIlluminance) < 1e-4f);
    CHECK(EffectiveRange(*lamp->light) == 5.0f);
}

TEST_CASE(ShadowAtlas_PackingIsTightAndDisjoint)
{
    std::vector<std::uint32_t> sizes{1024};
    sizes.insert(sizes.end(), 6, 512);
    sizes.insert(sizes.end(), 10, 256);
    sizes.insert(sizes.end(), 4, 128);
    const auto packed = PackShadowTiles(sizes, 4096, 128);
    CHECK(packed.has_value());
    if (!packed)
        return;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const glm::uvec2 a = (*packed)[i];
        CHECK(a.x % sizes[i] == 0 && a.y % sizes[i] == 0); // aligned to its own size
        CHECK(a.x + sizes[i] <= 4096 && a.y + sizes[i] <= 4096);
        for (std::size_t j = i + 1; j < sizes.size(); ++j) {
            const glm::uvec2 b       = (*packed)[j];
            const bool       overlap = a.x < b.x + sizes[j] && b.x < a.x + sizes[i] && a.y < b.y + sizes[j] &&
                                 b.y < a.y + sizes[i];
            CHECK(!overlap);
        }
    }
    // Exactly full, then one tile too many.
    const std::vector<std::uint32_t> full(16, 1024);
    CHECK(PackShadowTiles(full, 4096, 128).has_value());
    const std::vector<std::uint32_t> over(17, 1024);
    CHECK(!PackShadowTiles(over, 4096, 128).has_value());
}

TEST_CASE(ShadowAtlas_CubeFacesAndBorder)
{
    // Face order +X, -X, +Y, -Y, +Z, -Z: the face axis maps to the view direction (-Z).
    const glm::vec3 light{1.0f, 2.0f, 3.0f};
    const glm::vec3 axes[kCubeFaces] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (std::uint32_t f = 0; f < kCubeFaces; ++f) {
        const glm::vec3 v = glm::vec3(CubeFaceView(light, f) * glm::vec4(light + axes[f] * 2.0f, 1.0f));
        CHECK(glm::all(glm::epsilonEqual(v, glm::vec3(0.0f, 0.0f, -2.0f), 1e-5f)));
    }
    // 90 degrees plus a 4-texel border on a 256 tile: the face edge lands 4 texels inside.
    const float t = ShadowTanHalfWithBorder(1.0f, 256, 4.0f);
    CHECK(std::abs(1.0f / t - (1.0f - 8.0f / 256.0f)) < 1e-6f);
}

TEST_CASE(Scene_UuidsAndSiblingOrder)
{
    Scene        scene;
    const Entity root = scene.CreateEntity("Root");
    const Entity a    = scene.CreateEntity("A", root);
    const Entity b    = scene.CreateEntity("B", root);
    const Entity c    = scene.CreateEntity("C", root, 42);
    Registry&    r    = scene.GetRegistry();
    CHECK(r.Get<Uuid>(c).value == 42 && scene.FindByUuid(42) == c);
    CHECK(r.Get<Uuid>(a).value != 0 && r.Get<Uuid>(a).value != r.Get<Uuid>(b).value);
    CHECK(scene.CreateEntity("Dup", NullEntity, 42) != c); // taken: gets a fresh one
    CHECK(scene.FindByUuid(42) == c);

    CHECK(scene.SiblingIndex(a) == 0 && scene.SiblingIndex(b) == 1 && scene.SiblingIndex(c) == 2);
    scene.SetParent(c, root, 0);
    CHECK(scene.SiblingIndex(c) == 0 && scene.SiblingIndex(a) == 1);
    CHECK(scene.IsAncestor(root, c) && !scene.IsAncestor(c, root));

    scene.DestroyEntity(root);
    CHECK(scene.FindByUuid(42) == NullEntity && !r.Valid(a));
    scene.Clear();
    CHECK(r.AliveCount() == 0);
}

TEST_CASE(SceneSerializer_SnapshotRestoreAndState)
{
    Scene        scene;
    Registry&    r      = scene.GetRegistry();
    const Entity parent = scene.CreateEntity("Parent");
    scene.CreateEntity("First", parent);
    const Entity node = scene.CreateEntity("Node", parent);
    const Entity child = scene.CreateEntity("Child", node);
    scene.EditTransform(node).position = glm::vec3(1.0f, 2.0f, 3.0f);
    scene.EditTransform(node).rotation = glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
    r.Emplace<Light>(child, Light{.type = LightType::Spot, .intensity = 7.0f, .castShadows = false});
    r.Emplace<MeshRenderer>(node, MeshRenderer{.model = ModelHandle{3, 9}, .meshIndex = 2});
    const std::uint64_t nodeUuid  = r.Get<Uuid>(node).value;
    const std::uint64_t childUuid = r.Get<Uuid>(child).value;

    // Delete + undo: same UUIDs, same place, same components.
    const Entity      roots[] = {node};
    const std::string snapshot = SnapshotEntities(scene, roots);
    scene.DestroyEntity(node);
    CHECK(scene.FindByUuid(nodeUuid) == NullEntity);
    const auto restored = RestoreEntities(scene, snapshot, RestoreMode::Original);
    CHECK(restored.size() == 1);
    const Entity node2  = scene.FindByUuid(nodeUuid);
    const Entity child2 = scene.FindByUuid(childUuid);
    CHECK(node2 != NullEntity && child2 != NullEntity && restored[0] == node2);
    CHECK(r.Get<Hierarchy>(node2).parent == parent && scene.SiblingIndex(node2) == 1);
    CHECK(r.Get<Hierarchy>(child2).parent == node2);
    CHECK(r.Get<Name>(node2).value == "Node");
    CHECK(r.Get<Transform>(node2).position == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(std::abs(glm::dot(r.Get<Transform>(node2).rotation,
                            glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f))) - 1.0f) < 1e-5f);
    CHECK(r.Has<MeshRenderer>(node2) && r.Get<MeshRenderer>(node2).model == (ModelHandle{3, 9}) &&
          r.Get<MeshRenderer>(node2).meshIndex == 2);
    CHECK(r.Has<Light>(child2) && r.Get<Light>(child2).type == LightType::Spot &&
          r.Get<Light>(child2).intensity == 7.0f && !r.Get<Light>(child2).castShadows);

    // Duplicate: fresh UUIDs, appended to the same parent, subtree copied.
    const Entity dupRoots[] = {node2};
    const auto   dup = RestoreEntities(scene, SnapshotEntities(scene, dupRoots), RestoreMode::Duplicate);
    CHECK(dup.size() == 1 && dup[0] != node2 && r.Get<Uuid>(dup[0]).value != nodeUuid);
    CHECK(r.Get<Hierarchy>(dup[0]).parent == parent && scene.SiblingIndex(dup[0]) == 2);
    CHECK(r.Get<Hierarchy>(dup[0]).children.size() == 1 && r.Has<Light>(r.Get<Hierarchy>(dup[0]).children[0]));

    // Entity state: components only, absent ones are removed again.
    const std::string state = SnapshotEntityState(scene, node2);
    scene.EditTransform(node2).scale = glm::vec3(5.0f);
    r.Get<Name>(node2).value      = "Renamed";
    r.Remove<MeshRenderer>(node2);
    r.Emplace<Light>(node2);
    ApplyEntityState(scene, node2, state);
    CHECK(r.Get<Transform>(node2).scale == glm::vec3(1.0f) && r.Get<Name>(node2).value == "Node");
    CHECK(r.Has<MeshRenderer>(node2) && !r.Has<Light>(node2));
}

TEST_CASE(Scene_DirtyTransformsAndChanges)
{
    Scene     scene;
    Registry& r = scene.GetRegistry();
    // Two chains of 3 + a lone root.
    const Entity a  = scene.CreateEntity("A");
    const Entity a1 = scene.CreateEntity("A1", a);
    const Entity a2 = scene.CreateEntity("A2", a1);
    const Entity b  = scene.CreateEntity("B");
    const Entity b1 = scene.CreateEntity("B1", b);
    const Entity c  = scene.CreateEntity("C");
    scene.UpdateTransforms();
    CHECK(scene.CountStaleTransforms() == 0);
    SceneChanges first = scene.TakeChanges();
    CHECK(first.changed.size() == 6 && first.destroyed.empty() && !first.overflow);

    // Nothing dirty: nothing recomputed, nothing reported.
    scene.UpdateTransforms();
    CHECK(scene.LastTransformUpdate().updated == 0 && scene.TakeChanges().changed.empty());

    // Editing a child and its ancestor: one subtree, recomputed once.
    scene.EditTransform(a1).position = glm::vec3(1.0f, 0.0f, 0.0f);
    scene.EditTransform(a).position  = glm::vec3(0.0f, 2.0f, 0.0f);
    scene.UpdateTransforms();
    CHECK(scene.LastTransformUpdate().dirtyRoots == 1 && scene.LastTransformUpdate().updated == 3);
    CHECK(glm::vec3(r.Get<WorldTransform>(a2).matrix[3]) == glm::vec3(1.0f, 2.0f, 0.0f));
    CHECK(scene.CountStaleTransforms() == 0);
    SceneChanges changes = scene.TakeChanges();
    CHECK(changes.changed.size() == 3 && std::ranges::find(changes.changed, b) == changes.changed.end());

    // Reparenting marks the moved subtree; the world transform follows the new parent.
    scene.SetParent(b, a2);
    scene.UpdateTransforms();
    CHECK(scene.LastTransformUpdate().updated == 2 && scene.CountStaleTransforms() == 0);
    CHECK(glm::vec3(r.Get<WorldTransform>(b1).matrix[3]) == glm::vec3(1.0f, 2.0f, 0.0f));

    // MarkChanged reports without recomputing; destruction is reported with the old handles.
    (void)scene.TakeChanges();
    scene.MarkChanged(c);
    scene.MarkChanged(c);
    scene.DestroyEntity(a1); // with a2, b, b1
    changes = scene.TakeChanges();
    CHECK(changes.changed.size() == 1 && changes.changed[0] == c && changes.destroyed.size() == 4);

    // A destroyed dirty entity must not leave its flag to the slot's next owner.
    scene.EditTransform(c).position = glm::vec3(5.0f);
    scene.DestroyEntity(c);
    const Entity reused = scene.CreateEntity("Reused");
    scene.EditTransform(reused).position = glm::vec3(3.0f);
    scene.UpdateTransforms();
    CHECK(glm::vec3(r.Get<WorldTransform>(reused).matrix[3]) == glm::vec3(3.0f) && scene.CountStaleTransforms() == 0);
}

TEST_CASE(AabbTree_MatchesBruteForce)
{
    std::mt19937                          rng(1234);
    std::uniform_real_distribution<float> pos(-50.0f, 50.0f), ext(0.1f, 3.0f);
    const auto randomBox = [&] {
        const glm::vec3 c{pos(rng), pos(rng), pos(rng)};
        const glm::vec3 e{ext(rng), ext(rng), ext(rng)};
        return Aabb{c - e, c + e};
    };
    const auto overlaps = [](const Aabb& a, const Aabb& b) {
        return glm::all(glm::lessThanEqual(a.min, b.max)) && glm::all(glm::lessThanEqual(b.min, a.max));
    };

    AabbTree                  tree;
    std::vector<Aabb>         boxes;   // tight bounds by user data
    std::vector<std::int32_t> proxies; // -1: removed
    for (std::uint32_t i = 0; i < 2000; ++i) {
        boxes.push_back(randomBox());
        proxies.push_back(tree.Insert(boxes.back(), i));
    }
    // Moves (small and large) and removals.
    std::uniform_int_distribution<std::uint32_t> pick(0, 1999);
    for (int step = 0; step < 3000; ++step) {
        const std::uint32_t i = pick(rng);
        if (proxies[i] == AabbTree::kNull)
            continue;
        if (step % 7 == 0) {
            tree.Remove(proxies[i]);
            proxies[i] = AabbTree::kNull;
        } else {
            const glm::vec3 delta = step % 2 ? glm::vec3(0.01f) : glm::vec3(pos(rng) * 0.2f);
            boxes[i]              = {boxes[i].min + delta, boxes[i].max + delta};
            tree.Move(proxies[i], boxes[i]);
        }
    }
    CHECK(tree.Validate());
    const std::size_t alive = static_cast<std::size_t>(std::ranges::count_if(proxies, [](std::int32_t p) { return p != AabbTree::kNull; }));
    CHECK(tree.LeafCount() == alive);
    CHECK(tree.Height() <= 3 * static_cast<int>(std::log2(static_cast<float>(alive))) + 2); // balanced

    // Box queries: every overlapping tight box is reported (fat boxes may add extra candidates).
    for (int q = 0; q < 50; ++q) {
        const Aabb                 query = randomBox();
        std::vector<std::uint32_t> found;
        tree.Query(query, [&](std::int32_t proxy) { found.push_back(tree.UserData(proxy)); });
        for (std::uint32_t i = 0; i < boxes.size(); ++i)
            if (proxies[i] != AabbTree::kNull && overlaps(boxes[i], query))
                CHECK(std::ranges::find(found, i) != found.end());
    }

    // Raycast: nearest tight-box hit equals brute force.
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    for (int q = 0; q < 50; ++q) {
        const glm::vec3 origin{pos(rng), pos(rng), pos(rng)};
        const glm::vec3 dir = glm::normalize(glm::vec3(unit(rng), unit(rng), unit(rng)) + glm::vec3(1e-3f));
        const glm::vec3 inv = 1.0f / dir;
        float           brute = -1.0f;
        for (std::uint32_t i = 0; i < boxes.size(); ++i) {
            if (proxies[i] == AabbTree::kNull)
                continue;
            const float t = AabbTree::RayBox(origin, inv, boxes[i], 1e9f);
            if (t >= 0.0f && (brute < 0.0f || t < brute))
                brute = t;
        }
        float best = -1.0f;
        tree.Raycast(origin, dir, 1e9f, [&](std::int32_t proxy, float maxT) {
            const float t = AabbTree::RayBox(origin, inv, boxes[tree.UserData(proxy)], maxT);
            if (t < 0.0f)
                return maxT;
            best = t;
            return t;
        });
        CHECK(std::abs(best - brute) < 1e-4f);
    }

    // Remove everything: empty and valid.
    for (std::int32_t& p : proxies)
        if (p != AabbTree::kNull) {
            tree.Remove(p);
            p = AabbTree::kNull;
        }
    CHECK(tree.Validate() && tree.LeafCount() == 0 && tree.Height() == -1);
}

TEST_CASE(ShadowAtlas_BuddyAllocatorReusesAndMerges)
{
    ShadowTileAllocator alloc;
    alloc.Reset(4096, 128);
    CHECK(alloc.FreeArea() == 4096ull * 4096ull);

    struct Tile {
        glm::uvec2    offset;
        std::uint32_t size;
    };
    std::vector<Tile>   tiles;
    const std::uint32_t sizes[] = {1024, 128, 512, 256, 128, 1024, 512, 128, 256, 2048};
    for (std::uint32_t size : sizes) {
        const auto offset = alloc.Allocate(size);
        CHECK(offset.has_value());
        if (offset)
            tiles.push_back({*offset, size});
    }
    const auto disjoint = [&] {
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const Tile& a = tiles[i];
            if (a.offset.x % a.size != 0 || a.offset.y % a.size != 0 || a.offset.x + a.size > 4096 ||
                a.offset.y + a.size > 4096)
                return false;
            for (std::size_t j = i + 1; j < tiles.size(); ++j) {
                const Tile& b = tiles[j];
                if (a.offset.x < b.offset.x + b.size && b.offset.x < a.offset.x + a.size &&
                    a.offset.y < b.offset.y + b.size && b.offset.y < a.offset.y + a.size)
                    return false;
            }
        }
        return true;
    };
    CHECK(disjoint());
    std::uint64_t used = 0;
    for (const Tile& t : tiles)
        used += std::uint64_t{t.size} * t.size;
    CHECK(alloc.FreeArea() == 4096ull * 4096ull - used);

    // Freeing a tile and allocating the same size reuses space; the others keep their place.
    const Tile freed = tiles[1];
    alloc.Free(freed.offset, freed.size);
    tiles.erase(tiles.begin() + 1);
    const auto again = alloc.Allocate(128);
    CHECK(again.has_value());
    if (again)
        tiles.push_back({*again, 128});
    CHECK(disjoint());

    // Free everything: blocks merge back into the whole atlas.
    for (const Tile& t : tiles)
        alloc.Free(t.offset, t.size);
    CHECK(alloc.FreeArea() == 4096ull * 4096ull);
    const auto whole = alloc.Allocate(4096);
    CHECK(whole.has_value() && *whole == glm::uvec2(0));
    CHECK(!alloc.Allocate(128).has_value()); // full
}

// --- Physics ---------------------------------------------------------------------------------

namespace {

struct PhysicsFixture {
    ThreadPool                  pool{2};
    EventBus                    bus;
    Scene                       scene;
    PhysicsWorld                physics{pool, bus};
    std::vector<CollisionEvent> events;
    Subscription                sub = bus.Subscribe<CollisionEvent>([this](const CollisionEvent& e) { events.push_back(e); });

    Entity Body(const char* name, glm::vec3 position, BodyType type, Collider collider, Entity parent = NullEntity)
    {
        const Entity e = scene.CreateEntity(name, parent);
        scene.EditTransform(e).position = position;
        scene.GetRegistry().Emplace<RigidBody>(e, RigidBody{.type = type});
        scene.GetRegistry().Emplace<Collider>(e, collider);
        return e;
    }
    Entity Ground()
    {
        Collider box;
        box.halfExtents = {20.0f, 0.5f, 20.0f};
        return Body("Ground", {0.0f, -0.5f, 0.0f}, BodyType::Static, box);
    }
    static Collider Sphere(float radius)
    {
        Collider c;
        c.shape  = ColliderShape::Sphere;
        c.radius = radius;
        return c;
    }
    void Run(float seconds)
    {
        for (int i = 0, n = static_cast<int>(seconds * 60.0f); i < n; ++i)
            physics.Step(scene, 1.0f / 60.0f);
    }
    glm::vec3 WorldPosition(Entity e) const { return scene.GetRegistry().Get<WorldTransform>(e).matrix[3]; }
    std::size_t Count(bool begin, bool trigger) const
    {
        return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [&](const CollisionEvent& e) {
            return e.begin == begin && e.trigger == trigger;
        }));
    }
};

bool Near(const glm::vec3& a, const glm::vec3& b, float eps) { return glm::all(glm::epsilonEqual(a, b, eps)); }

} // namespace

TEST_CASE(Physics_FallRestSleepAndEvents)
{
    PhysicsFixture f;
    const Entity   ground = f.Ground();
    const Entity   ball   = f.Body("Ball", {0.0f, 5.0f, 0.0f}, BodyType::Dynamic, PhysicsFixture::Sphere(0.5f));
    f.physics.Sync(f.scene);
    CHECK(f.physics.Stats().bodies == 2);
    CHECK(f.physics.Activity(ground) == BodyActivity::Static);
    CHECK(f.physics.Activity(ball) == BodyActivity::Active);

    f.Run(4.0f);
    CHECK(std::abs(f.WorldPosition(ball).y - 0.5f) < 0.03f); // Jolt penetration slop 2 cm
    CHECK(f.scene.CountStaleTransforms() == 0);
    CHECK(f.physics.Activity(ball) == BodyActivity::Sleeping);
    // One Begin, no End although Jolt drops the contacts of sleeping bodies.
    CHECK(f.events.size() == 1);
    if (!f.events.empty()) {
        const CollisionEvent& e = f.events[0];
        CHECK(e.begin && !e.trigger);
        CHECK((e.a == ground && e.b == ball) || (e.a == ball && e.b == ground));
    }
    CHECK(f.physics.Stats().contactPairs == 1);

    // Kicked upwards: wakes up, leaves the ground (End), lands again (Begin).
    f.physics.AddImpulse(ball, {0.0f, 5.0f, 0.0f});
    CHECK(f.physics.Activity(ball) == BodyActivity::Active);
    f.Run(0.3f);
    CHECK(f.WorldPosition(ball).y > 0.8f);
    CHECK(f.Count(false, false) == 1);
    f.Run(3.0f);
    CHECK(f.Count(true, false) == 2);
    CHECK(std::abs(f.WorldPosition(ball).y - 0.5f) < 0.03f); // Jolt penetration slop 2 cm

    // Destroying a touching body ends its contact.
    f.scene.DestroyEntity(ball);
    f.physics.Sync(f.scene);
    CHECK(f.Count(false, false) == 2);
    CHECK(f.physics.Stats().bodies == 1 && f.physics.Stats().contactPairs == 0);
}

TEST_CASE(Physics_QueriesTeleportAndKinematic)
{
    PhysicsFixture f;
    const Entity   ground = f.Ground();
    const Entity   ball   = f.Body("Ball", {0.0f, 3.0f, 0.0f}, BodyType::Static, PhysicsFixture::Sphere(1.0f));
    f.physics.Sync(f.scene);

    auto hit = f.physics.Raycast({0.0f, 10.0f, 0.0f}, {0.0f, -2.0f, 0.0f}, 100.0f);
    CHECK(hit && hit->entity == ball && std::abs(hit->distance - 6.0f) < 1e-3f);
    CHECK(hit && Near(hit->normal, {0.0f, 1.0f, 0.0f}, 1e-3f) && Near(hit->point, {0.0f, 4.0f, 0.0f}, 1e-3f));
    hit = f.physics.Raycast({0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f, ball); // ignored
    CHECK(hit && hit->entity == ground && std::abs(hit->distance - 10.0f) < 1e-3f);
    CHECK(!f.physics.Raycast({0.0f, 10.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 100.0f));
    CHECK(!f.physics.Raycast({0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 5.0f)); // too short

    const auto cast = f.physics.SphereCast({3.0f, 3.0f, 0.0f}, 0.5f, {-1.0f, 0.0f, 0.0f}, 10.0f);
    CHECK(cast && cast->entity == ball && std::abs(cast->distance - 1.5f) < 1e-2f);
    CHECK(cast && Near(cast->normal, {1.0f, 0.0f, 0.0f}, 1e-2f) && Near(cast->point, {1.0f, 3.0f, 0.0f}, 1e-2f));

    // Moving a static body by hand teleports it.
    f.scene.EditTransform(ball).position = {5.0f, 3.0f, 0.0f};
    f.physics.Sync(f.scene);
    CHECK(!f.physics.Raycast({0.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f, ground));
    hit = f.physics.Raycast({5.0f, 10.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
    CHECK(hit && hit->entity == ball);

    // A kinematic box moved during simulation pushes a dynamic box out of its way.
    Collider box;
    box.halfExtents = glm::vec3(0.5f);
    const Entity pusher = f.Body("Pusher", {-3.0f, 0.5f, 0.0f}, BodyType::Kinematic, box);
    const Entity crate  = f.Body("Crate", {0.0f, 0.5f, 0.0f}, BodyType::Dynamic, box);
    f.Run(0.5f);
    for (int i = 0; i < 120; ++i) { // 2 m/s along +X for 2 s
        f.scene.EditTransform(pusher).position.x += 2.0f / 60.0f;
        f.physics.Step(f.scene, 1.0f / 60.0f);
    }
    CHECK(std::abs(f.WorldPosition(pusher).x - 1.0f) < 1e-3f);
    CHECK(f.WorldPosition(crate).x > 1.5f);
    CHECK(f.physics.Activity(pusher) == BodyActivity::Kinematic);

    // Removing the collider removes the body.
    f.scene.GetRegistry().Remove<Collider>(crate);
    f.physics.Sync(f.scene);
    CHECK(!f.physics.HasBody(crate) && f.physics.HasBody(pusher));
    f.physics.Reset();
    CHECK(f.physics.Stats().bodies == 0);
    f.physics.Sync(f.scene);
    CHECK(f.physics.Stats().bodies == 3);
}

TEST_CASE(Physics_TriggerScaleAndHierarchy)
{
    PhysicsFixture f;
    f.Ground();
    Collider zone;
    zone.halfExtents = {2.0f, 0.5f, 2.0f};
    zone.trigger     = true;
    const Entity trigger = f.Body("Zone", {0.0f, 3.0f, 0.0f}, BodyType::Static, zone);
    const Entity ball    = f.Body("Ball", {0.0f, 6.0f, 0.0f}, BodyType::Dynamic, PhysicsFixture::Sphere(0.25f));
    f.Run(3.0f);
    CHECK(std::abs(f.WorldPosition(ball).y - 0.25f) < 0.02f); // fell through the trigger
    CHECK(f.Count(true, true) == 1 && f.Count(false, true) == 1);
    CHECK(f.Count(true, false) == 1); // ground
    // Queries ignore triggers.
    const auto hit = f.physics.Raycast({1.0f, 10.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
    CHECK(hit && hit->entity != trigger);

    // World scale scales the shape (box per axis, sphere by the largest axis).
    Collider unit;
    unit.halfExtents = glm::vec3(0.5f);
    const Entity parent = f.scene.CreateEntity("Parent");
    f.scene.EditTransform(parent).position = {10.0f, 0.0f, 0.0f};
    f.scene.EditTransform(parent).scale    = glm::vec3(2.0f);
    const Entity box = f.Body("Box", {0.0f, 3.0f, 0.0f}, BodyType::Static, unit, parent); // world (10, 6, 0), 2x2x2
    f.physics.Sync(f.scene);
    auto top = f.physics.Raycast({10.0f, 20.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
    CHECK(top && top->entity == box && std::abs(top->point.y - 7.0f) < 1e-3f);
    f.scene.EditTransform(box).scale = {1.0f, 2.0f, 1.0f}; // now 2x4x2
    f.physics.Sync(f.scene);
    top = f.physics.Raycast({10.0f, 20.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 100.0f);
    CHECK(top && std::abs(top->point.y - 8.0f) < 1e-3f);

    // A dynamic child of a rotated, translated parent: the local transform is written so that the
    // world transform matches the body; no stale transforms, no teleport feedback.
    const Entity holder = f.scene.CreateEntity("Holder");
    f.scene.EditTransform(holder).position = {-10.0f, 1.0f, 0.0f};
    f.scene.EditTransform(holder).rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const Entity child = f.Body("Child", {0.0f, 4.0f, 2.0f}, BodyType::Dynamic, PhysicsFixture::Sphere(0.5f), holder);
    f.Run(3.0f);
    CHECK(f.scene.CountStaleTransforms() == 0);
    const glm::vec3 world = f.WorldPosition(child);
    CHECK(Near(world, {-8.0f, 0.5f, 0.0f}, 0.03f)); // local +Z rotated by 90 deg about Y = world +X
    const auto below = f.physics.Raycast(world + glm::vec3(0.0f, 5.0f, 0.0f), {0.0f, -1.0f, 0.0f}, 10.0f);
    CHECK(below && below->entity == child);
}

TEST_CASE(Physics_CharacterWalksClimbsAndJumps)
{
    PhysicsFixture f;
    f.Ground();
    Collider stepBox;
    stepBox.halfExtents = {1.0f, 0.1f, 2.0f}; // 20 cm step at x = 2..4
    f.Body("Step", {3.0f, 0.1f, 0.0f}, BodyType::Static, stepBox);
    Collider wall;
    wall.halfExtents = {0.2f, 2.0f, 2.0f};
    f.Body("Wall", {7.0f, 2.0f, 0.0f}, BodyType::Static, wall);

    const Entity player = f.scene.CreateEntity("Player");
    f.scene.EditTransform(player).position = {0.0f, 0.05f, 0.0f};
    f.scene.GetRegistry().Emplace<CharacterController>(player);
    f.Run(0.5f);
    auto state = f.physics.GetCharacterState(player);
    CHECK(state && state->onGround);
    CHECK(f.physics.Activity(player) == BodyActivity::Character);
    CHECK(std::abs(f.WorldPosition(player).y) < 0.05f);

    // Walk onto the step.
    f.physics.SetCharacterInput(player, {2.0f, 0.0f, 0.0f}, false);
    f.Run(1.5f);
    CHECK(f.WorldPosition(player).x > 2.5f);
    CHECK(std::abs(f.WorldPosition(player).y - 0.2f) < 0.05f);

    // Keep walking: stops at the wall (radius 0.3 + wall half width 0.2).
    f.Run(3.0f);
    CHECK(f.WorldPosition(player).x < 6.55f && f.WorldPosition(player).x > 6.3f);

    // Jump: leaves the ground, lands again.
    f.physics.SetCharacterInput(player, glm::vec3(0.0f), true);
    f.Run(0.25f);
    state = f.physics.GetCharacterState(player);
    CHECK(state && !state->onGround && f.WorldPosition(player).y > 0.5f);
    f.Run(2.0f);
    state = f.physics.GetCharacterState(player);
    CHECK(state && state->onGround && std::abs(f.WorldPosition(player).y) < 0.05f);

    // Teleport by hand, then removal.
    f.scene.EditTransform(player).position = {-5.0f, 1.0f, 0.0f};
    f.Run(1.0f);
    CHECK(Near(f.WorldPosition(player), {-5.0f, 0.0f, 0.0f}, 0.05f));
    f.scene.GetRegistry().Remove<CharacterController>(player);
    f.physics.Sync(f.scene);
    CHECK(!f.physics.HasBody(player) && f.physics.Stats().characters == 0);
}

TEST_CASE(SceneSerializer_PhysicsComponents)
{
    Scene        scene;
    const Entity e = scene.CreateEntity("Body");
    Registry&    r = scene.GetRegistry();
    r.Emplace<RigidBody>(e, RigidBody{.type = BodyType::Kinematic, .mass = 3.0f, .linearDamping = 0.2f,
                                      .angularDamping = 0.3f, .gravityFactor = 0.5f, .allowSleeping = false});
    Collider c;
    c.shape       = ColliderShape::Capsule;
    c.halfExtents = {1.0f, 2.0f, 3.0f};
    c.radius      = 0.7f;
    c.halfHeight  = 1.1f;
    c.center      = {0.0f, 0.5f, 0.0f};
    c.friction    = 0.9f;
    c.restitution = 0.4f;
    c.trigger     = true;
    r.Emplace<Collider>(e, c);
    r.Emplace<CharacterController>(e, CharacterController{.radius = 0.4f, .height = 2.0f, .maxSlope = 0.5f,
                                                          .stepHeight = 0.25f, .jumpSpeed = 6.0f});

    const std::string   snapshot = SnapshotEntities(scene, std::span(&e, 1));
    const auto          copies   = RestoreEntities(scene, snapshot, RestoreMode::Duplicate);
    CHECK(copies.size() == 1);
    if (copies.size() == 1) {
        const Entity copy = copies[0];
        CHECK(r.Has<RigidBody>(copy) && r.Get<RigidBody>(copy) == r.Get<RigidBody>(e));
        CHECK(r.Has<Collider>(copy) && r.Get<Collider>(copy) == c);
        CHECK(r.Has<CharacterController>(copy) && r.Get<CharacterController>(copy) == r.Get<CharacterController>(e));
    }

    // Entity state (undo): removing components is restored too.
    const std::string state = SnapshotEntityState(scene, e);
    r.Remove<Collider>(e);
    r.Get<RigidBody>(e).mass = 9.0f;
    ApplyEntityState(scene, e, state);
    CHECK(r.Has<Collider>(e) && r.Get<Collider>(e) == c && r.Get<RigidBody>(e).mass == 3.0f);
}
