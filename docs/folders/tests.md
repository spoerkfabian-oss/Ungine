# Folder `tests/`

Test executables and CMake test scripts. How to build and run them is described in
[build-and-test.md](../build-and-test.md#tests).

## Executables

| Target | Sources | Needs |
|---|---|---|
| `EngineTests` | `CoreTests`, `ScriptTests`, `BlueprintTests` (incl. prefabs), `Blueprint2Tests`, `Blueprint3Tests`, `ProjectTests`, `AudioTests`, `AudioMixTests`, `LifecycleTests`, `StreamingTests`, `CookTests`, `PhysicsToolsTests`, `ParticleTests`, `NavigationTests`, `AnimationTests`, `RenderingTests`, `TerrainTests`, `MaterialTests` | CPU only (audio uses the offline mixer or miniaudio's null device) |
| `EngineGpuTests` | `GpuTests.cpp` | A Vulkan device and a window (lavapipe + Xvfb in CI) |

Both take an optional filter argument: only cases whose name contains it run
(`EngineTests Blueprint`). Helpers: `Test.h` (`TEST_CASE`, checks, `RunAll`),
`BlueprintTestUtil.h` (graph building helpers). Tests read sample content via `ENGINE_ASSET_DIR`
(`assets/`) and write only below the system temp directory.

GPU test conventions: editor tests pass `.layoutFile = {}` so a local `editor.ini` cannot change the
layout; pixel-exact comparisons disable TAA; `UNGINE_TEST_SCREENSHOT=<png>` saves a screenshot in the
tests that support it; under Xvfb the cursor lags one frame, so UI automation holds end positions for
two frames.

## CTest smoke tests (`-DENGINE_GPU_TESTS=ON`)

- Sandbox runs: plain, editor (64 lights), 1024 lights, 5000 instances, physics (300 bodies), physics
  with the editor, CPU culling, hot reload. They fail on validation errors (exit code 2).
- Project runs built by fixtures:
  - `SmokeProjectSetup` (Physics template) → `UngineEditorSmoke`, `UnginePlayerSmoke`
  - `SmokeBasicProjectSetup` (Basic template) → `UnginePlayerBasicSmoke`
  - `SmokeLevelSwitchProjectSetup` (Basic + level switch + textured model) →
    `UnginePlayerLevelSwitchSmoke`, then `SmokePackageSetup` (`UngineCook`) →
    `UnginePackagedPlayerSmoke` (plays from `Content.upak`; fails on "not cooked into the pak")

Smokes need a display: run CTest under `xvfb-run -a` on headless Linux.

## CMake scripts

- `MakeProject.cmake` — creates a project from a template like `Project::Create`:
  `cmake -DTEMPLATE=<templates/X> -DDIR=<out> -DNAME=<name> [-DLEVEL_SWITCH=ON] [-DMODEL=<file.glb>]
  -P MakeProject.cmake`. `LEVEL_SWITCH` (Basic) starts in a small scene whose Blueprint opens the
  Annex level behind a custom loading screen; `MODEL` places a model in the Annex level.
- `PackageSmoke.cmake` — checks an installed or unpacked package:
  `cmake -DINSTALL=<package root> -DWORK=<scratch dir> [-DRUN_PLAYER=ON] -P PackageSmoke.cmake`. It
  verifies the layout (executables, ≥ 10 SPIR-V files, shader sources, templates, samples), builds a
  level-switch project from the packaged Basic template, cooks it with the packaged `UngineCook` and
  optionally runs the cooked game (200 frames, must reach the Annex level without errors). Used by the
  release workflow.

## Adding tests

- Put CPU cases in the matching `*Tests.cpp` (or a new file listed in `tests/CMakeLists.txt`); GPU and
  editor cases in `GpuTests.cpp`.
- For a bug fix, check that the new test fails without the fix.
