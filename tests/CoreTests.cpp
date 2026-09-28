#include "Test.h"

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/GltfLoader.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Frustum.h"

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
