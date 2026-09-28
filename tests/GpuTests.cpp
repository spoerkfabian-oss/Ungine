// Integration tests on a real Vulkan device (needs a display; CI: Xvfb + lavapipe).
#include "Test.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Core/Window.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/VulkanContext.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

// One window per process (GLFW), so every test shares this.
struct Fixture {
    EventBus                       events;
    std::unique_ptr<Window>        window;
    std::unique_ptr<VulkanContext> context;
    std::unique_ptr<Renderer>      renderer;
    std::unique_ptr<ThreadPool>    jobs;
    std::unique_ptr<AssetManager>  assets;

    std::vector<ModelHandle> loaded, failed;
    Subscription             loadedSub, failedSub;

    Fixture()
    {
        window   = std::make_unique<Window>(WindowDesc{.title = "EngineGpuTests", .width = 320, .height = 240}, events);
        context  = std::make_unique<VulkanContext>(*window, VulkanContextDesc{.appName = "EngineGpuTests",
                                                                              .enableValidation = true});
        renderer = std::make_unique<Renderer>(*context, *window, events, RendererDesc{.vsync = false});
        jobs     = std::make_unique<ThreadPool>(4);
        assets   = std::make_unique<AssetManager>(*renderer, *jobs, events);
        loadedSub = events.Subscribe<AssetLoadedEvent<Model>>([this](const auto& e) { loaded.push_back(e.handle); });
        failedSub = events.Subscribe<AssetFailedEvent<Model>>([this](const auto& e) { failed.push_back(e.handle); });
    }

    ~Fixture()
    {
        context->WaitIdle();
        assets.reset(); // same teardown order as Application
        jobs.reset();
    }

    // Runs frames (asset update + empty frame) until `done` or the frame budget is spent.
    bool Pump(const std::function<bool()>& done, int maxFrames = 2000)
    {
        for (int i = 0; i < maxFrames; ++i) {
            window->PollEvents();
            events.Flush();
            assets->Update();
            if (done())
                return true;
            if (auto frame = renderer->BeginFrame())
                renderer->EndFrame(*frame);
        }
        return false;
    }
};

Fixture*        g_Fixture = nullptr;
const fs::path  kBox      = fs::path(ENGINE_ASSET_DIR) / "models" / "BoxTextured.glb";

Fixture& F() { return *g_Fixture; }

bool Settled(ModelHandle h)
{
    const AssetState s = F().assets->State(h);
    return s != AssetState::Loading && s != AssetState::Uploading;
}

} // namespace

TEST_CASE(Asset_LoadBecomesReady)
{
    const ModelHandle h = F().assets->LoadModel(kBox);
    CHECK(h);
    CHECK(F().assets->State(h) == AssetState::Loading);
    CHECK(F().assets->Get(h) == nullptr);

    F().loaded.clear();
    CHECK(F().Pump([&] { return Settled(h); }));
    CHECK(F().assets->State(h) == AssetState::Ready);
    const Model* model = F().assets->Get(h);
    CHECK(model && !model->meshes.empty() && model->textures.size() == 1);
    CHECK(F().loaded.size() == 1 && F().loaded[0] == h);

    F().assets->Release(h);
    CHECK(F().assets->State(h) == AssetState::Invalid);
    CHECK(F().assets->Get(h) == nullptr);
}

TEST_CASE(Asset_CacheSharesHandleAndRefCounts)
{
    const ModelHandle a = F().assets->LoadModel(kBox);
    const ModelHandle b = F().assets->LoadModel(kBox.parent_path() / ".." / "models" / kBox.filename());
    CHECK(a == b);
    CHECK(F().assets->RefCount(a) == 2);
    CHECK(F().assets->CachedCount() == 1);
    CHECK(F().Pump([&] { return Settled(a); }));

    F().assets->Release(a);
    CHECK(F().assets->State(a) == AssetState::Ready); // one reference left
    F().assets->Release(b);
    CHECK(F().assets->State(a) == AssetState::Invalid);
    CHECK(F().assets->CachedCount() == 0);
}

