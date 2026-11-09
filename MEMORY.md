# Ungine — project memory

Leading handover file: current state, decisions, lessons and priorities. Technical details live in
[docs/](docs/README.md); working rules in [CLAUDE.md](CLAUDE.md). Update this file whenever the
project state materially changes.

- Last updated: 2026-11-09
- Repository: spoerkfabian-oss/Ungine, default branch `main`, working branch `branch`
- Version: 0.37.0 (`CMakeLists.txt`)

## 1. Collaboration preferences

- Answer in German, short and to the point; work token-efficiently. Repository docs are in English
  (decided 2026-11-06).
- Before a task, ask about real ambiguities (short multiple-choice questions with a recommended
  option).
- Use and update this memory often.
- Review your own work and end with a self-review: "bereits behoben" (already fixed) / "noch offen"
  (still open).
- Deliver real Git commits (no ZIP archives).

## 2. Current state

- Phases 1–37 are implemented and merged (see [docs/history.md](docs/history.md)); PR #25 (NSIS fix +
  English documentation overhaul) merged as c917f0a.
- **Open: PR #26** on `branch`: (1) hardening after the external audit of fc5e213 (BUG-002…008):
  pak/VFS entry paths, cook symlinks outside `Content/`, terrain JSON size, KTX2 range/block overflow,
  cooked-model counts, navmesh mesh signatures with a geometry hash, indices past the vertices; new
  `tests/MalformedInputTests.cpp` + `tests/AllocationProbe.*`. (2) Phase 38 Tools stacked on it at the
  owner's request: undoable content delete (`Saved/Trash`), nav-link / IK-target gizmos, bone overlay,
  background packaging with progress + cancel.
- Tag `v0.37.0` (first GitHub release) still to push — owner decides when.
- Next phases: not scheduled; candidates in [ROADMAP.md](ROADMAP.md). Ask the owner first.

## 3. Git and delivery rules

- Work on `branch`; bring it up to date with `main` before new work (fast-forward or merge); never push
  to other branches. `claude/friendly-mccarthy-33nhl0` is an old merged branch — do not reuse it.
- One PR per phase from `branch`; drive it until CI (GCC, Clang, MSVC, GPU suite, release workflow when
  packaging changes) is green; the owner merges; the next phase starts after the merge. Stacking on an
  open PR only when the owner says so.
- Never merge (or recommend merging) with red or running CI. PR #11 (phases 21–23) was merged red and
  broke `main`; PR #5/#6 likewise. Branch protection with required checks is recommended.
- Commit messages and PRs carry no model identifiers.

## 4. Pitfalls and lessons

Build and CI
- Compile the real targets with production warning flags; `-fsyntax-only` misses
  `-Wunused-function` (ValidPhysicsScale incident).
- MSVC-only failures seen repeatedly: C4458/C4456 shadowing (also a local in a nested `Impl` hiding a
  member of the outer class, e.g. never name a local `settings` in `PhysicsWorld.cpp`), C4505 unused
  functions, `fs::path` has no implicit conversion to `std::string` (use UTF-8 strings + `PathFromUtf8`),
  non-`noexcept` moves of standard containers break the ECS component concept (use `FlatMap`), Windows
  temp paths in short form (`RUNNER~1`) — compare paths with `fs::equivalent`. Before pushing, scan
  changed files with `-Wshadow` via `build/compile_commands.json`.
- Smoke tests need `xvfb-run -a ctest …`; without a display they fail instantly.
- Run `EngineGpuTests` from `build/`: editor tests without a project write into the working directory
  (e.g. `Terrains/`), which pollutes the repository root.
- CPack: values with backslashes (NSIS registry commands) need `CPACK_VERBATIM_VARIABLES ON`. NSIS can
  be tested on Linux (`apt install nsis`, mini project with `set(WIN32 TRUE)`).
- The release workflow runs only on main, tags, manual runs and PRs touching packaging files.

Vulkan and lavapipe
- llvmpipe loads speculatively from buffer references even when a loop runs zero times → never pass
  address 0 for lists read in loops (use a dummy element).
