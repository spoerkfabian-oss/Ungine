// Integration tests on a real Vulkan device (needs a display; CI: Xvfb + lavapipe).
#include "Test.h"

#include "Editor/Editor.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Primitives.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Core/Window.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Renderer/Vulkan/VulkanContext.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <string_view>
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
const fs::path  kSpheres  = fs::path(ENGINE_ASSET_DIR) / "models" / "MetalRoughSpheresNoTextures.glb";

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

TEST_CASE(Asset_GeneratedModelIsNotCached)
{
    const std::size_t cached = F().assets->CachedCount();
    const ModelHandle a      = F().assets->CreateModel(MakePlane("Plane", 4.0f, MaterialData{.name = "Gray"}));
    const ModelHandle b      = F().assets->CreateModel(MakePlane("Plane", 4.0f, MaterialData{.name = "Gray"}));
    CHECK(a != b); // same name, still two assets
    CHECK(F().assets->CachedCount() == cached);
    CHECK(F().Pump([&] { return Settled(a) && Settled(b); }));

    const Model* plane = F().assets->Get(a);
    CHECK(plane && plane->meshes.size() == 1 && plane->meshes[0].submeshes[0].indexCount == 6);
    F().assets->Release(a);
    F().assets->Release(b);
    CHECK(F().assets->State(a) == AssetState::Invalid && F().assets->State(b) == AssetState::Invalid);
    CHECK(F().assets->CachedCount() == cached);
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

TEST_CASE(Render_PbrFrameAndFrustumCulling)
{
    // Full SceneRenderer path (IBL generation, PBR, sky, tone mapping) under validation.
    const ModelHandle h = F().assets->LoadModel(kSpheres);
    CHECK(F().Pump([&] { return Settled(h); }));
    const Model* model = F().assets->Get(h);
    CHECK(model != nullptr);
    if (!model)
        return;

    Scene scene;
    InstantiateModel(scene, h, *model);
    scene.UpdateTransforms();
    SceneRenderer renderer(*F().renderer, *F().context, *F().assets);
    renderer.shadows.resolution  = 1024; // lavapipe rasterizes on the CPU
    renderer.shadows.maxDistance = 50.0f;

    const glm::vec3 center = (model->boundsMin + model->boundsMax) * 0.5f;
    const float     radius = glm::length(model->boundsMax - model->boundsMin) * 0.5f;
    const glm::vec3 eye    = center + glm::vec3(0.0f, 0.0f, radius * 3.0f);
    const auto renderTowards = [&](glm::vec3 target) {
        const CameraData camera{.view       = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f)),
                                .projection = PerspectiveReverseZ(glm::radians(60.0f), 320.0f / 240.0f, 0.01f),
                                .position   = eye};
        SceneRenderStats stats;
        for (int i = 0; i < 3; ++i) { // skipped frames (swapchain out of date) are fine
            if (auto frame = F().renderer->BeginFrame()) {
                renderer.Render(*frame, scene, camera);
                F().renderer->EndFrame(*frame);
                stats = renderer.Stats();
            }
        }
        return stats;
    };

    const SceneRenderStats facing = renderTowards(center);
    CHECK(facing.drawCalls > 0 && facing.culled == 0 && facing.triangles > 0);
    CHECK(facing.shadowDraws >= facing.drawCalls); // at least one cascade sees every caster
    // GPU timestamps of the passes (frames in flight old, collected after the fence wait).
    const auto timings = F().renderer->Profiler().Results();
    CHECK(!F().renderer->Profiler().Supported() || timings.size() >= 6);
    for (const GpuTiming& t : timings)
        CHECK(t.name && t.milliseconds >= 0.0 && t.milliseconds < 60'000.0);
    CHECK(timings.empty() || std::string_view{timings.front().name} == "Frame");

    // Auto exposure (default on) reached the CPU through the per-frame-slot readback.
    CHECK(std::isfinite(facing.exposure) && facing.exposure > 0.0f && facing.averageLuminance > 0.0f);

    // Debug views and AO / auto exposure toggles.
    for (DebugView view : {DebugView::AmbientOcclusion, DebugView::Normals, DebugView::None}) {
        renderer.post.debugView = view;
        CHECK(renderTowards(center).drawCalls == facing.drawCalls);
    }
    renderer.ao.enabled        = false;
    renderer.post.autoExposure = false;
    renderer.post.exposure     = 2.0f;
    CHECK(renderTowards(center).exposure == 2.0f);
    renderer.ao.enabled        = true;
    renderer.post.autoExposure = true;
    renderer.post.exposure     = 1.0f;

    // Feature toggles and runtime resolution changes (map recreation while frames are in flight).
    renderer.post.bloom            = false;
    renderer.shadows.debugCascades = true;
    renderer.shadows.cascadeCount  = 2;
    renderer.shadows.resolution    = 512;
    CHECK(renderTowards(center).drawCalls == facing.drawCalls);
    renderer.shadows.enabled = false;
    CHECK(renderTowards(center).shadowDraws == 0);
    renderer.shadows.enabled = true;
    renderer.post.bloom      = true;
    // Moving the sun regenerates the IBL maps while earlier frames may still sample them.
    for (float angle : {0.3f, 1.2f, 2.5f}) {
        renderer.lighting.sky.sunDirection = glm::vec3(std::sin(angle), -0.6f, std::cos(angle));
        CHECK(renderTowards(center).drawCalls == facing.drawCalls);
    }

    const SceneRenderStats away = renderTowards(eye + (eye - center)); // model behind the camera
    CHECK(away.drawCalls == 0 && away.culled == facing.drawCalls);

    F().assets->Release(h);
}

