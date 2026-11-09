# Architecture

## Goals and scope

Ungine is a modular 3D game engine with an integrated editor and a standalone player, comparable in
workflow to Unreal Engine or Unity: ECS scenes, Blueprint visual scripting, prefabs, physically based
GPU-driven rendering, Jolt physics, 3D audio, navigation, animation, particles, terrain and node-graph
materials, plus project templates, cooking and packaging.

- **Primary target:** Windows with Visual Studio 2022 (MSVC); Linux (GCC/Clang) is fully supported
  for development and CI. No macOS.
- **Reference GPU:** GTX 1070 Ti (Pascal, dedicated transfer queue). Vulkan 1.3; mesh shaders are
  intentionally not required.
- **Original brief:** core (platform, events, ECS, asynchronous resource manager), Vulkan rendering
  (PBR, CSM, tone mapping, bloom, SSAO, Forward+, frustum culling), scene/physics (BVH, Jolt), tooling
  (ImGui editor).

## Technology stack

All dependencies are fetched by CMake `FetchContent`, pinned in `cmake/Dependencies.cmake` and
included as `SYSTEM`. CMake ≥ 3.25.

| Library | Use | Notes |
|---|---|---|
| Vulkan 1.3, volk, vk-bootstrap, VMA | GPU API, loader, device setup, memory | |
| GLFW, GLM | Window/input, math | |
| cgltf, stb_image | glTF and image loading | |
| nlohmann/json 3.11.3 | Scenes, assets, settings | engine-private |
| Jolt Physics v5.6.0 | Physics | engine-private, RTTI and exceptions on, asserts routed to the engine log |
| meshoptimizer v0.25, bc7enc_rdo (pinned commit) | Mesh optimization/LODs, BC7/BC5 encoding | engine-private, `-O2` even in Debug |
| miniaudio 0.11.22 (+ stb_vorbis, verblib) | Audio | own static lib `miniaudio` from `engine/src/ThirdParty/miniaudio_impl.c`, `MA_NO_RESOURCE_MANAGER`, `-O2` |
| recastnavigation v1.6.0 | Navmeshes and crowds | static lib `recastnav` (Recast, Detour, DetourCrowd, DetourTileCache), `-O2` |
| glslang vulkan-sdk-1.4.357.0 | Runtime GLSL → SPIR-V for materials | `ENABLE_OPT=OFF`, no HLSL/binaries, `-O2` |
| zstd v1.5.6 | Pak entry compression | static lib `zstd` from the library sources, `ZSTD_DISABLE_ASM`, engine-private |
| Monocypher 4.0.2 | Pak signatures (Ed25519) and digests (BLAKE2b) | static lib `monocypher` (+ optional ed25519), engine-private |
| Dear ImGui (docking) 1.92.9, ImGuizmo | Editor UI | only in `editor/` |

`glslc` from the Vulkan SDK compiles the engine shaders at build time (`cmake/Shaders.cmake`).

## Source layout

