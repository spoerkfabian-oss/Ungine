#include "Test.h"

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Frustum.h"

#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <atomic>
#include <cmath>
#include <chrono>
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

int main(int argc, char** argv)
{
    return Test::RunAll(argc > 1 ? argv[1] : "");
}