TEST_CASE(Render_ClusteredLights)
{
    // Point and spot lights through light culling + shading under validation, CPU light culling,
    // cluster list overflow and the cluster debug view.
    MaterialData material{.name = "Floor", .baseColorFactor = glm::vec4(0.8f), .metallic = 0.0f, .roughness = 0.7f};
    const ModelHandle h = F().assets->CreateModel(MakePlane("Floor", 20.0f, material));
    CHECK(F().Pump([&] { return Settled(h); }));
    const Model* model = F().assets->Get(h);
    CHECK(model != nullptr);
    if (!model)
        return;

    Scene scene;
    InstantiateModel(scene, h, *model);
    Registry& registry = scene.GetRegistry();
    const auto addLight = [&](glm::vec3 position, Light light) {
        const Entity e = scene.CreateEntity("Light");
        registry.Get<Transform>(e).position = position;
        registry.Emplace<Light>(e, light);
        return e;
    };
    addLight({0.0f, 1.0f, 0.0f}, {.type = LightType::Point, .color = glm::vec3(1.0f, 0.2f, 0.2f), .intensity = 5.0f, .range = 4.0f});
    const Entity spot = addLight({2.0f, 2.0f, 0.0f}, {.type = LightType::Spot, .intensity = 20.0f, .range = 6.0f,
                                                      .innerConeAngle = 0.3f, .outerConeAngle = 0.3f}); // hard edge
    registry.Get<Transform>(spot).rotation = glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)); // down
    addLight({0.0f, 1.0f, 60.0f}, {.intensity = 5.0f, .range = 2.0f});   // behind the camera: CPU-culled
    addLight({0.0f, 1.0f, -5.0f}, {.intensity = 0.0f, .range = 2.0f});   // black: skipped
    scene.UpdateTransforms();

    SceneRenderer renderer(*F().renderer, *F().context, *F().assets);
    renderer.shadows.resolution = 512;
    const glm::vec3 eye{0.0f, 3.0f, 8.0f};
    const CameraData camera{.view       = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                            .projection = PerspectiveReverseZ(glm::radians(60.0f), 320.0f / 240.0f, 0.05f),
                            .position   = eye,
                            .nearPlane  = 0.05f};
    const auto render = [&] {
        SceneRenderStats stats;
        for (int i = 0; i < 3; ++i)
            if (auto frame = F().renderer->BeginFrame()) {
                renderer.Render(*frame, scene, camera);
                F().renderer->EndFrame(*frame);
                stats = renderer.Stats();
            }
        return stats;
    };

    SceneRenderStats stats = render();
    CHECK(stats.lightsTotal == 4 && stats.lights == 2);
    bool cullScope = false;
    for (const GpuTiming& t : F().renderer->Profiler().Results())
        cullScope |= std::string_view{t.name} == "Light culling";
    CHECK(!F().renderer->Profiler().Supported() || cullScope);

    renderer.post.debugView = DebugView::LightClusters;
    CHECK(render().lights == 2);
    renderer.post.debugView = DebugView::None;

    // More lights than a cluster holds: the list is truncated, nothing breaks.
    for (int i = 0; i < 300; ++i)
        addLight({0.01f * static_cast<float>(i % 17), 0.5f, 0.01f * static_cast<float>(i / 17)},
                 {.intensity = 0.05f, .range = 3.0f});
    scene.UpdateTransforms();
    CHECK(render().lights == 302);

    renderer.lights.enabled = false;
    stats                   = render();
    CHECK(stats.lights == 0 && stats.lightsTotal == 304);

    F().assets->Release(h);
}