```
CMakeLists.txt            top-level build (version, options, subdirectories)
cmake/                    Dependencies.cmake, Shaders.cmake, Package.cmake (see docs/folders/cmake.md)
engine/include/Engine/    public engine headers
  Core/        Application, Window, Input, InputMap, Log, ThreadPool, MoveOnlyFunction, FlatMap,
               Platform, Project, FileSystem (pak/VFS), GameOptions
  Events/      Events, EventBus
  ECS/         Entity, Registry (header-only)
  Scene/       Components, Scene, Camera, SceneSerializer, Prefab, LevelStreaming, Decals,
               AabbTree, SpatialIndex, Frustum
  Assets/      AssetHandle, AssetManager, Model, Animation, GltfLoader, Primitives, Texture,
               TextureCooker, MeshOptimizer, ContentCooker
  Physics/     PhysicsWorld, PhysicsMaterial, Ragdoll
  Audio/       AudioTypes, SoundCue, Sound, AudioEngine, AudioSystem
  Particles/   ParticleEffect, ParticleFields, ParticleWorld
  Navigation/  NavTypes, NavigationWorld
  Animation/   AnimGraph, AnimGraphPlayer, AnimIk, AnimationSystem
  Terrain/     TerrainData, TerrainBrush
  Material/    MaterialGraph, MaterialAsset
  Script/      ScriptValue, ScriptRegistry, ScriptGraph, ScriptNodes, ScriptCondition, ScriptSystem
  UI/          UiLayout, UiSystem
  Renderer/    Renderer, SceneRenderer, TextOverlay, GeometryPool, RangeAllocator, ShaderReload,
               ShaderCompiler, GpuProfiler, Environment, ShadowCascades, ShadowAtlas,
               Vulkan/{VkCommon, VkUtils, VulkanContext, Swapchain, Buffer, Image, Upload, Bindless, Pipeline}
engine/src/               implementation; engine-private headers live here, e.g.
                          Renderer/{GpuScene, GpuCulling, ParticleRenderer, TerrainRenderer},
                          Material/MaterialBuild, Navigation/NavJson, Scene/{SceneJson, PrefabInternal}.h,
                          Script/{ScriptJson, ScriptExpand}.h, Script/ScriptInherit.cpp, Audio/SoundFile.h
engine/shaders/           GLSL sources (+ include/*.glsl); also shipped as source for runtime material compiles
editor/include/Editor/    Editor, ScriptGraphEditor, ParticleEditor, AnimGraphEditor, MaterialEditor,
                          ProjectLauncher, FileDialog, History, ImGuiLayer
editor/src/               editor library `Editor` (panels, content browser, tools, windows)
apps/editor/              UngineEditor (project browser + editor)
apps/player/              UnginePlayer (runs a project or a packaged game; OptionsMenu.h)
apps/cook/                UngineCook (command-line packaging)
apps/sandbox/             Sandbox (developer demo)
templates/                project templates (see docs/folders/templates.md)
assets/                   sample models, scripts and sounds (see docs/folders/assets.md)
resources/                icons and desktop integration (see docs/folders/resources.md)
tests/                    CPU and GPU test suites, CMake test scripts (see docs/folders/tests.md)
tools/                    developer tools, not built by default (see docs/folders/tools.md)
docs/                     this documentation
.github/workflows/        ci.yml (build + tests), release.yml (installers)
```

Generated/ignored: `build*/`, `.deps/` (FetchContent sources), `asset_cache/` (sandbox caches),
`editor.ini`. A project keeps its caches under `<project>/Saved/` (`Cache/Textures`,
`Cache/Shaders`, `pipelines.bin`, `EditorLayout.ini`, `player.log`).

## Core runtime

### Application

- Member order `EventBus → Input → Window → VulkanContext → Renderer → ThreadPool → AssetManager`
  (+ audio after the AssetManager); destruction runs in reverse.
- Loop: `Input::NewFrame → PollEvents → EventBus::Flush → (minimized: WaitEvents) →
  FixedUpdate (1/60 s, accumulator) → AssetManager::Update → OnUpdate → BeginFrame / OnRender /
  EndFrame`. Frame time is clamped to 0.25 s.
- While minimized (or started at 0×0) rendering waits, but uploads and asset loads continue
  (`Flush` + `AssetManager::Update`).

### Events

Typed publish/subscribe with RAII `Subscription`; returning `true` consumes the event; re-entrancy
safe; `Enqueue` is thread-safe (delivered at `Flush`). Handlers are `MoveOnlyFunction`, so move-only
lambdas work. A `Subscription` must not outlive its bus (convention).

### ThreadPool

Fixed workers (hardware threads − 1), FIFO. `Submit` returns a future (exceptions propagate through
it); `Enqueue` is fire-and-forget (exceptions are logged). The destructor drains the queue, then joins.

### ECS

- `Entity` is 64 bit (index + generation); components live in sparse-set pools. Every entity has
  `Uuid`, `Name`, `Transform`, `WorldTransform` and `Hierarchy`.
