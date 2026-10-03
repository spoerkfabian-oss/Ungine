# Ungine — Project Memory

Persistent handover memory for continued development of Ungine.

- Last updated: 2026-10-03
- Repository: spoerkfabian-oss/Ungine
- Default branch: main
- Current working branch: branch
- Project type: C++20 / Vulkan 1.3 3D game engine
- Editor: UngineEditor
- Player: UnginePlayer
- Developer application: Sandbox

## 1. Project purpose

Ungine is a custom 3D game engine with an integrated editor and standalone player. Its intended workflow is broadly comparable to Unreal/Unity:

- Entity Component System
- scene and hierarchy editing
- Blueprint-style visual scripting
- prefabs
- physically based rendering
- GPU-driven rendering
- clustered Forward+
- Jolt Physics
- 3D audio using miniaudio
- asynchronous asset loading
- project templates
- packaging and standalone game execution

Primary development target is Windows with Visual Studio 2022/MSVC and Vulkan 1.3. The GTX 1070 Ti is the reference development GPU. Mesh shaders are intentionally not required.

The development style is concrete repository work: inspect the actual implementation, identify real defects, fix them, add regression tests where appropriate, commit the result, and verify the build/CI. Avoid speculative changes merely to produce commits.

## 2. Branch and Git rules

Repository:
spoerkfabian-oss/Ungine

Branch policy:
- main is the default integration branch.
- branch is the current user-requested working branch.
- New work should be committed to branch unless the user explicitly changes the target.
- claude/friendly-mccarthy-33nhl0 is an old feature branch that was merged and deleted. Do not recreate or use it.

Normal workflow:
1. Inspect current branch and relevant files.
2. Reproduce or reason about the concrete problem.
3. Make the smallest justified fix.
4. Add or update regression coverage when useful.
5. Commit with a clear message.
6. Check the resulting commit and CI.
7. Never claim CI is green without actually verifying it.

The user prefers real GitHub commits rather than ZIP archives.

## 3. Repository structure

Current intended structure:

Ungine/
- apps/
  - editor/
  - player/
  - sandbox/
- engine/
  - include/Engine/
  - src/
  - shaders/
- editor/
  - include/Editor/
  - src/
- tests/
- tools/
  - CMakeLists.txt
  - MakeSounds.cpp
- assets/
- templates/
- resources/
- cmake/
- CMakeLists.txt
- CLAUDE.md
- README.md

Architecture of the structure:
- Public engine headers are under engine/include/Engine.
- Engine-private implementation headers remain under engine/src.
- Editor headers are organized under editor/include/Editor.
- Applications live under apps.
- The developer sandbox lives under apps/sandbox.
- Tool-specific build configuration lives in tools/CMakeLists.txt.
- Renderer implementation details such as GpuScene and GpuCulling remain private.

Relevant organization commits:
- 6189db649ec47b7e899c028565f19d2c022fc7d4 — organize editor public headers
- b308b342d9451eaa9d285760a5cff344b082a52c — move developer sandbox into apps
- a5b31d179c50b3a2d47e6f660e4f340da24b0c6f — document tools build layout

## 4. Build and test

Typical Windows build:

cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release

CMake FetchContent supplies the dependencies. Vulkan SDK is required for glslc and a Vulkan 1.3 driver is required for runtime rendering.

Test targets:
- EngineTests — CPU tests
- EngineGpuTests — GPU/editor smoke tests
- CTest integration
- GPU tests can be enabled with ENGINE_GPU_TESTS=ON

Warning policy:
- GCC/Clang use strong warnings and Werror in CI.
- MSVC uses W4 and WX.
- Warning regressions are treated as build failures.

Packaging:
- cpack -C Release
- Windows can produce NSIS and ZIP packages.
- Linux produces a tarball and desktop integration script.

## 5. Technology stack

Core:
- C++20
- CMake 3.25+
- GLM
- GLFW
- nlohmann/json 3.11.3
- volk
- vk-bootstrap
- Vulkan Memory Allocator
- Jolt Physics 5.6.0
- meshoptimizer 0.25
- cgltf
- stb_image

Rendering:
- Vulkan 1.3
- dynamic rendering
- synchronization2
- descriptor indexing/bindless
- buffer device address
- timeline semaphores
- GPU-driven scene data
- Forward+
- PBR
- cascaded shadow maps
- GTAO
- bloom
- automatic exposure
- tone mapping
- image based lighting

Audio:
- miniaudio 0.11.22
- stb_vorbis
- Freeverb

