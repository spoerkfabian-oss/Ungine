// Integration tests on a real Vulkan device (needs a display; CI: Xvfb + lavapipe).
#include "Test.h"

#include "Editor/Editor.h"
#include "Editor/ProjectLauncher.h"
#include "Editor/ScriptGraphEditor.h"
#include "Engine/Core/Project.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Primitives.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Core/Window.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/TextOverlay.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/SpatialIndex.h"
#include "Engine/Script/ScriptRegistry.h"
#include "Engine/Script/ScriptSystem.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Renderer/Vulkan/VulkanContext.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
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

    // Runs frames (asset update + empty frame) until `done`, the frame budget or 120 s are spent.
    bool Pump(const std::function<bool()>& done, int maxFrames = 1000000)
    {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < maxFrames && std::chrono::steady_clock::now() - start < std::chrono::seconds(120); ++i) {
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
    renderer.culling.gpuDriven   = false; // exact per-submesh draw counts (GPU path: Render_GpuDriven*)

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

    Scene        scene;
    const Entity floor    = InstantiateModel(scene, h, *model);
    Registry&    registry = scene.GetRegistry();
    const auto addLight = [&](glm::vec3 position, Light light) {
        const Entity e = scene.CreateEntity("Light");
        scene.EditTransform(e).position = position;
        registry.Emplace<Light>(e, light);
        return e;
    };
    const Entity point = addLight({0.0f, 1.0f, 0.0f}, {.type = LightType::Point, .color = glm::vec3(1.0f, 0.2f, 0.2f),
                                                       .intensity = 5.0f, .range = 4.0f});
    const Entity spot = addLight({2.0f, 2.0f, 0.0f}, {.type = LightType::Spot, .intensity = 20.0f, .range = 6.0f,
                                                      .innerConeAngle = 0.3f, .outerConeAngle = 0.3f}); // hard edge
    scene.EditTransform(spot).rotation = glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)); // down
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

    // Local shadows: spot = 1 atlas tile, point = 6, cached while nothing in range changes.
    CHECK(stats.shadowedLights == 2 && stats.shadowTiles == 7);
    const auto renderOne = [&] {
        for (int attempt = 0; attempt < 10; ++attempt) // skipped frames (swapchain) are retried
            if (auto frame = F().renderer->BeginFrame()) {
                renderer.Render(*frame, scene, camera);
                F().renderer->EndFrame(*frame);
                return renderer.Stats();
            }
        return SceneRenderStats{};
    };
    stats = renderOne();
    CHECK(stats.shadowTiles == 7 && stats.shadowTilesRendered == 0 && stats.localShadowDraws == 0);
    scene.EditTransform(floor).position.y = -0.1f; // a caster in range of both lights
    scene.UpdateTransforms();
    stats = renderOne();
    CHECK(stats.shadowTilesRendered == 7 && stats.localShadowDraws > 0);
    CHECK(renderOne().shadowTilesRendered == 0);
    scene.EditTransform(point).position.x = 0.5f; // only the point light's six views
    scene.UpdateTransforms();
    CHECK(renderOne().shadowTilesRendered == 6);
    const Entity farAway = scene.CreateEntity("Far");
    registry.Emplace<MeshRenderer>(farAway, MeshRenderer{.model = h, .meshIndex = 0});
    scene.EditTransform(farAway).position = glm::vec3(0.0f, 0.0f, -100.0f); // out of every light's range
    scene.UpdateTransforms();
    CHECK(renderOne().shadowTilesRendered == 0);
    renderer.localShadows.depthBias += 0.5f; // baked into the maps
    CHECK(renderOne().shadowTilesRendered == 7);
    renderer.localShadows.depthBias -= 0.5f;

    // Budget, per-light flag, runtime atlas resize, debug view.
    renderer.post.debugView = DebugView::ShadowAtlas;
    CHECK(render().shadowTiles == 7);
    renderer.post.debugView        = DebugView::None;
    renderer.localShadows.maxLights = 1;
    CHECK(render().shadowedLights == 1);
    renderer.localShadows.maxLights = 8;
    renderer.localShadows.atlasSize = 1024; // recreated while earlier frames may still sample the old one
    stats = render();
    CHECK(stats.shadowedLights == 2 && stats.shadowTiles == 7);
    registry.Get<Light>(spot).castShadows = false;
    scene.MarkChanged(spot);
    CHECK(render().shadowTiles == 6);
    registry.Get<Light>(spot).castShadows = true;
    scene.MarkChanged(spot);
    renderer.localShadows.enabled = false;
    stats                         = render();
    CHECK(stats.shadowedLights == 0 && stats.lights == 2);
    renderer.localShadows.enabled = true;

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

TEST_CASE(Scene_SpatialIndexSync)
{
    // Meshes wait for their model, then follow moves and destruction; queries match brute force.
    Scene             scene;
    SpatialIndex      index;
    const ModelHandle box = F().assets->CreatePrimitive({.shape = PrimitiveShape::Box, .size = 1.0f});
    std::vector<Entity> boxes;
    for (int x = 0; x < 10; ++x)
        for (int z = 0; z < 10; ++z) {
            const Entity e = scene.CreateEntity("Box");
            scene.GetRegistry().Emplace<MeshRenderer>(e, MeshRenderer{.model = box, .meshIndex = 0});
            scene.EditTransform(e).position = glm::vec3(static_cast<float>(x) * 3.0f, 0.0f, static_cast<float>(z) * 3.0f);
            boxes.push_back(e);
        }
    const Entity lamp = scene.CreateEntity("Lamp");
    scene.GetRegistry().Emplace<Light>(lamp, Light{.intensity = 1.0f, .range = 2.0f});
    scene.UpdateTransforms();
    index.Sync(scene, *F().assets);
    CHECK(index.LightCount() == 1);
    if (F().assets->State(box) != AssetState::Ready) // usually still loading here
        CHECK(index.MeshCount() == 0 && index.LastSync().pending == 100);
    CHECK(F().Pump([&] { return Settled(box); }));
    index.Sync(scene, *F().assets);
    CHECK(index.MeshCount() == 100 && index.SubmeshCount() == 100 && index.LastSync().pending == 0);
    CHECK(index.MeshTree().Validate() && index.MeshTree().Height() <= 16);

    // Raycast straight down onto box (2, 3) at (6, 0, 9).
    const auto hit = index.Raycast(glm::vec3(6.0f, 10.0f, 9.0f), glm::vec3(0.0f, -1.0f, 0.0f));
    CHECK(hit.has_value() && hit->entity == boxes[2 * 10 + 3] && std::abs(hit->distance - 9.5f) < 1e-3f);
    CHECK(!index.Raycast(glm::vec3(1.5f, 10.0f, 1.5f), glm::vec3(0.0f, -1.0f, 0.0f)).has_value()); // between boxes

    // Frustum query == brute force over the tight bounds.
    const glm::mat4 viewProj = PerspectiveReverseZ(glm::radians(40.0f), 1.0f, 0.1f) *
                               glm::lookAt(glm::vec3(-5.0f, 8.0f, -5.0f), glm::vec3(10.0f, 0.0f, 10.0f), glm::vec3(0, 1, 0));
    const Frustum       frustum = Frustum::FromViewProjection(viewProj);
    std::vector<Entity> found;
    index.QueryMeshes(frustum, [&](const SpatialIndex::MeshProxy& p) { found.push_back(p.entity); });
    std::size_t expected = 0;
    for (Entity e : boxes) {
        const bool inside = frustum.Intersects(*index.Bounds(e));
        expected += inside ? 1 : 0;
        CHECK(inside == (std::ranges::find(found, e) != found.end()));
    }
    CHECK(found.size() == expected && expected > 0 && expected < boxes.size());

    // Move one box far away, destroy another.
    scene.EditTransform(boxes[0]).position = glm::vec3(500.0f, 0.0f, 0.0f);
    scene.DestroyEntity(boxes[1]);
    scene.UpdateTransforms();
    index.Sync(scene, *F().assets);
    CHECK(index.MeshCount() == 99 && !index.Bounds(boxes[1]).has_value());
    CHECK(index.Bounds(boxes[0]).has_value() && index.Bounds(boxes[0])->min.x > 499.0f);
    CHECK(index.ChangedRegions().size() == 3); // old + new bounds of the moved box, the destroyed one
    CHECK(index.MeshTree().Validate());
    F().assets->Release(box);
}

TEST_CASE(Render_PickingAndOutline)
{
    // Entity IDs from the prepass: pick the box in the middle of the image and the sky in a corner.
    const ModelHandle h = F().assets->CreatePrimitive({.shape = PrimitiveShape::Box, .size = 2.0f});
    CHECK(F().Pump([&] { return Settled(h); }));
    const Model* model = F().assets->Get(h);
    CHECK(model != nullptr);
    if (!model)
        return;
    Scene        scene;
    const Entity root = InstantiateModel(scene, h, *model);
    const Entity box  = scene.GetRegistry().Get<Hierarchy>(root).children.at(0);
    scene.UpdateTransforms();

    SceneRenderer renderer(*F().renderer, *F().context, *F().assets);
    renderer.overlay.picking  = true;
    renderer.overlay.outlined = {box};
    const glm::vec3  eye{0.0f, 0.0f, 6.0f};
    const CameraData camera{.view       = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                            .projection = PerspectiveReverseZ(glm::radians(60.0f), 320.0f / 240.0f, 0.05f),
                            .position   = eye,
                            .nearPlane  = 0.05f};
    const auto pick = [&](std::uint32_t x, std::uint32_t y) -> std::optional<Entity> {
        std::optional<Entity> result;
        bool                  requested = false;
        for (int i = 0; i < 8 && !result; ++i)
            if (auto frame = F().renderer->BeginFrame()) {
                if (!requested) { // the output is the swapchain image here (320 x 240)
                    renderer.RequestPick(x * frame->extent.width / 320, y * frame->extent.height / 240);
                    requested = true;
                }
                renderer.Render(*frame, scene, camera);
                F().renderer->EndFrame(*frame);
                result = renderer.TakePickResult();
            }
        return result;
    };
    const auto center = pick(160, 120);
    CHECK(center.has_value() && *center == box);
    const auto corner = pick(2, 2);
    CHECK(corner.has_value() && *corner == NullEntity);

    renderer.overlay.picking = false; // releases the ID target; outline ignored
    CHECK(!pick(160, 120).has_value());
    F().assets->Release(h);
}