- Components must be nothrow-movable (concept-checked). **Do not use `std::map/set/list/unordered_*`
  members**: their move is not `noexcept` on MSVC (GCC/Clang do not notice). Use `FlatMap`
  (`Core/FlatMap.h`).
- `Emplace` gives the strong exception guarantee (a duplicate replaces + asserts); `Emplace` on a dead
  entity throws `std::invalid_argument`. `View::Each` iterates a snapshot, so callbacks may create and
  destroy entities/components freely. `ViewOf() const` returns a read-only `ConstView` without a
  snapshot. Component IDs are atomic; the registry itself is not thread-safe. A slot whose generation
  is exhausted is retired. `Get` without the component is a contract violation (assert).
- `Scene::SetParent` validates in Release too (invalid or cyclic → `false`); `CreateEntity` is
  all-or-nothing.

### Transforms and change tracking

- Write transforms **only** through `Scene::EditTransform(e)` / `SetTransform` (marks dirty). Direct
  registry writes go unnoticed; tests use `CountStaleTransforms()` to catch them. `Scene::GetRegistry()`
  stays mutable, so this is a convention.
- `UpdateTransforms` recomputes dirty subtrees only. Dirty flags are per entity index and cleared on
  destroy (safe slot reuse).
- Call `MarkChanged(e)` after changing `MeshRenderer`, `Light` or other render-relevant components
  after creation (the inspector and `ApplyEntityState` do this).
- `TakeChanges()` returns `SceneChanges{changed, destroyed, overflow}` (deduplicated; > 1 M entries →
  overflow = rebuild). There is exactly **one** consumer: the SpatialIndex inside the SceneRenderer.
- `Registry::EntityAtIndex` maps a slot to its current generation.

### Spatial index (BVH)

- `AabbTree`: dynamic tree in the style of Box2D/DBVT (fat AABBs with a 10 % margin, SAH-like
  insertion, AVL rotations, box/frustum queries, clipped raycasts, `Validate`).
- `SpatialIndex` (owned by the SceneRenderer, `Spatial()`): a mesh tree (world AABB of the whole mesh)
  and a light tree (box around the light range). `Sync` applies `TakeChanges`; meshes without a ready
  model wait in a pending list. `ChangedRegions()` (old + new bounds of changed meshes) drives the
  shadow cache. `Raycast` tests AABBs only (use the physics raycast for shapes).

## Vulkan layer

### Context and frames

- Vulkan 1.3 dynamic rendering + synchronization2; 1.2 buffer device address, descriptor indexing
  (update-after-bind, update-unused-while-pending), timeline semaphores, host query reset; 1.0
  anisotropy, depth clamp, non-solid fill, multi-draw indirect; `independentBlend` (required since
  TAA). A dedicated transfer queue is used when present, otherwise the graphics queue.
  `ValidationErrorCount()` counts validation errors (used by smoke tests).
- `kFramesInFlight = 2`. `FrameContext {cmd, image, view, format, extent, frameIndex, imageIndex}`;
  the swapchain image arrives in `COLOR_ATTACHMENT_OPTIMAL`. Per frame: command pool, fence,
  image-acquired semaphore; render-finished semaphores are per swapchain image. The fence is reset only
  after a successful acquire. The swapchain barrier in `BeginFrame` covers color attachment reads and
  writes (overlay-only passes load the image).
- Resize: with `VK_EXT_swapchain_maintenance1` (+ surface maintenance) there is no device wait idle —
  present fences per present, old swapchains are retired and freed once their fences signal. Without
  the extension the old `vkDeviceWaitIdle` path is used.
- Coordinates: negative viewport Y flip (+Y up, glTF CCW winding kept), Reverse-Z (clear 0,
  `GREATER_OR_EQUAL`), `D32_SFLOAT` depth, sRGB BGRA8 swapchain.

### Bindless