Editor:
- Dear ImGui docking
- ImGuizmo

## 6. Core architecture

### Application lifetime

The Application member/lifetime ordering is intentionally:

EventBus -> Input -> Window -> VulkanContext -> Renderer -> ThreadPool -> AssetManager

Destruction happens in reverse order.

Main-loop concept:
1. Input new frame
2. Poll events
3. Flush EventBus
4. If minimized, wait for events while still processing required uploads/assets
5. FixedUpdate at 60 Hz using an accumulator
6. AssetManager update
7. application update
8. Renderer BeginFrame
9. render
10. Renderer EndFrame

Frame time is clamped to approximately 0.25 seconds.

### ECS

The engine uses a registry/entity/component architecture.

Important convention:
- Structural ECS changes should not be performed during active view iteration unless the registry guarantees safety.
- Transform writes are currently a convention rather than an enforced API rule.
- Scene::GetRegistry remains mutable intentionally.

This is a major future audit area because scripts, prefabs, physics and editor operations can all mutate entities.

### EventBus

Properties:
- typed publish/subscribe
- RAII Subscription
- true means event consumed
- re-entrancy safe
- thread-safe enqueue
- delivery during Flush
- move-only handlers supported

Lifetime rule:
- Subscription must not outlive its EventBus.

### ThreadPool

Properties:
- fixed worker count, normally hardware concurrency minus one
- FIFO work queue
- Submit returns futures
- future exceptions propagate
- fire-and-forget jobs log exceptions
- destructor drains the queue and joins workers

## 7. Vulkan context and synchronization

The Vulkan context enables the intended Vulkan 1.3 feature set, including:
- dynamic rendering
- synchronization2
- buffer device address
- descriptor indexing/update-after-bind
- timeline semaphores
- host query reset
- anisotropy
- depth clamp
- non-solid fill
- multi-draw indirect

Frames in flight:
- 2

Depth/coordinate conventions:
- negative viewport Y flip
- positive visual Y direction
- glTF CCW winding
- Reverse-Z
- depth clear = 0
- comparison = GREATER_OR_EQUAL
- D32_SFLOAT depth
- sRGB BGRA8 swapchain

Resize:
- swapchain maintenance extensions are used when available
- present fences allow old swapchains to be retired without unconditional device idle
- fallback uses device idle where required

Lifetime rule:
- GPU resources must not be released until the relevant fence/timeline guarantees that no in-flight command can reference them.

## 8. UploadQueue

UploadQueue supports asset work from arbitrary worker threads.

Operations include:
- CreateBuffer
- CreateTexture2D
- CreateTexture
- WriteBuffer

Design:
- staging ring
- default staging size around 64 MB
- 16-byte alignment
- wrapping ring
- oversized or full-ring allocations use dedicated staging buffers
- copy recording is synchronized
- uploads are grouped into batches
- batches are submitted to the transfer queue when possible
- timeline semaphore values identify batches
- graphics command buffers record the required acquire operations

Hardening already completed:
- failed batches cannot remain permanently pending
- discarded batches preserve ring ordering
- reused batch state is reset
- resources remain alive until upload readiness is established

Known limitations:
- a full ring uses dedicated staging instead of waiting
- budget is byte based
- mip operations can consume graphics time
- QFOT has not been exercised by the current software test environment because it exposes only one queue family

## 9. AssetManager

Asset handles use index + generation. Zero is null. Handles do not themselves keep assets alive.

Model flow:
1. normalize the path
2. cache by normalized absolute path
3. increment reference count
4. load/parse on the thread pool
5. build model and GPU resources
6. upload asynchronously
7. main-thread Update transitions the asset toward Ready
8. publish load/failure events after processing

Release behavior:
- zero references normally release the asset
- still-uploading assets use deferred/graveyard lifetime
- assets with unfinished jobs use an orphan path
- shutdown waits for jobs, flushes uploads and releases resources

Known limitations:
- hot reload uses mtime polling
- embedded textures are coupled to their model
- generated CreateModel assets are not reloadable
- instance refresh uses node-name matching
- texture cache uses LRU/mtime behavior
- some glTF texture formats/extensions are unsupported
- geometry-pool exhaustion is a load failure
- placeholders are simple box geometry

Open audit:
- simultaneous reload and release
- shutdown while worker results are still pending
- stale generation handles
- failed upload cleanup
- bindless slot lifetime
- renderer shutdown ordering

## 10. Rendering

The renderer is GPU-driven but still performs important CPU-side visibility work.