TEST_CASE(SceneFile_SaveLoadRoundTrip)
{
    // Files: models by path (relative to the scene) or primitive recipe, lights, settings, camera.
    Scene             scene;
    Registry&         r      = scene.GetRegistry();
    const ModelHandle boxTex = F().assets->LoadModel(kBox);
    const ModelHandle plane  = F().assets->CreatePrimitive({.shape = PrimitiveShape::Plane, .size = 10.0f});
    const Entity      a      = scene.CreateEntity("A");
    r.Emplace<MeshRenderer>(a, MeshRenderer{.model = boxTex, .meshIndex = 0});
    const Entity b = scene.CreateEntity("B", a);
    r.Emplace<MeshRenderer>(b, MeshRenderer{.model = plane, .meshIndex = 0});
    scene.EditTransform(b).position = glm::vec3(1.0f, 2.0f, 3.0f);
    const Entity lamp = scene.CreateEntity("Lamp", b);
    r.Emplace<Light>(lamp, Light{.type = LightType::Spot, .intensity = 9.0f, .outerConeAngle = 0.5f});
    const ModelHandle generated = F().assets->CreateModel(MakePlane("Tmp", 1.0f, MaterialData{}));
    const Entity      skipped   = scene.CreateEntity("Generated");
    r.Emplace<MeshRenderer>(skipped, MeshRenderer{.model = generated, .meshIndex = 0});

    SceneRenderer settings(*F().renderer, *F().context, *F().assets);
    settings.post.tonemapper      = Tonemapper::Aces;
    settings.localShadows.maxLights = 3;
    settings.culling.occlusion      = false;
    FlyCamera camera;
    camera.position = glm::vec3(4.0f, 5.0f, 6.0f);
    camera.yaw      = 1.25f;
    PhysicsSettings physics;
    physics.gravity        = {0.0f, -3.0f, 0.0f};
    physics.collisionSteps = 3;
    physics.interpolate    = false;
    physics.SetLayerCollision(2, 7, false);

    const fs::path file = fs::path(ENGINE_ASSET_DIR) / "test_roundtrip.scene.json"; // next to the models
    SaveSceneFile(file, scene, *F().assets, {.renderer = &settings, .camera = &camera, .physics = &physics});

    Scene         loaded;
    SceneRenderer settings2(*F().renderer, *F().context, *F().assets);
    FlyCamera       camera2;
    PhysicsSettings physics2;
    const auto      handles =
        LoadSceneFile(file, loaded, *F().assets, {.renderer = &settings2, .camera = &camera2, .physics = &physics2});
    fs::remove(file);

    Registry& r2 = loaded.GetRegistry();
    CHECK(r2.AliveCount() == 4 && handles.size() == 2);
    const Entity a2    = loaded.FindByUuid(r.Get<Uuid>(a).value);
    const Entity b2    = loaded.FindByUuid(r.Get<Uuid>(b).value);
    const Entity lamp2 = loaded.FindByUuid(r.Get<Uuid>(lamp).value);
    const Entity gen2  = loaded.FindByUuid(r.Get<Uuid>(skipped).value);
    CHECK(a2 != NullEntity && b2 != NullEntity && lamp2 != NullEntity && gen2 != NullEntity);
    if (a2 == NullEntity || b2 == NullEntity || lamp2 == NullEntity || gen2 == NullEntity)
        return;
    CHECK(r2.Get<Hierarchy>(b2).parent == a2 && r2.Get<Hierarchy>(lamp2).parent == b2);
    CHECK(r2.Get<Transform>(b2).position == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(r2.Get<MeshRenderer>(a2).model == boxTex); // cache hit: same handle, refcount + 1
    CHECK(r2.Get<MeshRenderer>(b2).model == plane);
    CHECK(F().assets->RefCount(boxTex) == 2 && F().assets->RefCount(plane) == 2);
    CHECK(r2.Has<Light>(lamp2) && r2.Get<Light>(lamp2).intensity == 9.0f && r2.Get<Light>(lamp2).outerConeAngle == 0.5f);
    CHECK(!r2.Has<MeshRenderer>(gen2)); // generated models cannot be saved
    CHECK(settings2.post.tonemapper == Tonemapper::Aces && settings2.localShadows.maxLights == 3);
    CHECK(!settings2.culling.occlusion && settings2.culling.gpuDriven);
    CHECK(camera2.position == glm::vec3(4.0f, 5.0f, 6.0f) && camera2.yaw == 1.25f);
    CHECK(physics2.gravity == physics.gravity && physics2.collisionSteps == 3 && !physics2.interpolate);
    CHECK(physics2.layerCollision == physics.layerCollision && !physics2.LayersCollide(7, 2));

    // Broken files throw and leave the scene alone.
    const fs::path broken = fs::path(ENGINE_ASSET_DIR) / "test_broken.scene.json";
    { std::ofstream(broken) << R"({"version": 1, "entities": [{"uuid": 5, "name": "X", "mesh": {"model": {}}}]})"; }
    bool threw = false;
    try {
        (void)LoadSceneFile(broken, loaded, *F().assets);
    } catch (const std::exception&) {
        threw = true;
    }
    fs::remove(broken);
    CHECK(threw && r2.AliveCount() == 4);

    for (ModelHandle handle : handles)
        F().assets->Release(handle);
    for (ModelHandle handle : {boxTex, plane, generated})
        F().assets->Release(handle);
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
    scene.EditTransform(spot).position = glm::vec3(0.5f, 1.0f, 1.0f);
    const Entity point = scene.CreateEntity("Point");
    scene.GetRegistry().Emplace<Light>(point, Light{.intensity = 5.0f, .range = 2.0f});
    SceneRenderer sceneRenderer(*F().renderer, *F().context, *F().assets);
    sceneRenderer.shadows.resolution = 512;
    FlyCamera camera;
    camera.position = glm::vec3(0.0f, 1.0f, 4.0f);

    std::vector<ModelHandle> modelRefs;
    const EditorContext      context{.window        = *F().window,
                                     .renderer      = *F().renderer,
                                     .scene         = scene,
                                     .assets        = *F().assets,
                                     .sceneRenderer = sceneRenderer,
                                     .camera        = camera,
                                     .modelRefs     = modelRefs};
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

TEST_CASE(Editor_UndoRedoDuplicateAndSceneFiles)
{
    const ModelHandle h = F().assets->LoadModel(kBox);
    CHECK(F().Pump([&] { return Settled(h); }));
    const Model* model = F().assets->Get(h);
    CHECK(model != nullptr);
    if (!model)
        return;

    Scene         scene;
    Registry&     r     = scene.GetRegistry();
    const Entity  root  = InstantiateModel(scene, h, *model); // root + 1 mesh node
    const Entity  light = scene.CreateEntity("Light");
    r.Emplace<Light>(light, Light{.intensity = 3.0f});
    const std::uint64_t rootUuid  = r.Get<Uuid>(root).value;
    const std::uint64_t lightUuid = r.Get<Uuid>(light).value;
    SceneRenderer sceneRenderer(*F().renderer, *F().context, *F().assets);
    sceneRenderer.shadows.resolution = 512;
    FlyCamera                camera;
    std::vector<ModelHandle> modelRefs;
    Editor editor({.window        = *F().window,
                   .renderer      = *F().renderer,
                   .scene         = scene,
                   .assets        = *F().assets,
                   .sceneRenderer = sceneRenderer,
                   .camera        = camera,
                   .modelRefs     = modelRefs});
    const auto runFrames = [&](int count) {
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
    const std::size_t subtree      = 1 + model->nodes.size(); // root + one entity per node
    const std::size_t rootChildren = r.Get<Hierarchy>(root).children.size();
    const std::size_t initial      = r.AliveCount();
    CHECK(initial == subtree + 1);

    // Selection outline covers the meshes below the selected root; picking target is on.
    editor.Select(root);
    runFrames(2);
    CHECK(sceneRenderer.overlay.picking && sceneRenderer.overlay.outlined.size() == 1);

    // Duplicate: subtree copied with new UUIDs and selected; undo / redo recreate the same copy.
    editor.DuplicateSelection();
    CHECK(r.AliveCount() == initial + subtree && editor.Selection().size() == 1 && editor.Selected() != root);
    const std::uint64_t copyUuid = r.Get<Uuid>(editor.Selected()).value;
    CHECK(copyUuid != rootUuid && editor.CanUndo() && editor.HasUnsavedChanges());
    CHECK(editor.Undo() && r.AliveCount() == initial && scene.FindByUuid(copyUuid) == NullEntity);
    CHECK(editor.Redo() && r.AliveCount() == initial + subtree && scene.FindByUuid(copyUuid) != NullEntity);
    CHECK(editor.Undo());

    // Multi-selection delete, undo restores both with their UUIDs and hierarchy.
    editor.Select(root);
    editor.ToggleSelection(light);
    CHECK(editor.Selection().size() == 2);
    editor.DeleteSelection();
    CHECK(r.AliveCount() == 0 && editor.Selection().empty());
    CHECK(editor.Undo() && r.AliveCount() == initial);
    const Entity root2 = scene.FindByUuid(rootUuid);
    CHECK(root2 != NullEntity && scene.FindByUuid(lightUuid) != NullEntity);
    CHECK(root2 != NullEntity && r.Get<Hierarchy>(root2).children.size() == rootChildren);
    CHECK(editor.Redo() && r.AliveCount() == 0);
    CHECK(editor.Undo() && r.AliveCount() == initial);
    runFrames(2); // restored entities render

    // Box selection: meshes whose bounds center and lights whose position project into the box.
    std::size_t meshes = 0;
    r.ViewOf<MeshRenderer>().Each([&](Entity, MeshRenderer&) { ++meshes; });
    editor.SelectInRect({0.0f, 0.0f}, {1e5f, 1e5f}, false);
    CHECK(meshes > 0 && editor.Selection().size() == meshes + 1);
    editor.SelectInRect({0.0f, 0.0f}, {2.0f, 2.0f}, true); // additive, empty corner: unchanged
    CHECK(editor.Selection().size() == meshes + 1);
    editor.SelectInRect({0.0f, 0.0f}, {2.0f, 2.0f}, false);
    CHECK(editor.Selection().empty());

    // Save, new scene, open: same content, clean history, model refs owned by the application.
    const fs::path file = fs::path(ENGINE_ASSET_DIR) / "test_editor.scene.json";
    CHECK(editor.SaveScene(file) && !editor.HasUnsavedChanges() && editor.ScenePath() == file);
    editor.NewScene();
    CHECK(r.AliveCount() == 0 && !editor.CanUndo() && editor.ScenePath().empty());
    CHECK(editor.OpenScene(file));
    CHECK(r.AliveCount() == initial && scene.FindByUuid(rootUuid) != NullEntity && !modelRefs.empty());
    CHECK(!editor.CanUndo() && !editor.HasUnsavedChanges());
    runFrames(3);
    fs::remove(file);

    // A broken file leaves the scene alone.
    const fs::path broken = fs::path(ENGINE_ASSET_DIR) / "test_editor_broken.scene.json";
    { std::ofstream(broken) << "{ not json"; }
    CHECK(!editor.OpenScene(broken) && r.AliveCount() == initial);
    fs::remove(broken);

    for (ModelHandle ref : modelRefs)
        F().assets->Release(ref);
    F().assets->Release(h);
}

TEST_CASE(Physics_MeshColliderAndEditorPlayStop)
{
    Scene        scene;
    Registry&    r = scene.GetRegistry();
    PhysicsWorld physics(*F().jobs, F().events, F().assets.get());

    // Mesh collider on a model that is still loading: pending until it is Ready.
    const ModelHandle h    = F().assets->LoadModel(kBox);
    const Entity      mesh = scene.CreateEntity("Mesh");
    r.Emplace<MeshRenderer>(mesh, MeshRenderer{.model = h, .meshIndex = 0});
    r.Emplace<RigidBody>(mesh, RigidBody{.type = BodyType::Dynamic}); // falls back to static
    r.Emplace<Collider>(mesh, Collider{.shape = ColliderShape::Mesh});
    scene.EditTransform(mesh).scale = {2.0f, 1.0f, 2.0f};
    physics.Sync(scene);
    CHECK(!physics.HasBody(mesh) && physics.Stats().pendingMeshes == 1);
    CHECK(F().Pump([&] { return Settled(h); }));
    const Model* model = F().assets->Get(h);
    CHECK(model != nullptr && !model->collisionPositions.empty());
    if (!model)
        return;
    physics.Sync(scene);
    CHECK(physics.HasBody(mesh) && physics.Activity(mesh) == BodyActivity::Static);

    // The ray hits the scaled triangles (top face of the box mesh).
    Aabb local{glm::vec3(1e30f), glm::vec3(-1e30f)};
    for (const Submesh& sm : model->meshes[0].submeshes) {
        local.min = glm::min(local.min, sm.boundsMin);
        local.max = glm::max(local.max, sm.boundsMax);
    }
    const Aabb world = TransformAabb(local, r.Get<WorldTransform>(mesh).matrix);
    const auto hit   = physics.Raycast({0.25f, 10.0f, 0.25f}, {0.0f, -1.0f, 0.0f}, 100.0f);
    CHECK(hit && hit->entity == mesh && std::abs(hit->point.y - world.max.y) < 1e-3f);
    const auto side = physics.Raycast({10.0f, 0.5f * (world.min.y + world.max.y), 0.0f}, {-1.0f, 0.0f, 0.0f}, 100.0f);
    CHECK(side && std::abs(side->point.x - world.max.x) < 1e-3f && glm::dot(side->normal, glm::vec3(1, 0, 0)) > 0.99f);

    // Editor Play / Pause / Step / Stop.
    const Entity ball = scene.CreateEntity("Ball");
    scene.EditTransform(ball).position = {0.0f, world.max.y + 3.0f, 0.0f};
    r.Emplace<RigidBody>(ball);
    Collider sphere;
    sphere.shape  = ColliderShape::Sphere;
    sphere.radius = 0.25f;
    r.Emplace<Collider>(ball, sphere);
    const std::uint64_t ballUuid = r.Get<Uuid>(ball).value;
    const std::uint64_t meshUuid = r.Get<Uuid>(mesh).value; // entities are recreated by Stop
    const glm::vec3     start    = scene.GetTransform(ball).position;

    SceneRenderer sceneRenderer(*F().renderer, *F().context, *F().assets);
    sceneRenderer.shadows.resolution = 512;
    FlyCamera                camera;
    std::vector<ModelHandle> modelRefs;
    Editor editor({.window        = *F().window,
                   .renderer      = *F().renderer,
                   .scene         = scene,
                   .assets        = *F().assets,
                   .sceneRenderer = sceneRenderer,
                   .camera        = camera,
                   .modelRefs     = modelRefs,
                   .physics       = &physics});
    camera.position = {0.0f, world.max.y + 2.0f, 6.0f};
    camera.LookAt({0.0f, world.max.y, 0.0f});
    const auto runFrames = [&](int count, int stepsPerFrame) {
        for (int i = 0; i < count; ++i) {
            F().window->PollEvents();
            F().events.Flush();
            for (int s = 0; s < stepsPerFrame; ++s)
                editor.FixedUpdate(1.0f / 60.0f);
            F().assets->Update();
            editor.Update(1.0f / 60.0f);
            if (auto frame = F().renderer->BeginFrame()) {
                editor.Render(*frame);
                F().renderer->EndFrame(*frame);
            }
        }
    };
    editor.Select(ball);
    runFrames(3, 4); // edit mode: FixedUpdate does not simulate, bodies follow the scene
    CHECK(scene.GetTransform(ball).position == start && physics.HasBody(ball));
    const bool couldUndo = editor.CanUndo();

    editor.Play();
    CHECK(editor.GetPlayState() == PlayState::Playing);
    runFrames(20, 6); // 2 s
    const Entity playBall = scene.FindByUuid(ballUuid);
    CHECK(playBall != NullEntity && std::abs(scene.GetTransform(playBall).position.y - (world.max.y + 0.25f)) < 0.03f);
    CHECK(editor.CanUndo() == couldUndo); // no history while playing
    CHECK(!editor.SaveScene(fs::path(ENGINE_ASSET_DIR) / "test_play.scene.json"));
    CHECK(!fs::exists(fs::path(ENGINE_ASSET_DIR) / "test_play.scene.json"));

    // Pause holds, Step advances exactly one fixed step.
    physics.AddImpulse(playBall, {0.0f, 3.0f, 0.0f});
    editor.Pause();
    const glm::vec3 paused = scene.GetTransform(playBall).position;
    runFrames(2, 3);
    CHECK(scene.GetTransform(playBall).position == paused);
    editor.StepOnce();
    runFrames(1, 3);
    CHECK(std::abs(scene.GetTransform(playBall).position.y - paused.y - 3.0f / 60.0f) < 0.02f);

    // Stop restores the scene; the selection follows the UUID.
    editor.Stop();
    CHECK(editor.GetPlayState() == PlayState::Edit);
    const Entity restored = scene.FindByUuid(ballUuid);
    CHECK(restored != NullEntity && scene.GetTransform(restored).position == start);
    CHECK(editor.Selected() == restored);
    runFrames(2, 2);
    CHECK(physics.HasBody(restored) && scene.GetTransform(restored).position == start);
    CHECK(physics.HasBody(scene.FindByUuid(meshUuid)));
    CHECK(scene.CountStaleTransforms() == 0);

    // A second session starts from the restored scene.
    editor.Play();
    runFrames(5, 6);
    CHECK(scene.GetTransform(scene.FindByUuid(ballUuid)).position != start);
    editor.Stop();
    CHECK(scene.GetTransform(scene.FindByUuid(ballUuid)).position == start);

    for (ModelHandle ref : modelRefs)
        F().assets->Release(ref);
    F().assets->Release(h);
}

namespace {
// Renders `frames` frames of `camera` into an offscreen RGBA8 target and returns the last one.
std::vector<std::uint8_t> RenderImage(SceneRenderer& renderer, Scene& scene, const CameraData& camera, int frames)
{
    constexpr VkExtent2D kExtent{160, 120};
    constexpr VkFormat   kFormat = VK_FORMAT_R8G8B8A8_UNORM;
    Image  target(*F().context, {.extent    = {kExtent.width, kExtent.height, 1},
                                 .format    = kFormat,
                                 .usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 .debugName = "TestTarget"});
    Buffer readback(*F().context, {.size      = VkDeviceSize{kExtent.width} * kExtent.height * 4,
                                   .usage     = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   .memory    = MemoryUsage::Readback,
                                   .debugName = "TestReadback"});
    for (int i = 0, attempts = 0; i < frames && attempts < frames * 10; ++attempts) {
        auto frame = F().renderer->BeginFrame();
        if (!frame)
            continue;
        ++i;
        CmdImageBarrier(frame->cmd, {.image     = target.Handle(),
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                                     .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, // previous frame (WAW)
                                     .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
        renderer.Render(*frame, scene, camera, {.image = target.Handle(), .view = target.View(), .format = kFormat, .extent = kExtent});
        if (i == frames) {
            CmdImageBarrier(frame->cmd, {.image     = target.Handle(),
                                         .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                         .dstStage  = VK_PIPELINE_STAGE_2_COPY_BIT,
                                         .dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT});
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent      = {kExtent.width, kExtent.height, 1};
            vkCmdCopyImageToBuffer(frame->cmd, target.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.Handle(), 1, &region);
        }
        F().renderer->EndFrame(*frame);
    }
    F().context->WaitIdle();
    readback.Invalidate(0, VK_WHOLE_SIZE);
    const auto* bytes = static_cast<const std::uint8_t*>(readback.Mapped());
    return {bytes, bytes + readback.Size()};
}
} // namespace

TEST_CASE(Render_GpuDrivenMatchesCpu)
{
    // Instanced boxes behind a wall (occlusion), a mirrored box (clockwise bucket), the spheres
    // model (many batches): the GPU-driven path must produce the CPU path's image.
    const ModelHandle box     = F().assets->CreatePrimitive({.shape = PrimitiveShape::Box, .size = 1.0f});
    const ModelHandle spheres = F().assets->LoadModel(kSpheres);
    CHECK(F().Pump([&] { return Settled(box) && Settled(spheres); }));
    const Model* spheresModel = F().assets->Get(spheres);
    CHECK(spheresModel != nullptr && F().assets->Get(box) != nullptr);
    if (!spheresModel)
        return;

    Scene     scene;
    Registry& r = scene.GetRegistry();
    const auto addBox = [&](glm::vec3 position, glm::vec3 scale) {
        const Entity e = scene.CreateEntity("Box");
        r.Emplace<MeshRenderer>(e, MeshRenderer{.model = box, .meshIndex = 0});
        scene.EditTransform(e).position = position;
        scene.EditTransform(e).scale    = scale;
        return e;
    };
    for (int x = 0; x < 6; ++x)
        for (int z = 0; z < 6; ++z)
            addBox({static_cast<float>(x) * 1.5f - 3.75f, 0.5f, -2.0f - static_cast<float>(z) * 1.5f}, glm::vec3(1.0f));
    addBox({0.0f, 2.0f, 1.0f}, {12.0f, 4.0f, 0.5f});  // wall hiding the grid
    addBox({-4.0f, 0.5f, 4.0f}, {-1.0f, 1.0f, 1.0f}); // mirrored
    const Entity sphereRoot = InstantiateModel(scene, spheres, *spheresModel);
    scene.EditTransform(sphereRoot).position = {5.0f, 1.0f, 3.0f};
    scene.EditTransform(sphereRoot).scale    = glm::vec3(0.3f);
    scene.UpdateTransforms();

    SceneRenderer renderer(*F().renderer, *F().context, *F().assets);
    renderer.shadows.resolution = 512;
    renderer.post.autoExposure  = false; // adaptation depends on wall-clock frame times
    const glm::vec3  eye{0.0f, 2.0f, 12.0f};
    const CameraData camera{.view       = glm::lookAt(eye, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                            .projection = PerspectiveReverseZ(glm::radians(60.0f), 160.0f / 120.0f, 0.05f),
                            .position   = eye};

    renderer.culling.gpuDriven       = false;
    const std::vector<std::uint8_t> cpu = RenderImage(renderer, scene, camera, 3);
    const SceneRenderStats          cpuStats = renderer.Stats();
    renderer.culling.gpuDriven       = true;
    // Frame 1 draws everything late (no history), frame 2 early; counters lag two frames.
    const std::vector<std::uint8_t> gpu = RenderImage(renderer, scene, camera, 6);
    const SceneRenderStats          stats = renderer.Stats();

    std::size_t differing = 0;
    for (std::size_t i = 0; i < cpu.size(); ++i)
        differing += std::abs(static_cast<int>(cpu[i]) - static_cast<int>(gpu[i])) > 2 ? 1u : 0u;
    CHECK(differing == 0);
    CHECK(cpuStats.drawCalls > 0 && !cpuStats.gpuDriven);

    // Steady state (counters are two frames old): everything tested, the grid behind the wall
    // occluded, nothing newly visible, instances of one submesh batched.
    const auto meshNodes = static_cast<std::uint32_t>(
        std::ranges::count_if(spheresModel->nodes, [](const ModelNode& n) { return n.mesh >= 0; }));
    CHECK(stats.gpuDriven && stats.instances == 38 + meshNodes);
    CHECK(stats.gpuTested == stats.draws);
    CHECK(stats.gpuOccluded >= 18 && stats.gpuLate == 0);
    CHECK(stats.gpuEarly + stats.gpuOccluded + stats.gpuFrustumCulled == stats.gpuTested);
    CHECK(stats.batches < stats.draws && stats.gpuCommands > 0 && stats.drawCalls > 0);

    // Without occlusion culling everything in the frustum is drawn early.
    renderer.culling.occlusion = false;
    (void)RenderImage(renderer, scene, camera, 3);
    CHECK(renderer.Stats().gpuOccluded == 0 && renderer.Stats().gpuEarly + renderer.Stats().gpuFrustumCulled == stats.draws);
    renderer.culling.occlusion = true;

    // Frozen culling keeps the visible set of the freeze camera while looking elsewhere.
    renderer.culling.freeze = true;
    (void)RenderImage(renderer, scene, camera, 3);
    const std::uint32_t frozenEarly = renderer.Stats().gpuEarly;
    const CameraData    away{.view       = glm::lookAt(eye, eye + glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                             .projection = camera.projection,
                             .position   = eye};
    (void)RenderImage(renderer, scene, away, 3);
    CHECK(frozenEarly > 0 && renderer.Stats().gpuEarly == frozenEarly);
    renderer.culling.freeze = false;
    (void)RenderImage(renderer, scene, away, 3);
    CHECK(renderer.Stats().gpuEarly == 0 && renderer.Stats().gpuLate == 0 && renderer.Stats().gpuFrustumCulled == stats.draws);

    // Debug views of the GPU path.
    for (DebugView view : {DebugView::HiZ, DebugView::Culling}) {
        renderer.post.debugView = view;
        (void)RenderImage(renderer, scene, camera, 2);
    }
    renderer.post.debugView = DebugView::None;

    // A released model's instances leave the GPU scene before its geometry is freed.
    const std::uint32_t before = renderer.Stats().instances;
    F().assets->Release(spheres);
    (void)RenderImage(renderer, scene, camera, 3);
    CHECK(renderer.Stats().instances == 38 && renderer.Stats().instances < before);
    CHECK(renderer.Spatial().MeshCount() == 38); // released: the BVH drops its meshes too
    F().assets->Release(box);
}

namespace {
std::vector<std::byte> EncodePng(const std::vector<std::uint8_t>& rgba, int w, int h)
{
    std::vector<std::byte> out;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* bytes = static_cast<std::vector<std::byte>*>(context);
            bytes->insert(bytes->end(), static_cast<std::byte*>(data), static_cast<std::byte*>(data) + size);
        },
        &out, w, h, 4, rgba.data(), w * 4);
    return out;
}

std::vector<std::byte> SolidPng(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    std::vector<std::uint8_t> pixels;
    for (int i = 0; i < 16 * 16; ++i)
        pixels.insert(pixels.end(), {r, g, b, 255});
    return EncodePng(pixels, 16, 16);
}

void WriteFile(const fs::path& path, std::string_view text, int ageSeconds = 0)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(text.data(), static_cast<std::streamsize>(text.size()));
    // Pushed forward: a change must stay visible within the file system's timestamp resolution.
    if (ageSeconds != 0)
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(ageSeconds));
}

void WriteFile(const fs::path& path, const std::vector<std::byte>& bytes, int ageSeconds = 0)
{
    WriteFile(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), ageSeconds);
}

std::string Base64(const std::vector<std::uint8_t>& bytes)
{
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t n = (std::uint32_t{bytes[i]} << 16) |
                                (i + 1 < bytes.size() ? std::uint32_t{bytes[i + 1]} << 8 : 0u) |
                                (i + 2 < bytes.size() ? std::uint32_t{bytes[i + 2]} : 0u);
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? kTable[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kTable[n & 63] : '=';
    }
    return out;
}

// Square of half-size `s` in the XY plane facing +Z; base color from `image` (none if empty).
std::string QuadGltf(float s, const std::string& image)
{
    std::vector<std::uint8_t> buffer;
    const auto put = [&](const void* data, std::size_t bytes) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        buffer.insert(buffer.end(), p, p + bytes);
    };
    const float         positions[] = {-s, -s, 0.0f, s, -s, 0.0f, s, s, 0.0f, -s, s, 0.0f};
    const float         normals[]   = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const float         uvs[]       = {0, 1, 1, 1, 1, 0, 0, 0};
    const std::uint16_t indices[]   = {0, 1, 2, 0, 2, 3};
    put(positions, sizeof(positions));
    put(normals, sizeof(normals));
    put(uvs, sizeof(uvs));
    put(indices, sizeof(indices));
    const std::string material = image.empty()
                                     ? R"({"pbrMetallicRoughness": {"metallicFactor": 0.0}})"
                                     : R"({"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0}})";
    const std::string textures =
        image.empty() ? std::string{} : std::format(R"("images": [{{"uri": "{}"}}], "textures": [{{"source": 0}}],)", image);
    return std::format(R"({{
  "asset": {{"version": "2.0"}}, "scene": 0, "scenes": [{{"nodes": [0]}}], "nodes": [{{"mesh": 0}}],
  "meshes": [{{"primitives": [{{"attributes": {{"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}}, "indices": 3, "material": 0}}]}}],
  "materials": [{}], {}
  "buffers": [{{"byteLength": {}, "uri": "data:application/octet-stream;base64,{}"}}],
  "bufferViews": [{{"buffer": 0, "byteOffset": 0, "byteLength": 48}}, {{"buffer": 0, "byteOffset": 48, "byteLength": 48}},
                  {{"buffer": 0, "byteOffset": 96, "byteLength": 32}}, {{"buffer": 0, "byteOffset": 128, "byteLength": 12}}],
  "accessors": [
    {{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [{}, {}, 0], "max": [{}, {}, 0]}},
    {{"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"}},
    {{"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"}},
    {{"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"}}]
}})",
                       material, textures, buffer.size(), Base64(buffer), -s, -s, s, s);
}

CameraData FrontCamera(float distance)
{
    const glm::vec3 eye{0.0f, 0.0f, distance};
    return {.view       = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
            .projection = PerspectiveReverseZ(glm::radians(60.0f), 160.0f / 120.0f, 0.05f),
            .position   = eye};
}

glm::ivec3 CenterPixel(const std::vector<std::uint8_t>& image)
{
    const std::size_t i = (60 * 160 + 80) * 4;
    return {image[i], image[i + 1], image[i + 2]};
}
} // namespace

TEST_CASE(Asset_TexturesSharedCompressedAndHotReloaded)
{
    const fs::path dir = fs::temp_directory_path() / "ungine_asset_reload_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    WriteFile(dir / "albedo.png", SolidPng(255, 0, 0));
    WriteFile(dir / "quad.gltf", QuadGltf(1.0f, "albedo.png"));

    AssetManagerDesc desc;
    desc.textures.cacheDirectory = dir / "cache";
    desc.hotReload               = true;
    desc.pollSeconds             = 0.0;
    std::vector<TextureHandle> textureReloads;
    std::vector<ModelHandle>   modelReloads;
    Subscription s1 = F().events.Subscribe<AssetReloadedEvent<Texture>>([&](const auto& e) { textureReloads.push_back(e.handle); });
    Subscription s2 = F().events.Subscribe<AssetReloadedEvent<Model>>([&](const auto& e) { modelReloads.push_back(e.handle); });
    {
        AssetManager assets(*F().renderer, *F().jobs, F().events, desc);
        const auto   pump = [&](const std::function<bool()>& done) {
            return F().Pump([&] {
                assets.Update();
                return done();
            });
        };

        // The model's external image and the same file loaded directly are one texture asset.
        const ModelHandle quad = assets.LoadModel(dir / "quad.gltf");
        CHECK(pump([&] { return assets.State(quad) == AssetState::Ready; }));
        const Model* model = assets.Get(quad);
        CHECK(model && model->textures.size() == 1);
        if (!model || model->textures.empty())
            return;
        const TextureHandle albedo = model->textures[0];
        CHECK(assets.LoadTexture(dir / "albedo.png", TextureKind::Color) == albedo && assets.RefCount(albedo) == 2);
        assets.Release(albedo);
        const Texture* texture = assets.Get(albedo);
        const VkFormat format  = F().context->SupportsBC() ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_R8G8B8A8_SRGB;
        CHECK(texture && texture->format == format && texture->mipLevels == 5 && !texture->cacheHit);
        const std::uint32_t entry = assets.TableEntry(albedo);
        CHECK(texture && entry >= static_cast<std::uint32_t>(DefaultTexture::Count) &&
              F().renderer->TextureEntry(entry) == texture->bindlessSlot);

        Scene scene;
        const Entity root = InstantiateModel(scene, quad, *model);
        scene.UpdateTransforms();
        const Entity  node = scene.GetRegistry().Get<Hierarchy>(root).children.at(0);
        SceneRenderer renderer(*F().renderer, *F().context, assets);
        renderer.post.autoExposure = false;
        glm::ivec3 center = CenterPixel(RenderImage(renderer, scene, FrontCamera(3.0f), 3));
        CHECK(center.r > center.g + 40 && center.r > center.b + 40); // red

        // Texture hot reload: new image under the same table entry; the model is untouched.
        const std::uint32_t modelRevision = assets.Revision(quad), textureRevision = assets.Revision(albedo);
        WriteFile(dir / "albedo.png", SolidPng(0, 255, 0), 5);
        CHECK(pump([&] { return assets.Revision(albedo) != textureRevision; }));
        CHECK(assets.State(albedo) == AssetState::Ready && assets.TableEntry(albedo) == entry);
        CHECK(textureReloads.size() == 1 && textureReloads[0] == albedo && assets.Revision(quad) == modelRevision);
        CHECK(assets.Get(albedo) && F().renderer->TextureEntry(entry) == assets.Get(albedo)->bindlessSlot);
        center = CenterPixel(RenderImage(renderer, scene, FrontCamera(3.0f), 3));
        CHECK(center.g > center.r + 40 && center.g > center.b + 40); // green

        // Model hot reload: a larger quad; the texture asset carries over to the new version.
        WriteFile(dir / "quad.gltf", QuadGltf(2.0f, "albedo.png"), 5);
        CHECK(pump([&] { return assets.Revision(quad) != modelRevision; }));
        model = assets.Get(quad);
        CHECK(model && model->boundsMax.x == 2.0f && model->textures.size() == 1 && model->textures[0] == albedo);
        CHECK(modelReloads.size() == 1 && modelReloads[0] == quad && assets.RefCount(albedo) == 1);
        (void)RenderImage(renderer, scene, FrontCamera(3.0f), 3);
        const auto bounds = renderer.Spatial().Bounds(node);
        CHECK(bounds && std::abs(bounds->max.x - 2.0f) < 1e-4f);
        CHECK(renderer.Stats().instances == 1);

        // A broken file keeps the loaded version and reports the error.
        F().failed.clear();
        WriteFile(dir / "quad.gltf", std::string_view("{ broken"), 10);
        CHECK(pump([&] { return !assets.Error(quad).empty(); }));
        CHECK(assets.State(quad) == AssetState::Ready && assets.Get(quad) == model);
        CHECK(F().failed.size() == 1 && F().failed[0] == quad);

        // A missing image: the model loads, the texture fails (placeholder) and recovers by
        // itself once the file appears (hot reload polls missing files too).
        WriteFile(dir / "quad.gltf", QuadGltf(1.0f, "late.png"), 15);
        CHECK(pump([&] { return assets.Get(quad) && assets.Get(quad)->boundsMax.x == 1.0f; }));
        const TextureHandle late = assets.Get(quad)->textures.at(0);
        CHECK(assets.State(late) == AssetState::Failed && assets.State(albedo) == AssetState::Invalid);
        CHECK(F().renderer->TextureEntry(assets.TableEntry(late)) ==
              F().renderer->DefaultTextureIndex(DefaultTexture::Error));
        center = CenterPixel(RenderImage(renderer, scene, FrontCamera(3.0f), 3));
        WriteFile(dir / "late.png", SolidPng(0, 0, 255));
        CHECK(pump([&] { return assets.State(late) == AssetState::Ready; }));
        center = CenterPixel(RenderImage(renderer, scene, FrontCamera(3.0f), 3));
        CHECK(center.b > center.r + 40 && center.b > center.g + 40); // blue
        CHECK(F().renderer->TextureEntry(assets.TableEntry(late)) == assets.Get(late)->bindlessSlot);
        assets.Release(quad);
    }

    // The cooked images are cached: a new manager does not encode them again.
    {
        AssetManager        assets(*F().renderer, *F().jobs, F().events, desc);
        const TextureHandle t = assets.LoadTexture(dir / "late.png", TextureKind::Color);
        CHECK(F().Pump([&] {
            assets.Update();
            return assets.State(t) == AssetState::Ready;
        }));
        CHECK(assets.Get(t) && assets.Get(t)->cacheHit);
        assets.Release(t);
    }
    F().Pump([] { return false; }, 4);
    fs::remove_all(dir);
}

TEST_CASE(Asset_FailedModelPlaceholderAndRetry)
{
    const fs::path dir = fs::temp_directory_path() / "ungine_asset_retry_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path path = dir / "late.gltf";

    const ModelHandle h = F().assets->LoadModel(path);
    CHECK(F().Pump([&] { return Settled(h); }) && F().assets->State(h) == AssetState::Failed);
    const ResolvedMesh placeholder = F().assets->ResolveMesh(h, 3);
    CHECK(placeholder.placeholder && placeholder.model && placeholder.meshIndex == 0 && !F().assets->Get(h));
    const std::uint32_t failedRevision = F().assets->Revision(h);
    CHECK(failedRevision > 0);

    Scene        scene;
    const Entity e = scene.CreateEntity("Missing");
    scene.GetRegistry().Emplace<MeshRenderer>(e, MeshRenderer{.model = h, .meshIndex = 0});
    scene.UpdateTransforms();
    SceneRenderer renderer(*F().renderer, *F().context, *F().assets);
    renderer.post.autoExposure = false;
    glm::ivec3 center = CenterPixel(RenderImage(renderer, scene, FrontCamera(3.0f), 3));
    CHECK(renderer.Stats().instances == 1 && renderer.Spatial().MeshCount() == 1); // the placeholder box
    CHECK(center.r > center.g + 20 && center.b > center.g + 20);                  // magenta checker

    // Retry once the file exists: the real model replaces the placeholder.
    WriteFile(path, QuadGltf(0.25f, ""));
    F().loaded.clear();
    CHECK(F().assets->Reload(h));
    CHECK(F().Pump([&] { return F().assets->State(h) == AssetState::Ready; }));
    CHECK(F().loaded.size() == 1 && F().loaded[0] == h && F().assets->Revision(h) != failedRevision);
    CHECK(!F().assets->ResolveMesh(h, 0).placeholder && F().assets->Error(h).empty());
    (void)RenderImage(renderer, scene, FrontCamera(3.0f), 3);
    const auto bounds = renderer.Spatial().Bounds(e);
    CHECK(bounds && std::abs(bounds->max.x - 0.25f) < 1e-4f && renderer.Stats().instances == 1);
    // Generated models have no source to reload.
    const ModelHandle generated = F().assets->CreateModel(MakeBox("Box", 1.0f, MaterialData{}));
    CHECK(!F().assets->Reload(generated) && F().assets->Reload(h));
    CHECK(F().Pump([&] { return Settled(generated) && Settled(h) && F().assets->Models().size() >= 2; }));
    F().assets->Release(generated);
    F().assets->Release(h);
    F().Pump([] { return false; }, 4);
    fs::remove_all(dir);
}

TEST_CASE(Render_MeshLodSelection)
{
    const ModelHandle h = F().assets->CreateModel(
        MakeCapsule("LodBall", 1.0f, 0.0f, MaterialData{.name = "Ball", .metallic = 0.0f, .roughness = 0.5f}, 128, 64));
    CHECK(F().Pump([&] { return Settled(h); }));
    const Model* model = F().assets->Get(h);
    CHECK(model != nullptr);
    if (!model)
        return;
    const Submesh& sm = model->meshes[0].submeshes[0];
    CHECK(sm.lodCount == kMaxLods);

    Scene        scene;
    const Entity e = scene.CreateEntity("Ball");
    scene.GetRegistry().Emplace<MeshRenderer>(e, MeshRenderer{.model = h, .meshIndex = 0});
    scene.UpdateTransforms();
    SceneRenderer renderer(*F().renderer, *F().context, *F().assets);
    renderer.post.autoExposure = false;
    renderer.shadows.resolution = 512;

    struct Result {
        std::uint32_t lodDraws  = 0;
        std::uint64_t triangles = 0;
    };
    const auto measure = [&](bool gpu, float distance) { // GPU counters: two frames old
        renderer.culling.gpuDriven = gpu;
        (void)RenderImage(renderer, scene, FrontCamera(distance), gpu ? 5 : 1);
        return Result{renderer.Stats().lodDraws, renderer.Stats().triangles};
    };
    const std::uint64_t full = sm.indexCount / 3, coarsest = sm.lods[sm.lodCount - 1].indexCount / 3;
    const Result nearGpu = measure(true, 1.75f), farGpu = measure(true, 400.0f);
    const Result nearCpu = measure(false, 1.75f), farCpu = measure(false, 400.0f);
    CHECK(nearGpu.lodDraws == 0 && nearGpu.triangles == full);
    CHECK(farGpu.lodDraws == 1 && farGpu.triangles == coarsest && coarsest < full / 4);
    CHECK(nearCpu.lodDraws == 0 && nearCpu.triangles == full);
    CHECK(farCpu.lodDraws == 1 && farCpu.triangles == coarsest);

    // Inside the fade band before LOD 1 takes over: both levels drawn (complementary dithering).
    const float w      = 0.5f * 120.0f / std::tan(glm::radians(30.0f)); // pixels per unit at distance 1, 1 px
    const float inBand = sm.lods[1].error * w * 0.9f + std::sqrt(3.0f);   // + box radius
    const std::uint64_t both = full + sm.lods[1].indexCount / 3;
    CHECK(measure(true, inBand).triangles == both && measure(false, inBand).triangles == both);
    CHECK(renderer.Stats().drawCalls == 2 && renderer.Stats().lodDraws == 2);

    renderer.culling.forceLod = 2;
    CHECK(measure(true, 1.75f).triangles == sm.lods[2].indexCount / 3);
    CHECK(measure(false, 1.75f).triangles == sm.lods[2].indexCount / 3);
    renderer.culling.forceLod = -1;
    renderer.culling.lod      = false;
    CHECK(measure(true, 400.0f).triangles == full && measure(false, 400.0f).lodDraws == 0);
    renderer.culling.lod    = true;
    renderer.post.debugView = DebugView::Lod;
    (void)measure(true, 20.0f);
    (void)measure(false, 20.0f);
    F().assets->Release(h);
}

TEST_CASE(Render_ShaderReloadRebuildsPipelines)
{
    const ModelHandle box = F().assets->CreatePrimitive({.shape = PrimitiveShape::Box, .size = 1.0f});
    CHECK(F().Pump([&] { return Settled(box); }));
    Scene        scene;
    const Entity e = scene.CreateEntity("Box");
    scene.GetRegistry().Emplace<MeshRenderer>(e, MeshRenderer{.model = box, .meshIndex = 0});
    scene.UpdateTransforms();
    SceneRenderer renderer(*F().renderer, *F().context, *F().assets);
    renderer.post.autoExposure = false;
    const std::vector<std::uint8_t> before = RenderImage(renderer, scene, FrontCamera(3.0f), 3);

    // Every pipeline (scene, IBL, GPU scene, culling) rebuilt from the same SPIR-V: same image.
    const std::uint64_t generation = F().renderer->ShaderGeneration();
    F().renderer->NotifyShadersChanged();
    const std::vector<std::uint8_t> after = RenderImage(renderer, scene, FrontCamera(3.0f), 3);
    std::size_t differing = 0;
    for (std::size_t i = 0; i < before.size(); ++i)
        differing += std::abs(static_cast<int>(before[i]) - static_cast<int>(after[i])) > 2 ? 1u : 0u;
    CHECK(differing == 0 && F().renderer->ShaderGeneration() == generation + 1);

    // A real recompile (same source) goes through the watcher and bumps the generation.
    F().renderer->SetShaderHotReload(true);
    if (ShaderHotReload* reload = F().renderer->ShaderReloader(); reload && reload->Available()) {
        CHECK(reload->Recompile(ShaderPath("sky.frag.spv")));
        (void)RenderImage(renderer, scene, FrontCamera(3.0f), 2);
        CHECK(F().renderer->ShaderGeneration() == generation + 2);
    }
    F().renderer->SetShaderHotReload(false);
    F().assets->Release(box);
}

TEST_CASE(Upload_RingBudgetAndLargeUploads)
{
    // Small ring + budget: uploads wrap / overflow into own staging buffers and batches are
    // held back per Submit(); the data must arrive intact either way.
    constexpr VkDeviceSize kRing = 1u << 20, kBudget = 256u << 10;
    UploadQueue            queue(*F().context, {.stagingRingSize = kRing, .frameBudget = kBudget});
    std::mt19937           rng(7);
    std::vector<std::vector<std::byte>> contents;
    std::vector<Buffer>                 buffers;
    UploadTicket                        ticket = 0;
    for (const std::size_t size : {200u << 10, 200u << 10, 200u << 10, 700u << 10, 200u << 10, 200u << 10, 200u << 10,
                                   200u << 10, 64u, 3u}) {
        std::vector<std::byte> data(size);
        for (std::byte& b : data)
            b = static_cast<std::byte>(rng());
        buffers.push_back(queue.CreateBuffer(data, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, ticket));
        contents.push_back(std::move(data));
    }
    const UploadStats recorded = queue.Stats();
    CHECK(recorded.ringCapacity == kRing && recorded.ringUsed > 0 && recorded.ringUsed <= kRing);
    CHECK(recorded.dedicatedStaging >= 2); // the large one + what no longer fit the ring
    CHECK(recorded.queuedBatches >= 8);    // one 200 KB upload per batch (budget 256 KB)
    queue.Submit();
    const UploadStats submitted = queue.Stats();
    CHECK(submitted.submittedLastFrame > 0 && submitted.submittedLastFrame <= kBudget);
    CHECK(submitted.queuedBatches == recorded.queuedBatches - 1 && !queue.IsReady(ticket));

    queue.Flush();
    CHECK(queue.IsReady(ticket) && queue.Stats().ringUsed == 0 && queue.Stats().queuedBatches == 0);
    VkDeviceSize total = 0;
    for (const auto& c : contents)
        total += c.size();
    Buffer readback(*F().context, {.size = total, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   .memory = MemoryUsage::Readback, .debugName = "UploadReadback"});
    queue.ImmediateSubmit([&](VkCommandBuffer cmd) {
        VkDeviceSize offset = 0;
        for (std::size_t i = 0; i < buffers.size(); ++i) {
            const VkBufferCopy region{0, offset, contents[i].size()};
            vkCmdCopyBuffer(cmd, buffers[i].Handle(), readback.Handle(), 1, &region);
            offset += contents[i].size();
        }
    });
    readback.Invalidate(0, VK_WHOLE_SIZE);
    const auto*  bytes  = static_cast<const std::byte*>(readback.Mapped());
    bool         intact = true;
    VkDeviceSize offset = 0;
    for (const auto& c : contents) {
        intact = intact && std::memcmp(bytes + offset, c.data(), c.size()) == 0;
        offset += c.size();
    }
    CHECK(intact);
    F().context->WaitIdle();
}

TEST_CASE(Renderer_ResizeWithoutDeviceWait)
{
    // With swapchain maintenance the old swapchain is retired (no device wait) and destroyed
    // once its presents are done; either way the resize must be clean under validation.
    const VkExtent2D before = F().renderer->GetSwapchain().Extent();
    for (const auto& [w, h] : {std::pair{400, 300}, std::pair{256, 200}, std::pair{static_cast<int>(before.width), static_cast<int>(before.height)}}) {
        glfwSetWindowSize(F().window->Native(), w, h);
        CHECK(F().Pump([&] {
            const VkExtent2D e = F().renderer->GetSwapchain().Extent();
            return e.width == static_cast<std::uint32_t>(w) && e.height == static_cast<std::uint32_t>(h);
        }, 500));
    }
    CHECK(F().Pump([] { return F().renderer->RetiredSwapchains() == 0; }, 50));
}

TEST_CASE(Render_TransparentAndShadowOcclusion)
{
    const ModelHandle box   = F().assets->CreatePrimitive({.shape = PrimitiveShape::Box, .size = 1.0f});
    const ModelHandle glass = F().assets->CreateModel(MakeBox(
        "Glass", 1.0f, MaterialData{.name = "Glass", .baseColorFactor = {0.1f, 0.3f, 1.0f, 0.4f}, .alphaBlend = true}));
    CHECK(F().Pump([&] { return Settled(box) && Settled(glass); }));

    Scene     scene;
    Registry& r   = scene.GetRegistry();
    const auto add = [&](ModelHandle model, glm::vec3 position, glm::vec3 scale) {
        const Entity e = scene.CreateEntity("E");
        r.Emplace<MeshRenderer>(e, MeshRenderer{.model = model, .meshIndex = 0});
        scene.EditTransform(e).position = position;
        scene.EditTransform(e).scale    = scale;
        return e;
    };
    add(box, {0.0f, -0.5f, 0.0f}, {40.0f, 1.0f, 40.0f});  // ground
    add(box, {0.0f, 5.0f, -2.0f}, {60.0f, 20.0f, 0.5f});  // wall filling the view (no sky in the Hi-Z)
    add(box, {0.0f, 0.5f, -8.0f}, {1.0f, 1.0f, 1.0f});    // caster behind the wall, shadow straight down
    add(glass, {0.0f, 1.0f, 2.0f}, {1.0f, 1.0f, 1.0f});   // transparent box in front of the wall
    scene.UpdateTransforms();

    SceneRenderer renderer(*F().renderer, *F().context, *F().assets);
    renderer.post.autoExposure       = false;

    renderer.shadows.resolution      = 512;
    renderer.shadows.maxDistance     = 20.0f;
    renderer.lighting.sky.sunDirection = glm::normalize(glm::vec3(0.01f, -1.0f, 0.02f));
    const glm::vec3  eye{0.0f, 1.0f, 8.0f};
    const CameraData camera{.view       = glm::lookAt(eye, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                            .projection = PerspectiveReverseZ(glm::radians(60.0f), 160.0f / 120.0f, 0.05f),
                            .position   = eye};

    // Transparent: blended over the wall, sorted CPU pass on both paths, never an indirect command.
    renderer.culling.gpuDriven          = false;
    const std::vector<std::uint8_t> cpu = RenderImage(renderer, scene, camera, 3);
    CHECK(renderer.Stats().transparentDraws == 1);
    renderer.culling.gpuDriven          = true;
    const std::vector<std::uint8_t> gpu = RenderImage(renderer, scene, camera, 6);
    const SceneRenderStats          stats = renderer.Stats();
    CHECK(stats.transparentDraws == 1);
    std::size_t differing = 0;
    for (std::size_t i = 0; i < cpu.size(); ++i)
        differing += std::abs(static_cast<int>(cpu[i]) - static_cast<int>(gpu[i])) > 2 ? 1u : 0u;
    CHECK(differing == 0);
    const glm::ivec3 center = CenterPixel(gpu);
    CHECK(center.b > center.r + 20); // blue glass over the grey wall

    // The hidden caster's shadow falls behind the wall: culled against the Hi-Z; the image is the
    // same without that culling.
    CHECK(stats.shadowOccluded >= 1);
    renderer.culling.shadowOcclusion = false;
    const std::vector<std::uint8_t> reference = RenderImage(renderer, scene, camera, 6);
    CHECK(renderer.Stats().shadowOccluded == 0);
    differing = 0;
    for (std::size_t i = 0; i < gpu.size(); ++i)
        differing += std::abs(static_cast<int>(reference[i]) - static_cast<int>(gpu[i])) > 2 ? 1u : 0u;
    CHECK(differing == 0);
    F().assets->Release(glass);
    F().assets->Release(box);
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

TEST_CASE(Editor_BlueprintPlayAndGraphEditing)
{
    Scene        scene;
    Registry&    r = scene.GetRegistry();
    PhysicsWorld physics(*F().jobs, F().events, F().assets.get());
    ScriptSystem scripts(F().events, nullptr, &physics, F().assets.get());
    const Entity actor = scene.CreateEntity("Spawner");
    scene.EditTransform(actor).position = {0.0f, 1.0f, 0.0f};

    SceneRenderer sceneRenderer(*F().renderer, *F().context, *F().assets);
    sceneRenderer.shadows.resolution = 512;
    FlyCamera                camera;
    std::vector<ModelHandle> modelRefs;
    Editor editor({.window        = *F().window,
                   .renderer      = *F().renderer,
                   .scene         = scene,
                   .assets        = *F().assets,
                   .sceneRenderer = sceneRenderer,
                   .camera        = camera,
                   .modelRefs     = modelRefs,
                   .physics       = &physics,
                   .scripts       = &scripts});
    camera.position = {0.0f, 2.0f, 8.0f};
    const auto runFrames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            F().window->PollEvents();
            F().events.Flush();
            editor.FixedUpdate(1.0f / 60.0f);
            F().assets->Update();
            editor.Update(1.0f / 60.0f);
            if (auto frame = F().renderer->BeginFrame()) {
                editor.Render(*frame, 0.5f);
                F().renderer->EndFrame(*frame);
            }
        }
    };
    const auto countNamed = [&](const std::string& name) {
        int n = 0;
        r.ViewOf<Name>().Each([&](Entity, Name& e) { n += e.value == name ? 1 : 0; });
        return n;
    };

    // A new blueprint from the template (BeginPlay -> Print), extended: For 1..3 spawn a sphere at (0, i, 0).
    const fs::path     file = fs::path(ENGINE_ASSET_DIR) / "test_blueprint.ugraph";
    ScriptGraphEditor& bp   = editor.Blueprints();
    CHECK(bp.New(file) && bp.Count() == 1 && !bp.Dirty() && bp.Graph() && bp.Graph()->nodes.size() == 3);
    if (!bp.Graph())
        return;
    std::uint32_t print = 0, spawn = 0;
    for (const ScriptNode& n : bp.Graph()->nodes)
        if (n.type == "Debug.Print")
            print = n.id;
    bp.Edit("Build", [&](ScriptGraph& g) {
        const std::uint32_t loop = g.AddNode("Flow.ForLoop", {200.0f, 200.0f});
        const std::uint32_t make = g.AddNode("Vector.Make", {200.0f, 360.0f});
        spawn                    = g.AddNode("Entity.SpawnPrimitive", {500.0f, 200.0f}, "sphere");
        g.FindNode(loop)->defaults["First Index"] = std::int32_t{1};
        g.FindNode(loop)->defaults["Last Index"]  = std::int32_t{3};
        g.FindNode(spawn)->defaults["Simulate Physics"] = false;
        CHECK(g.Connect(print, "Then", loop, "In").empty());
        CHECK(g.Connect(loop, "Loop Body", spawn, "In").empty());
        CHECK(g.Connect(loop, "Index", make, "Y").empty());
        CHECK(g.Connect(make, "Vector", spawn, "Location").empty());
    });
    CHECK(bp.Dirty() && bp.CanUndo() && bp.Graph()->nodes.size() == 6);
    CHECK(bp.Undo() && bp.Graph()->nodes.size() == 3 && !bp.Dirty()); // back at the saved state
    CHECK(bp.Redo() && bp.Graph()->nodes.size() == 6 && bp.Dirty());
    bp.Select({spawn});
    bp.DuplicateSelection();
    CHECK(bp.Graph()->nodes.size() == 7 && bp.SelectedNodes().size() == 1 && bp.SelectedNodes()[0] != spawn);
    bp.DeleteSelection();
    CHECK(bp.Graph()->nodes.size() == 6);
    CHECK(std::ranges::none_of(bp.Diagnostics(), [](const ScriptDiagnostic& d) { return d.error; }));

    // Play runs the unsaved graph: three spheres; Stop removes them again.
    r.Emplace<ScriptComponent>(actor, ScriptComponent{file.string()});
    const std::uint64_t actorUuid = r.Get<Uuid>(actor).value; // Stop recreates the entities
    runFrames(2);
    editor.Play();
    runFrames(6);
    CHECK(scripts.Running() && countNamed("sphere") == 3);
    const auto messages = scripts.Messages();
    CHECK(std::ranges::any_of(messages, [](const ScriptMessage& m) { return m.text == "Hello from test_blueprint"; }));
    const ScriptDebugInfo* info = scripts.Debug(file);
    CHECK(info && info->nodeTimes.contains(spawn));
    bp.Focus(); // the Blueprint tab in front: canvas with execution highlight, viewport hidden
    runFrames(3);
    editor.Stop();
    const Entity restored = scene.FindByUuid(actorUuid);
    CHECK(!scripts.Running() && countNamed("sphere") == 0 && restored != NullEntity && r.Has<ScriptComponent>(restored));

    // Save, close, reopen.
    CHECK(bp.Save() && !bp.Dirty());
    bp.Close(0);
    CHECK(bp.Count() == 0 && bp.Open(file) && bp.Graph()->nodes.size() == 6);
    runFrames(2);
    bp.Close(0);
    fs::remove(file);
}

TEST_CASE(Editor_ProjectLauncherAndContent)
{
    const fs::path root = fs::temp_directory_path() / "ungine_tests" / "gpu_project";
    std::error_code ec;
    fs::remove_all(root, ec);
#ifndef _WIN32
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1); // the test's own recent-projects list
#endif
    // Project browser: a new project from the physics template.
    std::optional<fs::path> chosen;
    {
        ProjectLauncher launcher(*F().window, *F().renderer);
        const auto&     templates = launcher.Templates();
        const auto      physics   = std::ranges::find_if(templates, [](const ProjectTemplate& t) { return t.id == "Physics"; });
        CHECK(templates.size() >= 3 && physics != templates.end());
        for (int i = 0; i < 2; ++i) {
            F().window->PollEvents();
            launcher.Update();
            if (auto frame = F().renderer->BeginFrame()) {
                launcher.Render(*frame);
                F().renderer->EndFrame(*frame);
            }
        }
        CHECK(!launcher.CreateProject("bad/name", root, 0) && !launcher.Error().empty());
        CHECK(launcher.CreateProject("LauncherGame", root, static_cast<std::size_t>(physics - templates.begin())));
        chosen = launcher.Chosen();
        F().renderer->GetContext().WaitIdle(); // the launcher's ImGui textures
    }
    CHECK(chosen.has_value());
    if (!chosen)
        return;
    const auto recent = LoadRecentProjects();
    CHECK(!recent.empty() && recent.front().name == "LauncherGame");
    std::optional<Project> project = Project::Load(*chosen);
    CHECK(project.has_value());
    if (!project)
        return;

    // Editor on the project: content root, start scene, blueprint and model from the content browser.
    Scene        scene;
    PhysicsWorld physics(*F().jobs, F().events, F().assets.get());
    ScriptSystem scripts(F().events, nullptr, &physics, F().assets.get());
    SceneRenderer sceneRenderer(*F().renderer, *F().context, *F().assets);
    sceneRenderer.shadows.resolution = 512;
    FlyCamera                camera;
    std::vector<ModelHandle> modelRefs;
    {
        Editor editor({.window        = *F().window,
                       .renderer      = *F().renderer,
                       .scene         = scene,
                       .assets        = *F().assets,
                       .sceneRenderer = sceneRenderer,
                       .camera        = camera,
                       .modelRefs     = modelRefs,
                       .physics       = &physics,
                       .scripts       = &scripts,
                       .project       = &*project,
                       .layoutFile    = project->SavedDirectory() / "EditorLayout.ini"});
        const auto runFrames = [&](int count) {
            for (int i = 0; i < count; ++i) {
                F().window->PollEvents();
                F().events.Flush();
                editor.FixedUpdate(1.0f / 60.0f);
                F().assets->Update();
                editor.Update(1.0f / 60.0f);
                if (auto frame = F().renderer->BeginFrame()) {
                    editor.Render(*frame);
                    F().renderer->EndFrame(*frame);
                }
            }
        };
        CHECK(editor.ContentRoot() == project->ContentDirectory());
        CHECK(editor.OpenScene(project->StartScene()) && scene.GetRegistry().AliveCount() == 11);
        CHECK(scene.FindPrimaryCamera() != NullEntity);
        editor.OpenAsset(project->ContentDirectory() / "Scripts" / "RainOnSpace.ugraph");
        CHECK(editor.Blueprints().Count() == 1);
        fs::copy_file(fs::path(ENGINE_ASSET_DIR) / "models" / "BoxTextured.glb", project->ContentDirectory() / "Models" / "Box.glb", ec);
        editor.OpenAsset(project->ContentDirectory() / "Models" / "Box.glb");
        CHECK(F().Pump([&] {
            runFrames(1);
            return scene.GetRegistry().AliveCount() > 11;
        }));
        CHECK(editor.HasUnsavedChanges() && !editor.ConfirmQuit()); // asks first
        CHECK(editor.SaveAll() && !editor.HasUnsavedChanges() && editor.ConfirmQuit());
        CHECK(!editor.BuildAndRun()); // no UnginePlayer next to the test executable: reported, no crash
        runFrames(3);                 // content browser, project menus
        editor.NewScene();
    }
    for (ModelHandle h : modelRefs)
        F().assets->Release(h);
    CHECK(fs::exists(project->SavedDirectory() / "EditorLayout.ini"));
    fs::remove_all(root, ec);
}

TEST_CASE(Audio_SoundAssetsAndEditorPlay)
{
    const fs::path  dir = fs::temp_directory_path() / "ungine_gpu_audio";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path toneFile = dir / "tone.wav";
    WriteWav(toneFile, MakeTone(440.0f, 0.5f, 48000, 0.5f));

    // --- Sound assets: async load, cache, stream mode, failure, hot reload, release.
    AssetManager&            assets = *F().assets;
    std::vector<SoundHandle> loadedSounds, reloadedSounds, failedSounds;
    Subscription s1 = F().events.Subscribe<AssetLoadedEvent<SoundData>>([&](const auto& e) { loadedSounds.push_back(e.handle); });
    Subscription s2 = F().events.Subscribe<AssetReloadedEvent<SoundData>>([&](const auto& e) { reloadedSounds.push_back(e.handle); });
    Subscription s3 = F().events.Subscribe<AssetFailedEvent<SoundData>>([&](const auto& e) { failedSounds.push_back(e.handle); });
    const SoundHandle tone = assets.LoadSound(toneFile);
    CHECK(assets.State(tone) == AssetState::Loading);
    CHECK(F().Pump([&] { return assets.State(tone) == AssetState::Ready; }, 2000));
    CHECK(loadedSounds.size() == 1 && loadedSounds[0] == tone);
    const auto data = assets.Get(tone);
    CHECK(data && !data->Streamed() && data->frames == 24000 && data->sampleRate == 48000);
    CHECK(assets.LoadSound(dir / "." / "tone.wav") == tone && assets.RefCount(tone) == 2);
    const SoundHandle streamed = assets.LoadSound(toneFile, SoundLoadMode::Stream);
    CHECK(streamed != tone);
    CHECK(F().Pump([&] { return assets.State(streamed) == AssetState::Ready; }, 2000) && assets.Get(streamed)->Streamed());
    const SoundHandle missing = assets.LoadSound(dir / "missing.wav");
    CHECK(F().Pump([&] { return assets.State(missing) == AssetState::Failed; }, 2000));
    CHECK(failedSounds.size() == 1 && !assets.Error(missing).empty());
    const auto infos = assets.Sounds();
    CHECK(infos.size() == 3 && std::ranges::any_of(infos, [&](const SoundInfo& i) { return i.handle == streamed && i.streamed; }));

    // Hot reload: a changed file replaces the content (voices playing the old one keep it).
    assets.SetHotReload(true);
    WriteWav(toneFile, MakeTone(440.0f, 0.25f, 48000, 0.5f));
    fs::last_write_time(toneFile, fs::file_time_type::clock::now() + std::chrono::seconds(2), ec); // distinct mtime
    CHECK(F().Pump([&] { return !reloadedSounds.empty() && assets.Get(tone) && assets.Get(tone)->frames == 12000; }));
    CHECK(data->frames == 24000); // the old data is still intact
    assets.SetHotReload(false);
    assets.Release(streamed);
    assets.Release(missing);
    assets.Release(tone);
    CHECK(assets.RefCount(tone) == 1);

    // --- Editor: an Audio Source plays while playing, pauses with Pause, stops with Stop.
    Scene        scene;
    Registry&    r = scene.GetRegistry();
    PhysicsWorld physics(*F().jobs, F().events, F().assets.get());
    AudioEngine  engine(AudioEngineDesc{.device = false});
    AudioSystem  audio(engine, F().assets.get(), &physics);
    ScriptSystem scripts(F().events, nullptr, &physics, F().assets.get(), &audio);
    const Entity speaker = scene.CreateEntity("Speaker");
    scene.EditTransform(speaker).position = {0.0f, 1.0f, 0.0f};
    r.Emplace<AudioSource>(speaker, AudioSource{.sound = PathToUtf8(toneFile), .loop = true});
    const Entity zone = scene.CreateEntity("Room");
    // Dry room: a reverb tail would keep sounding while paused.
    r.Emplace<ReverbZone>(zone, ReverbZone{.halfExtents = glm::vec3(20.0f), .reverb = {.wet = 0.0f}});
    r.Emplace<AudioListener>(scene.CreateEntity("Ears"));

    SceneRenderer sceneRenderer(*F().renderer, *F().context, *F().assets);
    sceneRenderer.shadows.resolution = 512;
    FlyCamera                camera;
    std::vector<ModelHandle> modelRefs;
    Editor editor({.window        = *F().window,
                   .renderer      = *F().renderer,
                   .scene         = scene,
                   .assets        = *F().assets,
                   .sceneRenderer = sceneRenderer,
                   .camera        = camera,
                   .modelRefs     = modelRefs,
                   .physics       = &physics,
                   .scripts       = &scripts,
                   .audio         = &audio});
    camera.position = {0.0f, 2.0f, 8.0f};
    std::vector<float> mix(4800 * 2);
    const auto runFrames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            F().window->PollEvents();
            F().events.Flush();
            editor.FixedUpdate(1.0f / 60.0f);
            F().assets->Update();
            editor.Update(1.0f / 60.0f);
            engine.Render(std::span<float>(mix).first(800 * 2)); // ~1/60 s
            if (auto frame = F().renderer->BeginFrame()) {
                editor.Render(*frame, 0.5f);
                F().renderer->EndFrame(*frame);
            }
        }
    };
    const auto level = [&] {
        engine.Render(mix);
        double sum = 0.0;
        for (float v : mix)
            sum += static_cast<double>(v) * v;
        return std::sqrt(sum / static_cast<double>(mix.size()));
    };

    editor.Select(speaker); // inspector draws the Audio Source section, the viewport its icon and range
    runFrames(3);
    CHECK(!audio.Running() && engine.Stats().voices == 0);
    editor.Play();
    for (int i = 0; i < 200 && !audio.IsPlaying(speaker); ++i)
        runFrames(1);
    runFrames(3);
    CHECK(audio.Running() && audio.IsPlaying(speaker) && engine.Stats().voices == 1 && audio.Stats().zones == 1);
    CHECK(level() > 0.05);
    editor.Pause();
    runFrames(1);
    CHECK(level() < 1e-4 && audio.IsPlaying(speaker));
    editor.Play(); // resume
    runFrames(1);
    CHECK(level() > 0.05);
    editor.Stop();
    runFrames(2);
    CHECK(!audio.Running() && engine.Stats().voices == 0 && level() < 1e-4);

    // Content browser / double-click: preview on the UI bus.
    editor.OpenAsset(toneFile);
    runFrames(2);
    CHECK(audio.Previewing() && engine.Stats().voices == 1);
    audio.StopPreview();
    runFrames(2);
    CHECK(!audio.Previewing());
    fs::remove_all(dir, ec);
}