TEST_CASE(Asset_MissingFileFails)
{
    F().failed.clear();
    const ModelHandle h = F().assets->LoadModel("does/not/exist.glb");
    CHECK(F().Pump([&] { return Settled(h); }));
    CHECK(F().assets->State(h) == AssetState::Failed);
    CHECK(!F().assets->Error(h).empty());
    CHECK(F().failed.size() == 1 && F().failed[0] == h);
    F().assets->Release(h);
    CHECK(F().assets->State(h) == AssetState::Invalid);
}

TEST_CASE(Asset_ReleaseWhileLoadingThenReload)
{
    const ModelHandle first = F().assets->LoadModel(kBox);
    F().assets->Release(first); // job still running: handle is stale at once
    CHECK(F().assets->State(first) == AssetState::Invalid);
    CHECK(F().assets->CachedCount() == 0);

    const ModelHandle second = F().assets->LoadModel(kBox); // must not hit the orphaned entry
    CHECK(second != first);
    CHECK(F().Pump([&] { return Settled(second); }));
    CHECK(F().assets->State(second) == AssetState::Ready);
    CHECK(F().assets->State(first) == AssetState::Invalid);
    F().assets->Release(second);
}

TEST_CASE(Asset_StaleHandleAfterSlotReuse)
{
    const ModelHandle old = F().assets->LoadModel(kBox);
    CHECK(F().Pump([&] { return Settled(old); }));
    F().assets->Release(old);

    const ModelHandle fresh = F().assets->LoadModel(kBox);
    CHECK(fresh.index == old.index && fresh.generation != old.generation);
    CHECK(F().assets->State(old) == AssetState::Invalid);
    CHECK(F().Pump([&] { return Settled(fresh); }));
    CHECK(F().assets->Get(old) == nullptr && F().assets->Get(fresh) != nullptr);
    F().assets->Release(fresh);
}

TEST_CASE(Asset_ManyConcurrentLoads)
{
    // Distinct files -> distinct cache entries -> parallel jobs hammering uploader + bindless.
    const fs::path dir = fs::temp_directory_path() / "engine_gpu_tests";
    fs::create_directories(dir);
    std::vector<ModelHandle> handles;
    for (int i = 0; i < 16; ++i) {
        const fs::path copy = dir / ("box" + std::to_string(i) + ".glb");
        fs::copy_file(kBox, copy, fs::copy_options::overwrite_existing);
        handles.push_back(F().assets->LoadModel(copy));
    }
    CHECK(F().assets->CachedCount() == handles.size());
    CHECK(F().Pump([&] {
        for (ModelHandle h : handles)
            if (!Settled(h))
                return false;
        return true;
    }));
    for (ModelHandle h : handles) {
        CHECK(F().assets->State(h) == AssetState::Ready);
        F().assets->Release(h);
    }
    F().Pump([] { return false; }, 4); // let deferred releases run
    fs::remove_all(dir);
}

TEST_CASE(Asset_ShutdownWithLoadsInFlight)
{
    // Destroying the manager mid-load must wait for the jobs and free everything.
    {
        AssetManager local(*F().renderer, *F().jobs, F().events);
        for (int i = 0; i < 4; ++i)
            (void)local.LoadModel(kBox);
    }
    F().Pump([] { return false; }, 4);
}

int main(int argc, char** argv)
{
    int result = 1;
    try {
        Fixture fixture;
        g_Fixture = &fixture;
        result    = Test::RunAll(argc > 1 ? argv[1] : "");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fixture setup failed: %s\n", e.what());
        return 1;
    }
    g_Fixture = nullptr;

    const std::uint32_t errors = VulkanContext::ValidationErrorCount(); // teardown included
    std::printf("Vulkan validation errors: %u\n", errors);
    return errors == 0 ? result : 1;
}