Important GPU data layouts:
- Vertex: 48 B
- GpuMaterial: 80 B
- GpuSubmesh: 96 B
- GpuInstance: 128 B
- GpuDraw: 16 B
- GpuBatch: 32 B

Bindless resources include:
- texture2D array
- sampler array
- storage image array
- cube textures
- image arrays for compute

Main rendering sequence:
1. environment/IBL update when needed
2. shadow rendering
3. depth/normal prepass
4. GTAO
5. clustered light culling
6. PBR forward pass
7. sky
8. bloom
9. auto exposure
10. tone mapping

Implemented features:
- glTF metallic/roughness PBR
- IBL
- cascaded shadow maps
- point and spot lights
- local shadow atlas
- Forward+ clustered lighting
- depth/normal prepass
- GTAO and bilateral filtering
- bloom
- histogram exposure
- multiple tone mapping modes
- GPU profiler
- RenderOutput
- shader hot reload

Known limitations:
- no mesh shaders
- no meshlets
- no mip streaming
- no PCSS
- GTAO is screen space
- cluster grid is fixed at 16 x 9 x 24
- cluster lists are capped
- no depth min/max optimization for clusters
- CSM is recomputed every frame
- local shadow caching has documented limits
- actual GTX 1070 Ti runtime validation is still required

## 11. Scene and transform systems

Scene contains:
- entities
- transform hierarchy
- cameras
- mesh renderers
- lights
- physics
- audio
- scripts
- prefab data

Transform processing is hierarchy aware and propagates parent-before-child.

Spatial index/BVH:
- used for rendering visibility queries
- renderer owns the spatial index consumption path
- changes must be marked correctly

Editor reparenting:
- preserves world position/transform
- hierarchy mutations are delayed where necessary to avoid invalidating UI iteration

Known limitations:
- transform write rules are conventions
- transform/culling jobs are not broadly parallelized
- spatial raycast uses AABBs
- physics raycast uses Jolt shapes
- RenderOutput and swapchain share visibility logic

## 12. Physics

Backend:
- Jolt Physics 5.6.0

Lifecycle:
- ECS physics components synchronize to Jolt
- bodies are created/removed according to current component state
- simulation updates transforms
- world poses are written back to local transforms
- parent transforms are validated before inversion

Completed hardening:
- replaced invalid GLM isfinite usage with std::isfinite-based checks
- reject zero/near-zero scales
- reject NaN/Inf scales
- reject invalid physics matrices
- reject non-finite positions and rotations
- reject nearly singular parent matrices before inversion
- validate all relevant matrix components
- validate physics layer indices before accessing the collision matrix
- invalid bodies are removed and creation is retried after returning to a valid state

Regression tests:
- Physics_InvalidScaleDropsBody
- Physics_LayerMatrixValidation

Important historical failure:
ValidPhysicsScale existed but was not used. With warning-as-error builds this produced an unused-function failure. It is now called from SyncBody.

Known limitations:
- mesh colliders are for static/kinematic use
- no joints/constraints
- no per-triangle material system
- collision events lack full contact information
- CCD is more expensive
- negative scale is not preserved through current decomposition
- Scaled uses absolute scale
- synchronization is O(number of bodies) per frame

## 13. Blueprint and visual scripting

Supported systems include:
- execution graphs
- variables
- arrays
- maps
- structs
- enums
- interfaces
- Blueprint libraries
- functions
- macros
- custom events
- event dispatchers
- timelines
- tweens
- input actions
- save games
- level requests
- construction scripts
- debugger and breakpoints
- pure nodes
- prefab spawning

Completed hardening/testing includes:
- pure-node cycle detection using an active evaluation set
- 64-bit evaluation stamps
- recursion/depth defense
- construction-script state invalidation
- correct invalidation of closed graph programs
- array/map copy-on-write tests
- debugger stepping and call-stack tests
- macro/library/interface/dispatcher tests
- Blueprint type validation

Intentional constraints:
- no general recursion
- no Blueprint inheritance
- no nested container types such as arrays of arrays
- restricted map key types
- enum values are index based
- macro expansion copies graph state
- interface calls are synchronous
- latent nodes are not supported in function/interface bodies
- timelines have limited track types
- save slots are not versioned/encrypted
- level loading is synchronous
- entity references saved by UUID can become null after a level restart because spawned entities get new UUIDs

Construction Script:
- runs in editor after changes
- runs before BeginPlay
- generated entities are temporary/non-persistent
- owner reset uses entity state snapshots and user-edit preservation
- documented edge cases remain

