# cmake/

CMake modules included by the top-level `CMakeLists.txt`.

## Dependencies.cmake

Fetches and configures every third-party library with `FetchContent` (all pinned to a tag or commit
and added as `SYSTEM` so their warnings do not affect engine targets): Vulkan headers, volk,
vk-bootstrap, VMA, GLFW, GLM, cgltf, stb, nlohmann/json, Jolt Physics, meshoptimizer, bc7enc_rdo,
miniaudio (+ stb_vorbis, verblib), recastnavigation, glslang, Dear ImGui (docking) and ImGuizmo. Some
are built as small static libraries with `-O2` even in Debug (meshoptimizer, bc7enc, miniaudio,
recastnav, glslang). See [docs/architecture.md](../docs/architecture.md#technology-stack) for the
versions and roles.

- Change a version only together with a full build and test run on all CI platforms.
- `-DFETCHCONTENT_BASE_DIR=<dir>` shares downloads between build folders (CI caches `.deps/`).

## Shaders.cmake

Compiles `engine/shaders/*.{vert,frag,comp}` to SPIR-V with `glslc` from the Vulkan SDK
(`ENGINE_SHADER_OUTPUT_DIR`, depfiles for include tracking; Debug adds `-g -O0`). It also copies the
GLSL sources to `<output>/source`, which the runtime material compiler and shader hot reload use.

## Package.cmake

CPack configuration for installers (`cpack -C Release` in the build folder):

- Version `ENGINE_PACKAGE_VERSION` (cache variable, default = project version) and system names
  `windows-x64` / `linux-x64` → `Ungine-<version>-<system>.{exe,zip,tar.gz}`.
- Windows: NSIS (when `makensis` is found) + ZIP. The setup creates Start menu and desktop shortcuts
  to UngineEditor, uses `resources/ungine.ico`, and registers `.ungineproj` → `UngineEditor.exe "%1"`
  (HKCR, removed on uninstall).
- Linux: TGZ; `bin/install-desktop-integration.sh` does the desktop integration.
- `CPACK_VERBATIM_VARIABLES ON` is required: the NSIS registry commands contain backslashes and
  quotes that would otherwise be mangled in `CPackConfig.cmake`.
- The install layout itself (`bin/`, `bin/shaders`, `bin/templates`, `bin/samples`,
  `bin/resources`) is defined in `apps/CMakeLists.txt`.
- The release workflow (`.github/workflows/release.yml`) builds and tests these packages; see
  [docs/build-and-test.md](../docs/build-and-test.md#continuous-integration).
