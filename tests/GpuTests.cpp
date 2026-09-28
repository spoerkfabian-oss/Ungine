// Integration tests on a real Vulkan device (needs a display; CI: Xvfb + lavapipe).
#include "Test.h"

#include "Editor/Editor.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Primitives.h"
#include "Engine/Core/ThreadPool.h"
#include "Engine/Core/Window.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/SpatialIndex.h"
#include "Engine/Renderer/Vulkan/VulkanContext.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <fstream>
#include <optional>
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
    FlyCamera camera;
    camera.position = glm::vec3(4.0f, 5.0f, 6.0f);
    camera.yaw      = 1.25f;

    const fs::path file = fs::path(ENGINE_ASSET_DIR) / "test_roundtrip.scene.json"; // next to the models
    SaveSceneFile(file, scene, *F().assets, {.renderer = &settings, .camera = &camera});

    Scene         loaded;
    SceneRenderer settings2(*F().renderer, *F().context, *F().assets);
    FlyCamera     camera2;
    const auto    handles = LoadSceneFile(file, loaded, *F().assets, {.renderer = &settings2, .camera = &camera2});
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
    CHECK(camera2.position == glm::vec3(4.0f, 5.0f, 6.0f) && camera2.yaw == 1.25f);

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
