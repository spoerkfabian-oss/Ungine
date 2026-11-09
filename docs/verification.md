# Verification log

What was verified per phase, and how. Historical results — always check the current branch and CI
before quoting them as current. Environment unless noted: Linux, GCC 13 / Clang with
`-Wall -Wextra -Wpedantic` (`-Werror` since Phase 20), lavapipe (Mesa, Vulkan 1.4) + Khronos
validation layers with synchronization validation, Xvfb.

## Standing results

- Engine code builds warning-free on GCC and Clang (`-Werror`) and on MSVC (`/W4 /WX`, CI).
- ASan/UBSan runs (CPU + GPU suites) found no engine defects beyond those fixed below; the only
  remaining report is a known 112-byte llvmpipe JIT leak.
- TSan (CPU tests): no engine races; remaining reports come from lavapipe internals and Jolt's debug
  `PhysicsLock` tracking.

## Not verified

- A real GPU (reference: GTX 1070 Ti), including the **queue family ownership transfer path** (lavapipe
  has one queue family) and real performance.
- Windows runtime paths: process launch, `CommandLineToArgvW`, wide-char file APIs, icon resource,
  `/SUBSYSTEM:WINDOWS`, NSIS installer behavior (registry file association, shortcuts, uninstall). CI
  builds the setup but does not run it; Windows CI runs CPU tests only (no Vulkan driver).
