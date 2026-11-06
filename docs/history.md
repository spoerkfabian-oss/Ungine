# Phase history

Each phase was delivered as one or more commits and, from Phase 24 on, as one pull request from
`branch` (merged by the repository owner after CI was green).

| Phase | Content |
|---|---|
| 1 | CMake, log, window, application loop, Vulkan context |
| 2 | Event bus, swapchain, renderer (frames in flight, resize) |
| 3 | Buffers, images, uploads (mips), bindless, pipeline builder, shader build, deferred release |
| 4 | Depth, input, fly camera, ECS, scene graph, glTF loader, SceneRenderer, transient allocator |
| 5 | ThreadPool, UploadQueue (transfer queue, timeline, ownership transfer), thread-safe bindless, AssetManager, first tests and smoke test |
| 6 | PBR, IBL from a procedural sky, HDR + tone mapping, skybox, frustum culling, test models |
| 7 | Cascaded shadow maps, bloom, primitives, `CreateModel` |
| 8 | Depth/normal prepass, GTAO, specular occlusion, histogram auto exposure, debug views |
| 9 | ImGui editor library (docking, viewport, gizmo, panels), GPU profiler, `RenderOutput` |
| 10 | Clustered Forward+ (point/spot lights, `KHR_lights_punctual`), light editing, heatmap |
| 11 | Spot/point light shadows in an atlas |
| 12 | UUIDs, JSON scenes, primitives as assets, GPU picking, outline, multi-selection, undo/redo, file dialog |
| 13 | Dirty transforms + change lists, dynamic AABB tree, shadow cache with a buddy atlas, raycasts |
| 14 | Jolt physics (bodies, colliders, characters, events, queries), editor Play/Pause/Step/Stop |
| 15 | GPU-driven rendering: geometry pool, GPU scene, compute culling with two-phase Hi-Z occlusion, indirect draws |
| 16 | Asset pipeline: staging ring, texture assets (BC7/BC5, KTX2 cache, texture table), reload/hot reload, LODs, asset browser |
| 17 | Fixes (swapchain maintenance, minimize, upload, caches), pipeline cache, sorted transparency, LOD fades, physics layers/CCD/interpolation, Blueprint visual scripting, UngineEditor with projects, UnginePlayer, templates, installers |
| 18 | Audio (miniaudio): sound assets, buses, 3D, reverb zones, occlusion, editor and Blueprint integration |
| 19 | Blueprints 2: functions, arrays, timers, debugger, instance variables, tags; prefabs; text overlay |
| 20 | Blueprints 3: enums/structs/maps, macros, libraries, interfaces, dispatchers, timelines, tweens, input actions, save games, construction scripts, Find/Collapse; GitHub Actions CI |
| 21 | Skeletal animation: skins, clips, animator, GPU skinning, skin bounds (PR #11) |
| 22 | Runtime UI and the player pause menu (PR #11) |
| 23 | Content import and previews, reference repair (PR #11; merged uncompiled, repaired in PR #12) |
| A | Hardening: GPU tests for 21–23, randomized lifecycle audits and their fixes (PR #13) |
| 24 | Level/area streaming, asynchronous Open Level with loading screens (PR #14) |
| 25 | VFS and pak files, cooker, `UngineCook`, savegame versions, game options (PR #15) |
| 26 | Physics materials, contact data, joints, ragdolls, character tools (PR #16) |
| 27 | Sound cues, voice priorities/limits, snapshots, ducking, multi-ray occlusion (PR #17) |
| 28 | Particles: module stacks, CPU simulation, sprites/meshes/ribbons, particle editor (PR #18) |
| 29 | Navigation: Recast/Detour tiles per profile, crowd agents, bake files, editor tools (PR #19) |
| 30 | Animation state machines, blend spaces, layers, notifies, IK; Fox template (PR #20) |
| 31 | TAA, clustered decals, SSR, height and volumetric fog (PR #21) |
| 32 | Terrain: CDLOD, splat layers, holes, foliage, physics/nav sources, sculpting tools (PR #21) |
| 33 | Node-graph materials with runtime shader compilation and per-material pipelines (PR #22) |
| 34 | Nested prefabs and variants, settings undo (PR #23) |
| 35 | Blueprints: inheritance, recursion, nested containers (PR #24) |
| 36 | Blueprints: latent nodes in functions, dispatcher signatures, refactoring tools, ~55 utility nodes (PR #24) |
| 37 | Installers per CI: release workflow with tested Windows setup/ZIP and Linux TGZ (PR #24, fix in PR #25) |

Details of each phase's verification are in [verification.md](verification.md); decisions and lessons
are in [MEMORY.md](../MEMORY.md).