- Reused staging/readback memory needs execution barriers (sync-validation WAR); a swapchain pass that
  loads the image needs READ in the acquire barrier.
- glslang with SPIR-V 1.6 turns `discard` into DemoteToHelperInvocation → target SPIR-V 1.5.
- TAA jitter breaks pixel-exact comparisons → disable TAA in such tests. Fog history must reset on
  settings changes.
- Push constants must be re-pushed after switching from a depth-only to a fragment pipeline (lavapipe).

Engine
- Jolt objects (group filters, materials) must be created after `AcquireJolt` (allocator registration)
  and released before `ReleaseJolt`. Jolt constraint body order: body 1 = B, body 2 = A. Height-field
  holes are per vertex (a sample is removed only if all four cells are holes).
- `OptimizeMeshes` must remap skin influences when reordering vertices (Phase 21 bug found in Phase 30
  by looking at a screenshot) — look at a screenshot of every new model.
- Foliage mesh frames must be rotations (otherwise lighting is mirrored).
- Detour `findRandomPointAroundCircle` can return points outside the radius (retry).
- Paths stored in assets must stay relative across folders (`../`), otherwise packaged games break.
- Prefab fuzzing can grow exponentially; caps exist (≤ 300 entities, Apply only ≤ 24).
- Untrusted binary readers: check `offset > size || len > size - offset` (never `offset + len`), do
  block math in 64 bits, and check element counts against the smallest serialized record. Cooked
  models bypass `cgltf_validate`, so they validate index values themselves.
- Navmesh source signatures are stored in `.unav` bake files: only use values that are stable across
  sessions (content hashes, not asset revisions).
- Prove "no huge allocation" with `AllocationProbe` (tests) instead of relying on a crash.

Editor and tests
- GCC 13 crashed (ICE) on a designated-initializer `EditCommand` with lambdas while `History.h` was
  not included (incomplete type): include the header and build the command field by field.
- GPU tests that click on something small in the viewport need a large window (the default
  viewport is ~190x130 px); restore the old size afterwards, the window is shared by all tests.
- Always pair `PropertyRow` with `PopID` (ImGui ID stack assert). ImGui 1.92.9 swapped `AddRect`
  thickness/flags. Re-fetch component pointers after operations that may reallocate pools
  (e.g. `AssignMaterial`).
- Test editors use `.layoutFile = {}`; the GLFW window must be large enough for floating windows;
  under Xvfb the cursor lags one frame.
- A crash in the GPU suite may be reported after the wrong test (stdout is buffered in a pipe) — use
  ASan (`-fno-sanitize=vptr`, glslang has no RTTI) to find it. Never call `begin()`/`end()` on two
  different temporaries.

## 5. Audit priorities

Covered so far by randomized lifecycle tests (`tests/LifecycleTests.cpp`, GPU `Asset_LifecycleStress`)
and targeted tests. Still open:

1. Real hardware: GTX 1070 Ti with validation + synchronization validation, the dedicated transfer
   queue / ownership transfer path, real performance. (Needs the owner.)
2. Windows runtime: process launch, command-line parsing, Unicode paths, installer (file association,
   shortcuts, uninstall), Build & Run, packaged player. (Needs the owner.)
3. Blueprint runtime: debugger state after recompilation, Open Level in the middle of a chain.
4. Asset lifetime: shutdown with pending asset jobs under ASan for the newer tests; simultaneous
   reload + release.

## 6. Decision log (owner decisions)

- 2026-10-05: work on `branch`; plan A → 24 → 25 → 26 → 27 → C phases; one PR per phase.
- 24: additive sub-levels via volumes or Blueprint; async Open Level with a UI-canvas loading screen;
  editor Levels panel with previews (edit by opening the file); cross-level references by UUID.
- 25: one uncompressed pak (index + hashes; KTX2 textures, binary meshes, rest raw); everything from
  `Content/`; project save version with a Migrate Save Game event, newer slots refused; built-in options
  menu with remapping, saved per user.