- A real audio device (only miniaudio's null device and the offline mixer).
- Mouse-driven editor interaction beyond the automated UI tests (gizmo dragging, most drag & drop,
  panel resizing).

## Per phase

| Phase | Verification |
|---|---|
| 1–13 | Sandbox with 3 models, EngineGpuTests (render path, IBL regeneration, culling, shadow/bloom/AO/exposure toggles, debug views, GPU timings, editor frames, lights incl. > 256 per cluster, local shadows incl. atlas recreation, picking/outline, scene round trips, undo/redo, spatial index vs. brute force, shadow cache counts) with 0 validation errors; CPU tests for cascades, atlas packing, cube faces, UUIDs, snapshots, dirty transforms, AABB tree, buddy allocator; screenshots compared (Khronos references, tonemappers, shadows, bloom, AO, outline, BVH overlay). |
| 14 Physics | CPU tests (falling/resting/sleeping, events, raycast/sphere cast, teleports, kinematic push, triggers, scaling, characters on steps/walls/jumps, serialization); GPU test for mesh colliders and editor Play/Pause/Step/Stop; TSan clean. |
| 15 GPU-driven | GPU path == CPU path pixel-exact (occluders, mirrored instance, 102-mesh model), stationary counters, freeze, debug views; 20 000-instance stress (24 indirect calls vs. 9169 draws). |
| 16 Asset pipeline | CPU tests for the texture cooker, mesh optimizer, shader hot reload; GPU tests for texture assets, hot reload, broken reload, missing texture appearing later, placeholders, LOD GPU == CPU, pipeline rebuild, upload ring/budget. |
| 17 | CPU tests for model refresh, cache pruning, Unicode `.bin` paths, physics interpolation/CCD/layers, scripting graph + VM, projects, platform; GPU tests for resize without wait idle, sorted transparents, LOD fades, box selection, Blueprint editor, project browser + editor with a project; CTest 13/13; install + `cpack -G TGZ` checked manually; ASan/UBSan clean. |
| 18 Audio | 11 CPU tests (WAV, offline mix, panning, distance, pitch/Doppler, loop/fades, streaming, occlusion, reverb, device thread, AudioSystem, serialization, Blueprint nodes); GPU test for sound assets + editor Play; TSan found two miniaudio races (fixed by own gain nodes and a fixed world-up). |
| 19 Blueprints 2 | 7 CPU Blueprint tests + prefab test + ECS hardening; GPU test for functions/debugger/prefabs/text overlay (found a non-deferred overlay pipeline release). |
| 20 Blueprints 3 | 10 CPU tests (types, macros, libraries, interfaces, dispatchers, timelines, input, save games, debugger, construction scripts, collapse); GPU tests incl. mouse-driven palette/timeline input; first GitHub CI run green on GCC, Clang and MSVC (MSVC found the `std::map` move issue → `FlatMap`, C4458 shadowing). |
| 20 hardening | Physics finite checks, layer index validation, upload batch reuse, bindless underflow, 64-bit pure stamps; `Physics_InvalidScaleDropsBody`, `Physics_LayerMatrixValidation`. |
| 21–23 repair | Phases merged uncompiled; repaired (const views, initializers, ImGui API, MSVC `fs::path` → `std::string`); EngineGpuTests 30/30 + 13 smokes, CI green. |
| A Hardening | GPU tests for skinning, runtime UI, editor UI in Play, content import + reference repair, asset lifecycle stress; CPU lifecycle fuzzers; every fix checked red-without/green-with. |
| 24 Streaming | CPU streaming tests; GPU tests for sub-level models, editor previews/Play, level loader; the level-switch smoke found the swapchain READ hazard. CTest 17/17. |
| 25 Cooking | CPU cook tests (pak integrity, model round trip, cooked project view, reproducibility, savegame migration, options); package smokes from `Content.upak`; ASan found a null `memcpy` in the model reader. CTest 19/19. |
| 26 Physics tools | 7 CPU tests (materials, contacts, joints, characters, ragdoll, serialization, nodes), stable over 5 runs; GPU test for per-triangle materials and editor tools (found GroupFilter before allocator registration, inverted slider/6DOF limits). |
| 27 Audio mix | 6 CPU tests (cues, priority/limits, snapshots, ducking, multi-ray occlusion, serialization); GPU UI test found a missing `PopID`. |
| 28 Particles | 6 CPU tests + cook reference test; GPU pixel tests (sprites, ribbons, mesh particles, soft particles) and editor test; found one-shots never finishing with bursts, absolute paths outside the effect folder. |
| 29 Navigation | 6 CPU tests (build, queries, areas/links/obstacles/bake, crowd, character agents over links, nodes); GPU editor test; found Detour random points outside the radius, stuck characters on links, holes from loading colliders, lost obstacles after tile rebuilds. |
| 30 Animation | 6 CPU tests (files, transitions, blend spaces, layers, IK, system); GPU editor test; found foot IK hitting the own capsule, notifies per update, and the Phase 21 skin-influence reorder bug (via screenshot). |
| 31 TAA/decals/SSR/fog | 2 CPU tests; GPU tests (TAA edge energy −35 %, decals, SSR, fog analytic ≈ volumetric within ±1/255, editor UI); found an llvmpipe crash on null buffer references, fog history lag, missing compute stages in shadow barriers. |
| 32 Terrain | 6 CPU tests (data, store, height field with holes, navmesh source, brushes + undo, node); GPU tests (CDLOD, foliage, editor tools); found staging reuse hazards, hole mask threshold, mirrored foliage frame, MSVC C4458. |
| 33 Materials | 4 CPU tests (files, codegen, every node compiles in every template, overrides/nodes); GPU tests (unlit/instance/override/WPO/masked/translucent, GPU == CPU, picking, live Provide, lit == glTF ±1/255; editor); template materials in the player smokes; found SPIR-V 1.6 demote, ImGui `AddRect` argument order, test UB with two temporaries. EngineGpuTests 54/54. |
| 34 Nested prefabs | CPU nested prefab/variant test + lifecycle fuzz; GPU settings-undo/nested-prefab editor test. EngineGpuTests 55/55. |
| 35 Blueprints | 3 CPU tests (nested containers, recursion, inheritance incl. error cases); GPU inheritance editor test. |
| 36 Blueprints | 3 CPU tests (latent nodes in functions, dispatcher signatures, utility nodes); GPU tests for refactoring and class-path repair; full GPU suite with sync validation, 17/17 smokes, CI green. |
| 37 Installers | Linux Release build, Release `EngineTests`, `cpack -G TGZ`, `PackageSmoke.cmake` with the player (and a negative check with a broken package). The first release run found the NSIS escaping bug (reproduced and fixed locally with makensis 3.09 on Linux); the release workflow now passes on Windows (setup + ZIP + cook smoke) and Linux. |
| Hardening 2 | External audit (revision fc5e213), 7 findings checked against the code: pak entry paths escaping the mount root, symlinks leaving `Content/` in cooks, terrain JSON size, KTX2 range/block overflow, cooked-model count amplification, navmesh mesh signatures without geometry, indices past the vertices in cooked models / `OptimizeMeshes`. The glTF part of the last one was already covered by `cgltf_validate`. Each fix has a regression test (`MalformedInputTests`, `CookTests`, GPU `Navigation_MeshColliderReloadRebuildsTiles`) checked red-without/green-with. |
| 38 Tools | GPU tests: content delete to trash with undo/redo, restore conflict and purge (`Editor_ContentDeleteGoesToTrashAndUndoes`); link-end gizmo with undo (navigation editor test); IK gizmo on the Fox's hand while playing, the hand reaches the target (animation editor test); bone overlay draws every joint and a click selects the head; background packaging in the editor. CPU: packaging progress 0→1, cancel before copying and inside the cook, cook cancel leaves no pak. |
| 39 Paks | CPU: compression (≥ 2× on low-entropy text, raw beyond 1:1024 and for `PakCompression::None`), a flipped stored byte caught by the digest, an implausible size refused on open, deleted entries hiding older mounts / disk / cooked data, a hand-written v1 pak still read; signatures (right key, foreign key, unsigned, forged index with a fixed FNV hash), signed packaging with a key file, missing key, key store; patches (no-op, changed/new/deleted, numbering, mount order, a foreign patch blocks mounting, full packaging removes patches). Smoke: `UnginePatchedPlayerSmoke` plays a packaged game with `Patch_1.upak`. |
