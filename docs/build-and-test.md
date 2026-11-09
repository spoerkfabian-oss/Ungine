# Build, run and test

## Requirements

- CMake ≥ 3.25, a C++20 compiler (MSVC 2022, GCC 13, Clang), the Vulkan SDK (for `glslc`) and a
  Vulkan 1.3 driver. All libraries come through FetchContent.
- Linux packages: `glslc libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
  libxkbcommon-dev`; for headless runs `mesa-vulkan-drivers vulkan-validationlayers xvfb`.

## Building

```
# Windows (Visual Studio 2022; start project: UngineEditor)
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release

# Linux
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGLFW_BUILD_WAYLAND=OFF
cmake --build build
```

Options: `-DENGINE_GPU_TESTS=ON` (GPU tests and smoke tests in CTest), `-DENGINE_WARNINGS_AS_ERRORS=ON`
(`-Werror` / `/WX` for engine targets only), `-DENGINE_PACKAGE_VERSION=…` (package version, default
the version in `CMakeLists.txt`), `-DFETCHCONTENT_BASE_DIR=…` (share downloaded sources).
For sanitizer builds use separate FetchContent binary folders
(`-DFETCHCONTENT_BASE_DIR=<build>/_deps -DFETCHCONTENT_FULLY_DISCONNECTED=ON
-DFETCHCONTENT_SOURCE_DIR_<DEP>=.deps/<dep>-src`) and `-fno-sanitize=vptr` (glslang has no RTTI).

## Apps

| Command | Purpose |
|---|---|
| `UngineEditor [project.ungineproj] [--frames N]` | Editor; without a project the project browser opens |
| `UnginePlayer [project.ungineproj] [--frames N]` | Runs a project; without an argument the project next to the executable (packaged game, mounts `Content.upak`) |
| `UngineCook <project> <out> [--pak-only \| --patch] [--key <file>]` | Cooks and packages a game (or a patch against the game in `<out>`); exit 0 ok, 1 arguments/project, 2 cooking |
| `Sandbox …` | Developer demo (below) |
| `MakeSounds <dir>` | Regenerates the sample WAVs (`tools/`, not built by default) |

## Packages

`cpack -C Release` in the build directory: Windows NSIS setup (if `makensis` is found) + ZIP, Linux
TGZ. Names: `Ungine-<version>-windows-x64.{exe,zip}`, `Ungine-<version>-linux-x64.tar.gz`. Everything
is installed below `bin/`: the executables, `shaders/` (SPIR-V + `source/`), `templates/`, `samples/`
(models, scripts, sounds), `resources/` and on Linux `install-desktop-integration.sh`. Details in
[folders/cmake.md](folders/cmake.md).

## Sandbox

`Sandbox [model.glb] [--frames N] [--editor] [--lights N] [--instances N] [--physics N]
[--cpu-culling] [--debug-view N] [--hot-reload] [--uncompressed] [--script graph.ugraph]`

- Default model `assets/models/WaterBottle.glb`. Exit codes: 0 ok, 1 load error, 2 validation errors.
- Creates a floor (12 × model radius), sets `shadows.maxDistance = 10 r`, `ao.radius = 0.2 r`.
  Small models (radius < 0.5) are scaled to ~1 m for physics.
- `--instances N`: box grid, every tenth bobs (dirty transforms, BVH, shadow invalidation).
  `--hot-reload`: live reload of assets and shaders. `--uncompressed`: RGBA8 instead of BC7/BC5
  (lavapipe decodes BC in software). `--script`: graph on the model root (opened in the editor).
- Keys: RMB + WASD/QE camera, T tonemapper, −/= exposure, arrows sun, B bloom, P shadows, C cascade
  colors, O AO, V debug view, K GPU/CPU culling, F5 reload/retry the model, X auto exposure, L light
  stress demo, G drop 40 bodies, H remove them, left click pushes a body, J character mode (WASD,
  Space jump, Shift run), **F1 editor on/off**. The title shows CPU times and physics stats.

## Tests

| Suite | Contents |
|---|---|
| `EngineTests` (CPU) | `CoreTests`, `ScriptTests`, `BlueprintTests` (incl. prefabs), `Blueprint2Tests`, `Blueprint3Tests`, `ProjectTests`, `AudioTests`, `AudioMixTests`, `LifecycleTests` (randomized lifecycle audits), `StreamingTests`, `CookTests`, `PhysicsToolsTests`, `ParticleTests`, `NavigationTests`, `AnimationTests`, `RenderingTests`, `TerrainTests`, `MaterialTests`, `MalformedInputTests` (crafted files, allocation limits via `AllocationProbe`) |
| `EngineGpuTests` | Real Vulkan device + window: rendering, culling, shadows, assets, editor workflows, UI automation |
| Smokes (CTest) | `SandboxSmoke`, `SandboxEditorSmoke`, `SandboxLightsSmoke`, `SandboxInstancesSmoke`, `SandboxPhysicsSmoke`, `SandboxPhysicsEditorSmoke`, `SandboxCpuCullingSmoke`, `SandboxHotReloadSmoke`, `UngineEditorSmoke`, `UnginePlayerSmoke`, `UnginePlayerBasicSmoke`, `UnginePlayerLevelSwitchSmoke`, `UnginePackagedPlayerSmoke`, `UnginePatchedPlayerSmoke` with setup fixtures from `tests/MakeProject.cmake` and `tests/PatchSmoke.cmake` |

Running them (Linux, headless):

```
ninja -C build
build/tests/EngineTests [filter]
VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT \
  xvfb-run -a build/tests/EngineGpuTests [filter]
cd build && xvfb-run -a ctest -E "^Engine(Gpu)?Tests$" -j3   # smokes need a display
```

Test helpers: `UNGINE_TEST_SCREENSHOT=<png>` saves a screenshot from GPU tests that support it;
editor tests use `.layoutFile = {}` so `editor.ini` does not interfere; under Xvfb the cursor lags one
frame (hold end positions for two frames). See [folders/tests.md](folders/tests.md).

## Continuous integration

- **`.github/workflows/ci.yml`** (push, PR, manual): Linux GCC and Clang Debug with
  `-DENGINE_WARNINGS_AS_ERRORS=ON` + `EngineTests`; the GPU suite (lavapipe, Xvfb, synchronization
  validation, CTest without `EngineTests`) on GCC for PRs, main and manual runs; Windows VS 2022 Debug
  with `/W4 /WX` + `EngineTests` (latest Vulkan SDK only for glslc; version from
  `vulkan.lunarg.com/sdk/latest/windows.txt`, cached per version). FetchContent sources are cached in
  `.deps` (build folders removed before saving).
- **`.github/workflows/release.yml`**: tag `v<version>` → GitHub release with packages,
  `SHA256SUMS.txt`, generated notes (prerelease when the version contains `-`); pushes to main,
  manual runs (input `version`) and PRs touching packaging files → workflow artifacts (14 days).
  Version: tag without `v` > input > `CMakeLists.txt` + `-dev.<run>`. Linux: Release build,
  `EngineTests`, `cpack -G TGZ`, unpack, `tests/PackageSmoke.cmake` with `RUN_PLAYER=ON` under Xvfb.
  Windows: VS 2022 Release, glslc from the SDK, NSIS (installed via choco if missing), `EngineTests`,
  `cpack -G "NSIS;ZIP"` (prints the NSIS log on failure), unzip + package smoke without the player (no
  Vulkan driver on the runners).
- Never merge a PR with red or running CI.
