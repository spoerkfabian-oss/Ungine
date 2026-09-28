#include "Test.h"

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/GltfLoader.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Renderer/ShadowAtlas.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Engine/Scene/Camera.h"
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
    r.Get<Transform>(node).position = glm::vec3(1.0f, 2.0f, 3.0f);
    r.Get<Transform>(node).rotation = glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
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
    r.Get<Transform>(node2).scale = glm::vec3(5.0f);
    r.Get<Name>(node2).value      = "Renamed";
    r.Remove<MeshRenderer>(node2);
    r.Emplace<Light>(node2);
    ApplyEntityState(scene, node2, state);
    CHECK(r.Get<Transform>(node2).scale == glm::vec3(1.0f) && r.Get<Name>(node2).value == "Node");
    CHECK(r.Has<MeshRenderer>(node2) && !r.Has<Light>(node2));
}