TEST_CASE(Editor_BlueprintFunctionsDebuggerAndPrefabs)
{
    const std::uint32_t errorsBefore = VulkanContext::ValidationErrorCount();
    const fs::path      dir = fs::temp_directory_path() / ("ungine_gpu_p19_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);

    Scene        scene;
    Registry&    r = scene.GetRegistry();
    PhysicsWorld physics(*F().jobs, F().events, F().assets.get());
    ScriptSystem scripts(F().events, nullptr, &physics, F().assets.get());
    SceneRenderer sceneRenderer(*F().renderer, *F().context, *F().assets);
    sceneRenderer.shadows.resolution = 512;
    FlyCamera                camera;
    std::vector<ModelHandle> modelRefs;
    Editor editor({.window        = *F().window,
                   .renderer      = *F().renderer,
                   .scene         = scene,
                   .assets        = *F().assets,
                   .sceneRenderer = sceneRenderer,
                   .camera        = camera,
                   .modelRefs     = modelRefs,
                   .physics       = &physics,
                   .scripts       = &scripts});
    camera.position = {0.0f, 3.0f, 10.0f};
    TextOverlay text(*F().renderer);
    const auto runFrames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            F().window->PollEvents();
            F().events.Flush();
            editor.FixedUpdate(1.0f / 60.0f);
            F().assets->Update();
            editor.Update(1.0f / 60.0f);
            if (auto frame = F().renderer->BeginFrame()) {
                editor.Render(*frame, 0.5f);
                text.Add("Overlay text\nsecond line", {4.0f, 4.0f}, glm::vec4(1.0f, 0.8f, 0.2f, 1.0f), 1.0f);
                text.Render(*frame);
                F().renderer->EndFrame(*frame);
            }
        }
    };

    // --- Blueprint: a function, an exposed variable, a breakpoint --------------------------------
    const fs::path     file = dir / "Counter.ugraph";
    ScriptGraphEditor& bp   = editor.Blueprints();
    CHECK(bp.New(file) && bp.Graph());
    if (!bp.Graph())
        return;
    std::uint32_t print = 0, begin = 0;
    for (const ScriptNode& n : bp.Graph()->nodes) {
        if (n.type == "Debug.Print")
            print = n.id;
        if (n.type == "Event.BeginPlay")
            begin = n.id;
    }
    bp.Edit("Build", [&](ScriptGraph& g) {
        g.variables.push_back({"Count", PinType::Int, std::int32_t{0}});
        g.variables.push_back({"Speed", PinType::Float, 1.0f, true});
        CHECK(g.AddFunction("Bump", {0.0f, 0.0f}));
        std::uint32_t entry = 0, ret = 0;
        for (const ScriptNode& n : g.nodes) {
            if (n.type == "Function.Entry" && n.function == "Bump")
                entry = n.id;
            if (n.type == "Function.Return" && n.function == "Bump")
                ret = n.id;
        }
        g.FindFunction("Bump")->inputs.push_back({"Amount", PinType::Int});
        g.FunctionSignatureChanged("Bump");
        const std::uint32_t get = g.AddNode("Variable.Get", {100.0f, 150.0f}, "Count", "Bump");
        const std::uint32_t add = g.AddNode("Math.AddInt", {250.0f, 150.0f}, {}, "Bump");
        const std::uint32_t set = g.AddNode("Variable.Set", {400.0f, 0.0f}, "Count", "Bump");
        CHECK(g.Connect(entry, "Then", set, "In").empty());
        CHECK(g.Connect(set, "Then", ret, "In").empty());
        CHECK(g.Connect(get, "Value", add, "A").empty());
        CHECK(g.Connect(entry, "Amount", add, "B").empty());
        CHECK(g.Connect(add, "Result", set, "Value").empty());
        const std::uint32_t call  = g.AddNode("Function.Call", {200.0f, 0.0f}, "Bump");
        const std::uint32_t count = g.AddNode("Variable.Get", {200.0f, 120.0f}, "Count");
        g.FindNode(call)->defaults["Amount"] = std::int32_t{5};
        CHECK(g.Connect(begin, "Out", call, "In").empty());
        CHECK(g.Connect(call, "Then", print, "In").empty());
        CHECK(g.Connect(count, "Value", print, "Text").empty());
        g.FindNode(print)->defaults["Duration"] = 100.0f;
    });
    CHECK(std::ranges::none_of(bp.Diagnostics(), [](const ScriptDiagnostic& d) { return d.error; }));
    bp.OpenScope("Bump");
    CHECK(bp.Scope() && *bp.Scope() == "Bump");
    bp.Focus();
    runFrames(3); // function scope on the canvas, signature + locals in the sidebar, minimap
    bp.OpenScope({});
    bp.Select({print});
    bp.ToggleBreakpoints();
    CHECK(bp.Graph()->HasBreakpoint(print));
    std::uint32_t a = 0, b = 0;
    for (const ScriptNode& n : bp.Graph()->nodes)
        if (n.function.empty() && n.type == "Variable.Get")
            a = n.id;
    b = begin;
    bp.Select({a, b});
    bp.AlignSelection(ScriptGraphEditor::Align::Left);
    CHECK(bp.Graph()->FindNode(a)->position.x == bp.Graph()->FindNode(b)->position.x);
    CHECK(bp.Save());

    const Entity actor = scene.CreateEntity("Counter");
    ScriptComponent component{file.string()};
    component.variables["Speed"] = {2.5f, 0};
    r.Emplace<ScriptComponent>(actor, component);
    editor.Select(actor); // inspector: exposed variables
    runFrames(2);

    // Play: the breakpoint stops before Print with Count = 5; the graph opens there.
    editor.Play();
    runFrames(4);
    CHECK(scripts.DebugPaused() && scripts.PausedAt() && scripts.PausedAt()->node == print);
    const Entity running = actor;
    const auto   watch   = scripts.Watch(file, running);
    CHECK(watch && std::ranges::any_of(watch->variables, [](const auto& v) {
              return v.first == "Count" && ValuesEqual(v.second, std::int32_t{5});
          }));
    CHECK(watch && std::ranges::any_of(watch->variables, [](const auto& v) {
              return v.first == "Speed" && ValuesEqual(v.second, 2.5f);
          }));
    CHECK(bp.Scope() && bp.Scope()->empty() && bp.SelectedNodes().size() == 1 && bp.SelectedNodes()[0] == print);
    scripts.DebugContinue(scene);
    runFrames(2);
    CHECK(!scripts.DebugPaused() && std::ranges::any_of(scripts.Messages(), [](const ScriptMessage& m) { return m.text == "5"; }));
    editor.Stop();
    runFrames(1);

    // --- Prefabs with meshes: create, place, override, revert, apply (+ undo), scene file ---------
    const ModelHandle box = F().assets->CreatePrimitive({.shape = PrimitiveShape::Box, .size = 1.0f});
    modelRefs.push_back(box);
    const Entity crate = scene.CreateEntity("Crate");
    r.Emplace<MeshRenderer>(crate, MeshRenderer{.model = box, .meshIndex = 0});
    const Entity lamp = scene.CreateEntity("Lamp", crate);
    scene.EditTransform(lamp).position = {0.0f, 1.0f, 0.0f};
    r.Emplace<Light>(lamp, Light{.intensity = 3.0f, .range = 4.0f});
    const fs::path prefabFile = dir / "Prefabs" / "Crate.uprefab";
    const std::uint64_t crateUuid = r.Get<Uuid>(crate).value, lampUuid = r.Get<Uuid>(lamp).value;
    CHECK(editor.CreatePrefabFrom(crate, prefabFile) && r.Has<PrefabInstance>(crate) && r.Has<PrefabLink>(lamp));
    const Entity crate2 = editor.PlacePrefab(prefabFile, {3.0f, 0.0f, 0.0f});
    CHECK(crate2 != NullEntity && r.Has<MeshRenderer>(crate2) && r.Get<MeshRenderer>(crate2).model == box);
    const Entity        lamp2     = r.Get<Hierarchy>(crate2).children.at(0);
    const std::uint64_t lamp2Uuid = r.Get<Uuid>(lamp2).value; // handles change when undo restores subtrees
    editor.Select(crate2);
    runFrames(2); // inspector prefab header, hierarchy tint

    r.Get<Light>(lamp2).intensity = 9.0f;
    CHECK(PrefabOverriddenKeys(scene, F().assets.get(), lamp2) == std::vector<std::string>{"light"});
    CHECK(editor.RunPrefabOp(Editor::PrefabOp::Revert, lamp2, "light"));
    const Entity lamp2b = scene.FindByUuid(lamp2Uuid);
    CHECK(lamp2b != NullEntity && r.Get<Light>(lamp2b).intensity == 3.0f);
    CHECK(editor.Undo()); // back to the override (subtree restored)
    const Entity lamp2c = scene.FindByUuid(lamp2Uuid);
    CHECK(lamp2c != NullEntity && r.Get<Light>(lamp2c).intensity == 9.0f);

    r.Get<Light>(scene.FindByUuid(lampUuid)).color = {0.0f, 1.0f, 0.0f};
    CHECK(editor.RunPrefabOp(Editor::PrefabOp::Apply, scene.FindByUuid(crateUuid)));
    const Entity lamp2d = scene.FindByUuid(lamp2Uuid);
    CHECK(lamp2d != NullEntity && r.Get<Light>(lamp2d).color == glm::vec3(0.0f, 1.0f, 0.0f) && r.Get<Light>(lamp2d).intensity == 9.0f);
    runFrames(3);

    const fs::path sceneFile = dir / "Prefabs.scene.json";
    CHECK(editor.SaveScene(sceneFile));
    editor.NewScene();
    CHECK(editor.OpenScene(sceneFile));
    runFrames(6);
    int instances = 0, meshes = 0;
    r.ViewOf<PrefabInstance>().Each([&](Entity, PrefabInstance&) { ++instances; });
    r.ViewOf<MeshRenderer>().Each([&](Entity, MeshRenderer& m) { meshes += F().assets->State(m.model) == AssetState::Ready ? 1 : 0; });
    CHECK(instances == 2 && meshes == 2);
    CHECK(sceneRenderer.Stats().gpuDriven ? sceneRenderer.Stats().instances >= 2 : true);
    Entity loadedLamp = NullEntity;
    r.ViewOf<Light>().Each([&](Entity e, Light& l) {
        if (l.intensity == 9.0f)
            loadedLamp = e;
    });
    CHECK(loadedLamp != NullEntity && r.Get<Light>(loadedLamp).color == glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(editor.RunPrefabOp(Editor::PrefabOp::Unlink, loadedLamp) && PrefabInstanceRoot(scene, loadedLamp) == NullEntity);
    runFrames(2);

    editor.NewScene();
    bp.Close(0);
    std::error_code ec;
    fs::remove_all(dir, ec);
    CHECK(VulkanContext::ValidationErrorCount() == errorsBefore);
}

