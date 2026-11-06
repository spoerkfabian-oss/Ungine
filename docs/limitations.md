# Known limitations

These are known, accepted limitations — not regressions. Unverified platforms are listed in
[verification.md](verification.md#not-verified).

## Platform

- No macOS. Without `VK_EXT_swapchain_maintenance1` resizing and VSync changes wait for device idle.
- Windows runtime paths and the installer behavior are built in CI but not run (see verification).

## Core, uploads and assets

- The transform write rule (`EditTransform`) and "subscriptions must not outlive the bus" are
  conventions, not enforced. `MarkChanged` is needed after render-relevant component edits.
- Transforms and CPU culling are not parallelized.
- Upload: a full staging ring falls back to dedicated staging instead of waiting; the budget counts
  bytes, not transfer time; mip blits (`CreateTexture2D` only) cost graphics-queue time; parsing and
  cooking cannot be cancelled (only skipped at shutdown).
- Hot reload uses mtime polling (no OS watcher). Embedded textures reload only with their model;
  `CreateModel` models cannot reload or be saved. `RefreshModelInstances` matches nodes by name.
  Texture cache pruning by mtime (4 GB). BC7 without RDO; cooking is slow in Debug. The same file as
  Color and Linear is two assets. Placeholders are boxes without physics. Reloading a model recreates
  its mesh colliders (contacts break briefly).
- Textures: BC7/BC5/RGBA8 (+ BC1/3/4 from KTX2); no Basis supercompression, ASTC, BC6H/HDR or mip
  streaming. Storage images are fixed to `rgba16f` in GLSL.
- Shader hot reload blocks the main thread and needs glslc + the build tree; ImGui pipelines are
  excluded. The validation warning "OutputNotConsumed" (mesh.vert locations 5/6 in the prepass) is
  harmless.
- glTF: no COLOR_0, samplers, UV sets > 0, morph targets, basisu/webp, specular/clearcoat/
  transmission. Blending sorts per object (no OIT, no sorting within a mesh).

## Rendering

- No mesh shaders/meshlets, no deferred path, no PCSS, no area/IES lights, simple analytic sky; IBL
  regenerates completely on every sun change.
- AO is screen-space only (no temporal filter, no depth mip chain); auto exposure has no center
  weighting; bloom has no lens dirt and is dominated by the sun disc.
- CSM: cascades every frame (casters culled against the camera Hi-Z); cascade 0 can get very small
  with a wide camera.
- Lights: fixed 16 × 9 × 24 cluster grid, 256 lights per cluster, no depth min/max culling;
  intensities relative to the sun.
- Local shadows: invalidation by range sphere, moving lights re-render every frame, cache lost outside
  the view/budget, simple eviction, material changes do not invalidate, tile sizes jump with
  importance, spots > 85° only partly covered, cube faces not culled against the camera frustum.
- GPU-driven: fixed-size geometry pool (full = load error), full-size visible lists per view, local
  shadow tiles only frustum-culled, no material/depth sorting in opaque buckets, culling dispatch
  ≤ 4 M records, statistics two frames old, occlusion for the camera (and cascade casters) only;
  `RenderOutput` and swapchain share visibility. LODs are chosen by camera distance only (also for
  shadows); shadows do not cross-fade LODs; skinned meshes stay on LOD 0.
- TAA: no upscaling, no reactive mask (fast particles/transparents may smear), fixed 8 Halton
  positions; pixel-exact comparisons need TAA off.
- Decals only on opaque surfaces, 32 per cluster, normal decals replace the normal proportionally, no
  atlases/mip bias.
- SSR only from the previous frame's screen data (edges/occluded → IBL), half resolution, blur instead
  of cone tracing, no SSR on transparents.
- Fog: froxels fixed at 16 px × 64 slices, one shadow tap per froxel (noise averaged by jitter +
  history; light leaks at thin walls), fog volumes only boxes (max 64), no noise/wind, isotropic sky
  ambient, reprojection assumes perspective cameras, ~10 frames history lag for volume changes,
  transparents/particles use fog up to their own depth. No player options for TAA/SSR/fog.
- The picking result is two frames late; the outline covers visible pixels only.

## Scenes, prefabs and streaming

- Prefabs: hierarchy inside an instance comes from the prefab (reparenting members is not an override),
  overrides per JSON field (vectors as a whole), Apply writes the file immediately (undo restores only
  the scene), refresh by polling (1 s), the inspector recomputes overrides every frame, the root
  transform is not part of the prefab, Spawn Prefab loads synchronously.
- Nested prefabs/variants: inner-level Apply transfers component values of existing entities only
  (adding/removing entities only on level 0), intermediate overrides still win over an inner Apply,
  revert only against the outermost level, a prefab that would contain itself is flattened, variants
  remove base entities only by override and inherit the root components, renaming/deleting a base
  breaks its variants (repair only on move in the content browser), expansion on every load, no level
  display in the hierarchy.
- Level streaming: a level activates in one frame (spike for big levels), box volumes only (no
  priorities/budgets), sources = `StreamingSource` entities or the camera, sub-level renderer/physics/
  camera settings are ignored, a sub-level's primary camera can take over (smallest UUID), a file is
  loaded at most once, UUID collisions get new UUIDs (references to them break), only script entity
  references resolve late, previews are read-only, no automatic reload of a loaded level, the loading
  screen scene has no scripts/3D, Open Level drops running sub-levels.

## Physics

- Absolute Jolt tolerances (2 cm slop); mesh colliders static/kinematic only (no convex hulls);
  negative scale ignored; non-uniform scale + collider rotation approximated; characters rotate around
  Y only.
- Contact impulse is an estimate (not Jolt's solver lambda), one contact per pair and event; the On Hit
  threshold is a text parameter; persist events reach scripts only for pairs with a script instance.
- Joints: motors only on hinge/slider/swing-twist twist, no stabilization for long chains, breaking per
  lambda/substep (coarse), rebuilds lose warm starting.
- Ragdolls: capsules only, radii from bone length × factor, hard switch between animation and physics,
  the root entity does not follow the ragdoll.
- Characters: the inner body always pushes (`pushStrength` only for CharacterVirtual contacts), crouch
  only changes the capsule height, platform rotation only around Y.
- Materials: surface is a free string, no per-pair combination rules, hot reload by polling (1 s).
- CCD per body is more expensive; contact pair lookup on removal is O(pairs); Sync is O(bodies) per
  frame; interpolation writes transforms every frame (BVH and shadow invalidation for moving bodies);
  Play snapshots hold raw model handles.

## Audio

- No HRTF/binaural (deliberately), no cones in the editor, occlusion by rays without diffraction/
  portals (wall thickness ignored), one global Freeverb on the World bus, reverb zones without
  priority, ducking by block RMS (no look-ahead, possible one-block latency), snapshots change bus
  gains only, no per-bus effects, bus mute is a hard step (10 ms smoothing).
- Cues: no nesting/sequences/distance variants; looping repeats the chosen variant; instance limit
  per cue path; voice stealing by estimated loudness without bus volumes or virtual voices; cue paths in
  Blueprint pins are not repaired on move.
- Streaming reads per voice from the file (no caching); format detection by extension; hot reload
  affects new plays only; failed sources start only on the next Play; editor pause stops abruptly;
  Doppler follows transforms per frame; packaging copies audio raw.

## Visual scripting

- Macros are copied at compile time (code size, node state per copy, no recursion); recursion depth
  ≤ 256 (pure recursion re-evaluates inputs on every read).
- Inheritance: parent classes are graph files only (no C++/component inheritance); event overrides
  match by type + parameter; Call Parent only inside the overriding scope; inherited timelines/macros/
  dispatchers cannot be overridden; no inherited nodes shown in the child canvas; Cast To returns the
  same entity; the parent chain is re-parsed from disk on every change of an open graph.
- Latent nodes in functions: interface callers do not wait; Timeline and Tween Float are not allowed in
  functions; latent state is per call (Do Once/Gate share state); two overlapping tweens of the same
  node: the older call expires.
- Dispatcher signatures are strict (same parameter count). Refactoring renames only variables and
  functions; *Move to parent* in an unopened parent file has no undo.
- Map keys: no containers/structs. Renaming a type (enum/struct/interface = file name) does not repair
  references; enum values are stored by index (removing shifts).
- Libraries have no variables/events. Timelines have Float/Vector/Event tracks only. Tweens work in
  world space (scale local). Save slots are not encrypted; entities are saved by UUID (spawned entities
  get new UUIDs after a level restart → null). Open Level in editor Play is synchronous.
- Construction scripts: a user edit that restores exactly the constructed value is lost; spawned
  entities without a script are not constructed.
- Debugger: not in pure functions, interface calls or construction scripts; conditions only over
  variables. Collapse ignores comments and has no rename dialog. Search has no full-text search in pin
  values (except reference mode). Inline values hide below 50 % zoom. Undo is per graph. Scripts run on
  the main thread; the step limit only aborts endless loops.

## Editor, projects and packaging

- The project browser and the editor are two applications in sequence (the Vulkan device is recreated).
  Renaming a project changes only its name (not folder/file). No ImGui multi-viewports.
- Box selection by bounds centers (no lasso, no partial overlap); the multi-object inspector shows the
  primary's values (no mixed state); no reference prefabs/sub-scenes.
- Import: GLB, glTF with local URIs, PNG/JPEG/KTX2, WAV/OGG/MP3/FLAC; no remote URIs. Streamed audio has
  no waveform. Move/rename repair recognized saved formats and the open scene, not arbitrary path
  strings or unsaved open Blueprints. Deleting asks but cannot be undone and does not check dependents.
- Build & Run and packaging need UnginePlayer next to the editor. Linux desktop integration is a script
  (no .deb/.rpm).
- Settings undo: compared per frame, committed only when no widget is active; project-settings undo
  always marks the project dirty; navigation settings apply immediately (may rebuild); not in Play;
  gizmo mode, anchor mode and window visibility are not part of the view state; the camera only on
  jumps; construction scripts re-run after every history step.
- Pak: uncompressed and unsigned (hashes detect corruption only); entries are read whole (except
  streamed sounds); source images also ship raw; always everything below `Content/`; shaders ship as a
  folder (incl. GLSL sources for materials); models are re-optimized on every cook; editor packaging is
  synchronous; cooked models need identical `MeshOptimizeSettings` in cooker and player; reference
  checks only for known formats; unreferenced images are cooked as Color; no patch/DLC workflow.
- Save versions: migration only via the Blueprint event, the version is raised by hand.
- Options: the menu shows/remaps only an action's first key; no resolution/monitor/LOD/texture
  quality; axis remap changes keys only; no gamepad bindings; graphics options cap the level settings
  and apply only in the player.

## Particles

CPU simulation only; collision = one physics raycast per particle per frame; no depth-buffer
collision; unlit + emissive (mesh particles get a simple sun/sky factor); no shadows; drawn after
transparent meshes (no shared sorting); alpha sorting per emitter and between emitters by bounds
center; mesh particles do not write depth; no flipbook blending, motion vectors, light emission,
vector fields or skeletal sampling; file changes restart instances; script one-shots survive
`ScriptSystem::End`; a SceneRenderer must render once per frame; the editor preview has no physics;
delay applies only to the first cycle.

## Navigation

Geometry only from colliders and terrains (no render meshes; moving objects need `NavObstacle`); bounds
rasterized per tile and masked per cell; no detail meshes (heights from polygons); tiles uncompressed;
sources diffed every update (O(colliders)); spheres/capsules as coarse UV spheres; off-mesh links only
between neighboring tiles (< 9.6 m by default) and traversed linearly (characters teleported along);
obstacles only boxes with yaw or cylinders; ≤ 64 obstacle requests per update; bake files only for the
main scene; signatures miss model changes with identical index counts/bounds; crowd limited per
profile, ≤ 16 filters per profile, targets projected 2/4/2 m, Arrived by XZ distance, avoidance only
among agents, agent height from the profile; queries capped at 512 polygons/256 corners; no gizmo for
link endpoints; the overlay draws at most 40 000 triangles; the test path covers one profile.

## Animation

Edit mode does not animate (only Play and the graph preview); linear crossfades (no inertialization);
unwrapped exit time; triggers stay set until consumed; no sync groups between layers; no blend-space
smoothing; additive layers relative to the rest pose; masks are whole bones; foot placement needs
physics (no effect in the preview); no IK gizmo; no per-bone look-at limits; FABRIK without joint
limits; two-bone without stretching; root motion translation only (horizontal through characters);
≤ 8 notifies per update, sounds/effects only in Play; no bone overlay, retargeting or morph targets;
graph hot reload by polling (1 s).

## Terrain

One square grid per terrain (no tiled worlds or terrain streaming, no rotation/scale); resolution fixed
at creation (import resamples heights only); 16-bit heights, 8 splat layers, no height/slope-based
material rules (only triplanar), no virtual texturing/splat mips; CDLOD without normal geomorphing,
per-node min/max scans, holes per cell (single-cell holes are solid in physics), velocity without LOD
morph motion; CPU foliage scatter (rescatter on density/seed changes), mesh foliage without LOD/
impostors/collision, no grass interaction or translucency, global wind, thinning by the LOD camera;
brushes without noise/erosion/stamps, undo stores the touched rectangle per stroke, removing a layer
stores full copies; layer/foliage paths in `.uterrain` are not repaired on move; no terrain editing
in Play.

## Materials

Shaders compile at runtime (first use per material and engine shader state ~0.5 s in Debug on a
worker, then cached on disk); games ship GLSL sources; pipelines are created synchronously on first
use (hitch) and per SceneRenderer; no material functions/macros/static switches, no per-node preview,
type checks only during code generation (no automatic padding for generic pins); entity parameters
only scalar/vector (textures only via instances), instance overrides only for graph parameters; custom
materials only on mesh renderers (not terrain, foliage, particles, decals); translucent/additive
without shadows, SSR, AO or decals; WPO velocity uses current-frame parameters, WPO does not move
normals or physics/navmesh geometry, culling relies on `maxDisplacement`; masked/translucent graphs run
fully in prepass and shadows; hot reload by polling; the material window's undo is separate; the canvas
has no box selection, copy or minimap; the instance window loads the parent chain synchronously.