TEST_CASE(Editor_FramesSelectionAndToggle)
{
    // Editor frame flow (viewport texture + UI into the swapchain) under validation, recreated
    // like the Sandbox's F1 toggle.
    const ModelHandle h = F().assets->LoadModel(kBox);
    CHECK(F().Pump([&] { return Settled(h); }));
    const Model* model = F().assets->Get(h);
    CHECK(model != nullptr);
    if (!model)
        return;

    Scene         scene;
    const Entity  root = InstantiateModel(scene, h, *model);
    const Entity  spot = scene.CreateEntity("Spot");
    scene.GetRegistry().Emplace<Light>(spot, Light{.type = LightType::Spot, .intensity = 5.0f});
    scene.GetRegistry().Get<Transform>(spot).position = glm::vec3(0.5f, 1.0f, 1.0f);
    const Entity point = scene.CreateEntity("Point");
    scene.GetRegistry().Emplace<Light>(point, Light{.intensity = 5.0f, .range = 2.0f});
    SceneRenderer sceneRenderer(*F().renderer, *F().context, *F().assets);
    sceneRenderer.shadows.resolution = 512;
    FlyCamera camera;
    camera.position = glm::vec3(0.0f, 1.0f, 4.0f);

    const EditorContext context{.window        = *F().window,
                                .renderer      = *F().renderer,
                                .scene         = scene,
                                .assets        = *F().assets,
                                .sceneRenderer = sceneRenderer,
                                .camera        = camera};
    const auto runFrames = [&](Editor& editor, int count) {
        for (int i = 0; i < count; ++i) {
            F().window->PollEvents();
            F().events.Flush();
            F().assets->Update();
            editor.Update(1.0f / 60.0f);
            if (auto frame = F().renderer->BeginFrame()) {
                editor.Render(*frame);
                F().renderer->EndFrame(*frame);
            }
        }
    };

    for (int round = 0; round < 2; ++round) {
        Editor editor(context);
        CHECK(camera.moveRequiresLook);
        editor.Select(root);
        runFrames(editor, 6);
        CHECK(editor.Selected() == root);
        CHECK(editor.ViewportAspect() > 0.0f);
        CHECK(!editor.WantsKeyboard());

        bool uiScope = false;
        for (const GpuTiming& t : F().renderer->Profiler().Results())
            uiScope |= std::string_view{t.name} == "Editor UI";
        CHECK(!F().renderer->Profiler().Supported() || uiScope);
        CHECK(sceneRenderer.Stats().drawCalls > 0); // the scene went into the viewport texture
        CHECK(sceneRenderer.Stats().lightsTotal == 2);

        // Light overlay with a selected spot / point light (cone and range sphere).
        for (Entity light : {spot, point}) {
            editor.Select(light);
            runFrames(editor, 2);
            CHECK(editor.Selected() == light);
        }

        if (round == 1) { // selection of a destroyed entity is dropped
            editor.Select(root);
            scene.DestroyEntity(root);
            runFrames(editor, 2);
            CHECK(editor.Selected() == NullEntity);
        }
    }
    CHECK(!camera.moveRequiresLook); // restored by the editor
    F().assets->Release(h);
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
