# Ungine — contributor and agent guide

Ungine is a C++20 / Vulkan 1.3 game engine with an integrated editor (UngineEditor), a standalone
player (UnginePlayer), a cook tool (UngineCook) and a developer sandbox. This file holds the working
rules and the map of the documentation; read it first.

## Documentation map

| File | Purpose |
|---|---|
| [MEMORY.md](MEMORY.md) | **Leading handover file:** current state, open PRs, decisions, lessons learned, audit priorities |
| [docs/](docs/README.md) | Technical reference per system (architecture, rendering, assets, scene, physics, audio, scripting, particles, navigation, animation, terrain, materials, editor, build and test, verification, limitations, history) |
| [README.md](README.md) | User guide: installing, building, working with the editor |
| [ROADMAP.md](ROADMAP.md) | Completed and planned phases |
| Folder READMEs | `assets/`, `resources/`, `templates/`, `tools/`, `cmake/`, `tests/` |

Keep MEMORY.md and the affected `docs/` pages current with every phase or significant change.

## Working rules (from the repository owner)

- Answer in German, short and to the point; work token-efficiently. Repository documentation is
  written in English.
- Before a new task, ask about real ambiguities (short multiple-choice questions with a recommended
  option).
- Review your own work before delivering and end with a self-review split into "bereits behoben"
  (already fixed) and "noch offen" (still open).
- Role: principal software engineer / game engine architect. Deliver as Git commits (no ZIP files).
- Branch: `branch` (bring it up to date with `main` before new work); never push to other branches.
- Delivery: one PR per phase from `branch`; drive it until CI (incl. the GPU suite) is green; the owner
  merges; start the next phase only after the merge (or stack on the open PR when the owner says so).
- Never merge or recommend merging with red or running CI.

## Engineering rules

1. Fix concrete defects; do not make speculative changes. If an audit finds nothing, record it and
   move on.
2. Preserve the architecture unless evidence requires a change.
3. Warnings are errors (GCC/Clang `-Werror`, MSVC `/W4 /WX`). Compile the real targets — syntax-only
   checks miss `-Wunused-function` and friends.
4. Treat lifetime and synchronization as correctness: barriers, queue ownership, timeline values,
   frame lifetime (`DeferRelease`), worker jobs vs. destroyed owners.
5. ECS: consider iterator invalidation and generations; components must be nothrow-movable (no
   `std::map/set/list/unordered_*` members — use `FlatMap`); write transforms only via
   `Scene::EditTransform` / `SetTransform`; call `MarkChanged` after render-relevant edits.
6. Blueprints: consider VM state, recompilation, latent execution and teardown.
7. Physics: validate all external numbers before Jolt sees them.
8. Keep data formats backward compatible (new fields optional when reading).
9. Add regression tests for concrete bugs and check that they fail without the fix.
10. Verify CI yourself; never claim it is green without looking.
11. When a GPU or Windows runtime is not available, list the unverified paths explicitly.

## Quick reference

- Build: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGLFW_BUILD_WAYLAND=OFF && ninja -C build`
  (Windows: Visual Studio 2022 generator). Details: [docs/build-and-test.md](docs/build-and-test.md).
- Tests: `build/tests/EngineTests`, `xvfb-run -a build/tests/EngineGpuTests` (with
  `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`), smokes with
  `xvfb-run -a ctest -E "^Engine(Gpu)?Tests$"` (configure with `-DENGINE_GPU_TESTS=ON`).
- MSVC pitfalls that GCC/Clang miss: C4458/C4456 shadowing (also locals in a nested `Impl` hiding
  members of the outer class), `fs::path` has no implicit conversion to `std::string`, non-`noexcept`
  standard container moves. More in MEMORY.md, "Pitfalls and lessons".
- Coordinates: Reverse-Z (clear 0, `GREATER_OR_EQUAL`), negative viewport Y flip, glTF CCW winding.
- Commit messages and PRs must not contain model identifiers.