Debugger:
- breakpoints
- conditional breakpoints
- hit counts
- continue
- step over
- step into
- step out
- call stack
- watches and pin values

NEXT BLUEPRINT AUDIT:
- entity destruction during script execution
- component mutation during active script views
- latent-node synchronization and cancellation
- VM call-stack/depth behavior
- COW lifetime under mutation
- construction-owned entity cleanup
- level transition during execution
- debugger state after recompilation
- prefab spawn/teardown interaction

## 14. Prefabs

Prefabs are stored as .uprefab files.

Supported:
- create from hierarchy
- place/instantiate
- instance overrides
- revert one/all overrides
- Apply to prefab
- Unlink
- refresh after file changes
- Spawn Prefab from Blueprint

Intentional limitations:
- no nested prefabs
- no variants/inheritance
- hierarchy is defined by the prefab
- prefab root transform is outside the stored prefab definition
- Apply writes the prefab immediately
- undo restores scene state rather than a transactional file history
- refresh uses polling
- Spawn Prefab loads synchronously

Lifecycle audit:
- spawned entity destruction
- stale references after prefab refresh
- construction-owned entities
- scene unload ordering
- prefab rebuild while instances are selected or referenced

## 15. Audio

Audio uses miniaudio.

Implemented:
- WAV/OGG/MP3/FLAC
- streaming
- 2D and 3D playback
- buses
- master volume
- panning
- inverse-distance attenuation
- pitch
- Doppler
- looping
- fades
- pause/resume
- occlusion
- reverb zones
- listener
- editor preview
- Blueprint audio nodes

Race hardening:
- TSan found races around smoothed bus volume and listener world-up
- the affected implementation was changed to avoid unsafe concurrent miniaudio parameter access
- later CPU tests reported no engine race

Testing:
- null-device playback
- offline mix
- asset loading/cache/release
- editor playback controls
- preview
- occlusion/reverb behavior

Known limitations:
- no HRTF/binaural
- simple binary/single-ray occlusion
- one global Freeverb-style reverb path
- no sound-cue/randomization subsystem
- limited voice prioritization
- raw audio packaging

## 16. Editor

UngineEditor is the integrated development environment.

Major systems:
- project launcher
- dockspace
- viewport
- hierarchy
- inspector
- content browser
- renderer panels
- Blueprint graph editor
- prefab workflow
- undo/redo
- asset management
- game camera
- audio controls
- Blueprint debugger

The engine core deliberately does not depend on ImGui.

Known editor limitations:
- no ImGui multi-viewports
- limited import/thumbnail workflow
- deleting assets is not fully undoable
- renaming does not repair every external reference
- picking has frame delay
- box selection uses bounds centers
- multi-object inspector lacks a full mixed-value representation
- some interaction paths require manual testing

Editor-specific lifetime rule:
- viewport textures must remain alive long enough for the ImGui backend and GPU to finish using them
- editor resources must not be released using a renderer lifetime mechanism that is too early for the backend

## 17. Test and CI history

The project has extensive CPU and GPU/editor test coverage accumulated through phases 1–20.

Phase 20 covered:
- Blueprint types
- structs/enums/maps
- Switch/Select/MultiGate/Delay
- macros and libraries
- interfaces and dispatchers
- timelines and tweens
- input actions
- Save Game
- level requests
- debugger conditions and stepping
- Construction Script
- graph collapse
- editor Blueprint tooling

Documented validation results from the recent audit:
- CPU CTest suite passed
- GPU/editor suite passed in the software test environment
- synchronization validation reported 0 errors in those runs
- GCC/Clang warning builds were clean
- ASan/UBSan found no engine defects; the known llvmpipe JIT leak remained
- TSan found no engine race in the documented CPU run; external runtime/library findings were distinguished from engine code

These are historical results. Always verify the current branch and current CI before presenting them as current.

## 18. Recent concrete fixes

### GLM finite check fix
Commit:
1473984cceb95ed5d5209c64688116cb940bcd20

Problem:
GLM does not provide the used glm::isfinite API.

Fix:
Use std::isfinite with explicit component checks.

### Degenerate physics scale hardening
Commit:
611191d9a0a1b7ef2349587ee4aaa0c75dfd9e38

Problem:
Invalid scale values could reach Jolt body creation. ValidPhysicsScale was initially unused.

Fix:
- reject near-zero scales
- reject NaN/Inf
- remove invalid body
- retry when valid
- maintain complete finite matrix checks