- 26: joints as a component (Fixed/Hinge/Slider/Ball/Distance/Cone/6DOF) + ragdolls; per-triangle
  materials as `.uphysmat` per glTF material; contacts with point/normal/impulse/velocity/surfaces + Hit
  and Persist events; character rotation modes, platforms, crouch, sliding, push strength.
- 27: cues as `.ucue` wherever a sound path is accepted; priority 0–255 with stealing + per-cue/bus
  limits; snapshots + sidechain ducking; multi-ray occlusion with absorption; no diffraction, no HRTF.
- C phases (2026-10-14): 28 particles → 29 navmesh → 30 animation → 31 rendering → 32 terrain →
  33 materials → 34 nested prefabs + settings undo, one PR each.
- 28: modular emitter stacks, CPU simulation with jobs (not GPU), sprites/meshes/ribbons, sub-emitters,
  physics-raycast collision, unlit + emissive.
- 29: Recast/Detour tiled navmesh with runtime rebuild, DetourCrowd (direct or via character), agent
  profiles in the project (one navmesh each), areas/costs, off-mesh links, TileCache obstacles, baked
  `.unav`, full editor tools.
- 30: `.uanim` state machines, 1D/2D blend spaces, layers + masks, notifies, IK (two-bone, foot, look-at,
  FABRIK); Khronos Fox as test/template model.
- 31: TAA with motion vectors (no upscaling/FXAA), clustered decals, Hi-Z SSR with temporal
  accumulation, height + froxel volumetric fog with fog volumes.
- 32: CDLOD terrain, Jolt height field, navmesh source, 8 splat layers with sculpt/paint undo, foliage,
  heightmap import/export, holes, triplanar.
- 33: node-graph materials with generated GLSL, instances, blend/shading models, WPO with velocity,
  per-submesh overrides; glTF materials stay the default.
- 34: nested prefabs with per-level apply, variants; undo for renderer/project/physics settings and the
  editor view (camera only for jumps); asset windows keep their own undo.
- 35–37 (2026-11-03): Blueprint inheritance like UE + recursion + nested containers; latent nodes in
  functions + dispatcher checks + refactoring + utility nodes; CI-built installers (NSIS setup + ZIP,
  Linux TGZ). 36 and 37 stacked on PR #24 at the owner's request.
- 2026-11-06: documentation in English; `CLAUDE.md` becomes an index with rules, technical reference
  split into `docs/`; pages for the non-code folders in `docs/folders/` (owner: all docs in one place); stacked on PR #25.
- 2026-11-09: external audit — verify every finding, fix the real ones, one hardening PR.
- 2026-11-09: phase 38 = Tools (content delete undo, nav-link/IK gizmos, bone overlay, packaging
  progress), stacked on PR #26.

## 7. Merge log

| PR | Content | Merge |
|---|---|---|
| #11, #12 | Phases 21–23 (merged uncompiled) and their build repair (32ea260, 5e62aec) | 2026-10-05, 181cfc6 |
| #13 | Phase A hardening | 2026-10-06 |
| #14 | Phase 24 level streaming | f9a1699 |
| #15 | Phase 25 cooked builds, savegames, options | 4c58715 |
| #16 | Phase 26 physics tools | 2026-10-12 |
| #17 | Phase 27 audio | 8377582, 2026-10-14 |
| #18 | Phase 28 particles | fde7c2f, 2026-10-15 |
| #19 | Phase 29 navigation | fa23918, 2026-10-22 |
| #20 | Phase 30 animation | 2026-10-25 |
| #21 | Phases 31 + 32 rendering, terrain | 83cdfcb, 2026-10-27 |
| #22 | Phase 33 materials | 2026-11-02 |
| #23 | Phase 34 nested prefabs, settings undo | 2026-11-03 |
| #24 | Phases 35–37 Blueprints, installers | fc5e213, 2026-11-05 |
| #25 | NSIS fix + documentation overhaul | c917f0a, 2026-11-09 |
| #26 | Hardening after the external audit (BUG-002…008) + phase 38 tools | open |