Descriptor set 0: `0` texture2D[], `1` sampler[], `2` image2D[] (rgba16f), `3` textureCube[] (256),
`4` image2DArray[] (rgba16f, 256; cube faces for compute), `5` texture3D[], `6` image3D[] (fog).
One pipeline layout with 128 B push constants (`STAGE_ALL`). Default samplers: 0 LinearRepeat,
1 LinearClamp, 2 NearestClamp, 3 ShadowCompare (`GREATER_OR_EQUAL`, border = depth 0 = lit).
Compute shaders must use `SampleTextureLod` (no implicit derivatives). Add/remove is thread-safe.
Buffers are accessed through buffer device addresses (vertex pulling, no vertex input state).

### Deferred destruction and transient memory

- `DeferRelease` / `DeferCall` (main thread only) free resources once the frame slot comes round
  again after its fence wait. Upload resources may be released only after `IsReady(ticket)`.
- Transient allocator: 8 MB per frame, host-visible with BDA (`PushTransient<T>`).

### UploadQueue

- **Any thread:** `CreateBuffer / CreateTexture2D / CreateTexture(format, w, h, data, mips) /
  WriteBuffer(…, UploadTicket&)` copy into a **staging ring** (`UploadQueueDesc::stagingRingSize`
  64 MB, 16 B alignment, wraps; uploads larger than half the ring or a full ring get a dedicated
  staging buffer, counted in `Stats().dedicatedStaging`). The memcpy runs without the lock; copy
  recording is serialized into the open batch. A batch over `frameBudget` (32 MB) is closed.
  `CreateTexture` uploads all mips (BCn, no blits). A ticket is the batch's timeline value.
- **Main thread:** `Renderer::BeginFrame` calls `Submit()` (oldest batches up to the budget, at least
  one, to the transfer queue, signalling the timeline) and `RecordAcquires(cmd)` (acquire barriers for
  finished batches — queue family ownership transfer — plus mip blits on the graphics queue). The frame
  submit waits on the reached timeline value.
- The transfer side writes mip 0 only and ends a batch with release barriers when ownership changes;
  without a separate family it ends with a global memory barrier (avoids a sync-validation false
  positive). Batches start with a global barrier (memory reuse).
- `IsReady(t)` = the acquire was recorded in a frame. `Flush()` blocks (startup/shutdown).
  `ImmediateSubmit` blocks on the graphics queue.

### GPU data layouts (C++ ↔ GLSL)

| Struct | Size | Notes |
|---|---|---|
| `Vertex` | 48 B | `tangent.w = 0` means no tangent: the shader derives the frame from derivatives ((∇u, −∇v, N), glTF convention B = cross(N, T)·w) |
| `GpuMaterial` | 80 B | incl. normalScale, occlusionStrength; texture fields are **texture-table entries** |
| `GpuSubmesh` | 96 B | LOD 0 + lodCount + lodFirstIndex/lodIndexCount/lodError ×4 |
| `GpuInstance` | 240 B | model, normal matrix, entity id, `prevModel`, `prevJointOffset` (motion vectors) |
| `GpuDraw` | 16 B | instance, submesh, batch, `params` (custom material parameter block) |
| `GpuBatch` | 32 B | index range, `instanceBase`, camera/shadow bucket, WPO displacement, material slot |
| `GpuLight` | 64 B | world space, color × intensity, spot scale/offset, first shadow view |
| `GpuShadowView` | 96 B | viewProj, atlas rect, texel size/distance |
| `GpuDecal` | 192 B | |
| `MeshPush` | 24 B | frame, visible list, cascade, flags |

`FrameUniforms` (`frame.glsl`) holds view/projection matrices (jittered, unjittered and previous),
camera, sun, sky/IBL intensities and slots, cascades, cluster info, local shadows, Hi-Z info,
material time, and BDA pointers to lights, clusters, shadow views, geometry pool buffers
(vertices/materials/submeshes), GPU-scene instances/draws, Hi-Z, the texture table and material
parameters. A visible-list entry packs the draw record (22 bits), LOD, an LOD fade (7 bits) and a
fade direction bit; the vertex shader resolves `draw → instance + submesh → material`.