Tests:
ad9fe60911c879c71dd1e6c2ed21ac5a51073178

### MSVC warning fix
Commit:
a9791a82448b32719f8582de8ec48c4488092e2f

Problem:
A local variable named begin shadowed an outer name under MSVC W4/WX.

Fix:
Renamed it to spawnBegin.

### GitHub Actions update
Commit:
8055fee31ef3a398582deb4300bc83aa9fbdce27

Fix:
- actions/checkout upgraded to v5
- actions/cache upgraded to v5
Purpose:
Node 24-compatible GitHub Actions.

## 19. Important CI lesson

A previous failure showed that syntax-only checking was insufficient.

Valid-looking code still failed the real build because an unused helper triggered Werror on GCC/Clang and WX-related failure on MSVC.

Rule:
- perform actual target compilation
- use production warning flags
- wait for CI before merging
- do not assume syntax-only success means CI success

## 20. Known limitations that should not be mistaken for regressions

Rendering:
- no mesh shaders
- no meshlets
- no mip streaming
- no PCSS
- limited shadow cache
- fixed cluster layout
- deferred renderer is not the primary path

glTF:
- no morph targets
- limited color/UV support
- selected extensions unsupported

Physics:
- no joints
- limited collider set
- limited collision event data
- negative scale cannot be faithfully preserved

Blueprint:
- no general recursion
- no inheritance
- restricted nested containers
- synchronous level loading
- unversioned save slots
- documented construction-script semantics

Editor:
- in-game UI is rendered in the editor viewport but interaction is currently available only in the player; pause-menu options are session-only and input remapping is not implemented
- some interaction testing is manual
- limited import workflow
- no full automatic reference repair

Audio:
- no HRTF
- simple occlusion
- simple reverb

Packaging:
- content is currently packaged relatively raw
- no full cooked/pak distribution pipeline

## 21. Open audit priorities

Priority 1: ECS and Scene lifecycle
- destruction during script execution
- creation/destruction during view iteration
- component add/remove during views
- stale Entity references
- scene unload
- prefab instance teardown
- Construction Script-owned entities
- hierarchy deletion/reparenting
- selection after entity destruction

Priority 2: Blueprint runtime lifecycle
- call stack and depth
- pure COW arrays/maps
- latent scheduling/cancellation
- shutdown while callbacks exist
- level transition during execution
- spawned entity lifetime
- construction cleanup
- debugger state after recompilation

Priority 3: Asset lifetime
- reload + release races
- shutdown with pending jobs
- stale generation handles
- upload failures
- bindless lifetime
- renderer destruction ordering

Priority 4: Windows/MSVC runtime
- full W4/WX build
- process launch
- command-line parsing
- UTF-8/Unicode paths
- NSIS installer
- .ungineproj file association
- icons/resources
- Build & Run
- packaged player

Priority 5: Real hardware
- GTX 1070 Ti
- Vulkan validation
- synchronization validation
- dedicated transfer queue/QFOT
- actual GPU performance

## 22. Engineering rules for future work

1. Fix concrete bugs, not hypothetical ones.
2. Preserve existing architecture unless evidence requires a change.
3. Treat warnings as errors.
4. Treat lifetime and synchronization as first-class correctness concerns.
5. For ECS work, explicitly consider iterator invalidation and generation safety.
6. For Blueprint work, consider VM state, recompilation, latent execution and teardown.
7. For Vulkan work, reason about barriers, queue ownership, timeline values and frame lifetime.
8. For physics work, validate all external numeric input before Jolt receives it.
9. For asynchronous assets, ensure worker jobs cannot access destroyed owners.
10. Add regression coverage for concrete bugs.
11. Verify CI rather than assuming it.
12. Keep commits focused and descriptive.
13. Continue from the existing audit state rather than repeatedly fixing already-resolved issues.
14. If no concrete issue is found, do not manufacture a code change; document the audit result and move on.

## 23. Immediate continuation point

The repository is in a post-Phase-20 hardening state.

The recommended continuation sequence is:

ECS / Scene lifecycle
-> Blueprint runtime lifecycle
-> Prefab / Construction Script teardown
-> Asset shutdown/reload lifetime
-> Windows/MSVC runtime paths
-> GTX 1070 Ti + validation/QFOT

For every concrete issue:
- patch branch
- add regression coverage where appropriate
- commit
- verify CI
- update MEMORY.md when the project state materially changes

If an audit finds no concrete defect:
- make no speculative change
- record the inspected area
- continue with the next audit priority