// Phase 20 editor tools: macro / timeline / types / search windows, collapse, construction
// scripts following edits, debugger stepping with a conditional breakpoint.
TEST_CASE(Editor_Blueprint3ToolsAndConstruction)
{
    const std::uint32_t errorsBefore = VulkanContext::ValidationErrorCount();
    const fs::path      dir = fs::temp_directory_path() / ("ungine_gpu_p20_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    ScriptRegistry::Clear();
    ScriptRegistry::SaveEnumFile(dir / "Mood.uenum", {"Mood", {"Calm", "Angry"}, {}});
    ScriptRegistry::SaveInterfaceFile(dir / "Usable.uinterface", {"Usable", {{"Use", {{"Power", PinType::Float}}, {}}}, {}});
    CHECK(ScriptRegistry::LoadDirectory(dir).empty() && ScriptRegistry::FindEnum("Mood") && ScriptRegistry::FindInterface("Usable"));

    Scene        scene;
    Registry&    r = scene.GetRegistry();
    PhysicsWorld physics(*F().jobs, F().events, F().assets.get());
    ScriptSystem scripts(F().events, nullptr, &physics, F().assets.get());
    SceneRenderer sceneRenderer(*F().renderer, *F().context, *F().assets);
    sceneRenderer.shadows.resolution = 512;
    FlyCamera                camera;
    std::vector<ModelHandle> modelRefs;
    Editor editor({.window        = *F().window,
                   .renderer      = *F().renderer,
                   .scene         = scene,
                   .assets        = *F().assets,
                   .sceneRenderer = sceneRenderer,
                   .camera        = camera,
                   .modelRefs     = modelRefs,
                   .physics       = &physics,
                   .scripts       = &scripts});
    const auto runFrames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            F().window->PollEvents();
            F().events.Flush();
            editor.FixedUpdate(1.0f / 60.0f);
            F().assets->Update();
            editor.Update(1.0f / 60.0f);
            if (auto frame = F().renderer->BeginFrame()) {
                editor.Render(*frame, 0.5f);
                F().renderer->EndFrame(*frame);
            }
        }
    };

    // A blueprint with a construction script (Count posts), a macro, a timeline, events.
    const fs::path     file = dir / "Fence.ugraph";
    ScriptGraphEditor& bp   = editor.Blueprints();
    CHECK(bp.New(file) && bp.Graph());
    if (!bp.Graph())
        return;
    std::uint32_t print = 0, begin = 0;
    for (const ScriptNode& n : bp.Graph()->nodes) {
        if (n.type == "Debug.Print")
            print = n.id;
        if (n.type == "Event.BeginPlay")
            begin = n.id;
    }
    std::uint32_t spawn = 0, use = 0, add = 0, get = 0;
    bp.Edit("Build", [&](ScriptGraph& g) {
        g.variables.push_back({"Count", PinType::Int, std::int32_t{2}, true});
        g.variables.push_back({"Mood", PinType::Enum("Mood"), std::int32_t{1}, false});
        const std::uint32_t cons = g.AddNode("Event.Construction", {0.0f, 400.0f});
        const std::uint32_t loop = g.AddNode("Flow.ForLoop", {200.0f, 400.0f});
        const std::uint32_t last = g.AddNode("Math.SubtractInt", {0.0f, 520.0f});
        g.FindNode(last)->defaults["B"] = std::int32_t{1};
        get                             = g.AddNode("Variable.Get", {-150.0f, 520.0f}, "Count");
        spawn                           = g.AddNode("Entity.SpawnEmpty", {450.0f, 400.0f});
        const std::uint32_t self        = g.AddNode("Entity.Self", {300.0f, 520.0f});
        CHECK(g.Connect(cons, "Out", loop, "In").empty() && g.Connect(get, "Value", last, "A").empty() &&
              g.Connect(last, "Result", loop, "Last Index").empty() && g.Connect(loop, "Loop Body", spawn, "In").empty() &&
              g.Connect(self, "Self", spawn, "Parent").empty());
        // Macro AddOne(In, X) -> (Out, Y = X + 1), used between BeginPlay and Print.
        CHECK(g.AddMacro("AddOne", {0.0f, 0.0f}));
        g.FindMacro("AddOne")->inputs.push_back({"X", PinType::Int});
        g.FindMacro("AddOne")->outputs.push_back({"Y", PinType::Int});
        std::uint32_t in = 0, out = 0;
        for (const ScriptNode& n : g.nodes) {
            if (n.type == "Macro.Inputs")
                in = n.id;
            if (n.type == "Macro.Outputs")
                out = n.id;
        }
        add = g.AddNode("Math.AddInt", {200.0f, 100.0f}, {}, "AddOne");
        g.FindNode(add)->defaults["B"] = std::int32_t{1};
        CHECK(g.Connect(in, "X", add, "A").empty() && g.Connect(add, "Result", out, "Y").empty());
        use = g.AddNode("Macro.Use", {150.0f, 0.0f}, "AddOne");
        std::erase_if(g.links, [&](const ScriptLink& l) { return l.fromNode == begin; });
        CHECK(g.Connect(begin, "Out", use, "In").empty() && g.Connect(use, "Out", print, "In").empty() &&
              g.Connect(use, "Y", print, "Text").empty());
        g.FindNode(use)->defaults["X"] = std::int32_t{41};
        // Timeline + custom event with a parameter + dispatcher + interface.
        ScriptTimeline t{"Fade", 1.0f, false, false, {}};
        t.tracks.push_back({"Alpha", ScriptTrackKind::Float, {{0.0f, glm::vec3(0.0f)}, {1.0f, glm::vec3(1.0f)}}});
        t.tracks.push_back({"Ping", ScriptTrackKind::Event, {{0.5f, glm::vec3(0.0f)}}});
        g.timelines.push_back(t);
        g.AddNode("Timeline.Play", {0.0f, 700.0f}, "Fade");
        g.events.push_back({"Hit", {{"Damage", PinType::Float}}});
        g.dispatchers.push_back({"OnHit", {{"Damage", PinType::Float}}});
        g.AddNode("Event.Custom", {0.0f, 900.0f}, "Hit");
    });
    CHECK(std::ranges::none_of(bp.Diagnostics(), [](const ScriptDiagnostic& d) { return d.error; }));
    // Implementing the interface needs its function: Compile reports it.
    bp.Edit("Interface", [&](ScriptGraph& g) { g.interfaces.push_back("Usable"); });
    CHECK(std::ranges::any_of(bp.Diagnostics(), [](const ScriptDiagnostic& d) { return d.error; }));
    bp.Edit("Implement", [&](ScriptGraph& g) {
        g.AddFunction("Use", {0.0f, 1200.0f});
        g.FindFunction("Use")->inputs = {{"Power", PinType::Float}};
        g.FunctionSignatureChanged("Use");
    });
    CHECK(std::ranges::none_of(bp.Diagnostics(), [](const ScriptDiagnostic& d) { return d.error; }));
    CHECK(bp.Save());

    // The windows: macro scope, timeline editor, search, types.
    bp.OpenScope("AddOne");
    bp.OpenTimeline("Fade");
    bp.OpenSearch("Print");
    editor.OpenAsset(dir / "Mood.uenum");
    runFrames(4);
    if (const char* shot = std::getenv("UNGINE_TEST_SCREENSHOT")) { // documentation: the windows as they look
        glfwSetWindowSize(F().window->Native(), 1600, 900);
        runFrames(40);
        (void)std::system((std::string("import -window root ") + shot).c_str());
        glfwSetWindowSize(F().window->Native(), 320, 240);
        runFrames(5);
    }
    CHECK(!bp.Search("Print", false).empty() && bp.Search("Count", true).size() >= 1 && bp.Search("AddOne", true).size() == 3); // Macro node + its Inputs / Outputs
    bp.OpenScope({});

    // Construction script: follows edits of the graph and of the exposed variable.
    const Entity fence = scene.CreateEntity("Fence");
    r.Emplace<ScriptComponent>(fence, ScriptComponent{PathToUtf8(file)});
    editor.RunConstructionScripts();
    runFrames(2);
    const auto posts = [&] {
        int n = 0;
        r.ViewOf<ConstructionOwned>().Each([&](Entity, ConstructionOwned&) { ++n; });
        return n;
    };
    CHECK(posts() == 2);
    r.Get<ScriptComponent>(fence).variables["Count"] = {std::int32_t{4}, 0};
    bp.Edit("Default", [](ScriptGraph& g) { g.variables[0].value = std::int32_t{3}; }); // any graph edit runs them again
    runFrames(2);
    CHECK(posts() == 4);
    const fs::path sceneFile = dir / "Fence.scene.json";
    CHECK(editor.SaveScene(sceneFile));
    {
        Scene loaded;
        (void)LoadSceneFile(sceneFile, loaded, nullptr);
        int count = 0;
        loaded.GetRegistry().ViewOf<Uuid>().Each([&](Entity, Uuid&) { ++count; });
        CHECK(count == 1); // posts are not saved
    }

    // Collapse: the Print node into a function (undoable).
    bp.Select({print});
    CHECK(bp.CollapseSelection(false).empty() && bp.Graph()->FindFunction("NewFunction"));
    CHECK(std::ranges::none_of(bp.Diagnostics(), [](const ScriptDiagnostic& d) { return d.error; }));
    CHECK(bp.Undo() && !bp.Graph()->FindFunction("NewFunction"));

    // Debugger: conditional breakpoint inside the macro's copy, step over, call stack drawn.
    bp.Edit("Breakpoint", [&](ScriptGraph& g) {
        g.SetBreakpoint(print, true);
        g.breakpointOptions[print] = {"Count == 4", 0};
    });
    editor.Play();
    runFrames(2);
    CHECK(scripts.DebugPaused() && scripts.PausedAt() && scripts.PausedAt()->node == print);
    CHECK(posts() == 4); // rebuilt before BeginPlay
    scripts.DebugStepOver(scene);
    runFrames(2);
    CHECK(!scripts.DebugPaused());
    CHECK(std::ranges::any_of(scripts.Messages(), [](const ScriptMessage& m) { return m.text == "42"; }));
    editor.Stop();
    runFrames(2);
    CHECK(posts() == 4);

    editor.NewScene();
    runFrames(1);
    bp.Close(0);
    ScriptRegistry::Clear();
    std::error_code ec;
    fs::remove_all(dir, ec);
    CHECK(VulkanContext::ValidationErrorCount() == errorsBefore);
}
