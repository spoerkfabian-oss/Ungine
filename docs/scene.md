# Scenes, prefabs, streaming, skeletal animation and runtime UI

## Scene files and snapshots

- `Uuid` component (64 bit, random, never 0), `Scene::FindByUuid`, `CreateEntity(name, parent, uuid)`
  (0 or taken → new UUID), `Clear()`, `SetParent(child, parent, siblingIndex)`, `SiblingIndex`,
  `IsAncestor`.
- `SceneSerializer.h`: `SaveSceneFile` / `LoadSceneFile` (JSON, version 1; entities in parent-before-
  child order with UUID, parent, name, transform and all serialized components; file paths relative
  to the scene; primitives by recipe). Optional renderer settings (light, post, shadows, AO, lights,
  local shadows, culling, TAA, SSR, decals, fog — no debug flags), physics settings and the editor
  camera. Writes go through `.tmp` + rename. A failed load is rolled back completely; every model is
  acquired once and the handles belong to the caller. Missing JSON fields keep their defaults.
- Loading is split for streaming: `PrepareSceneFile(path)` (any thread: read, parse, collect model
  references incl. prefab files/overrides → `shared_ptr<const PreparedScene>`),
  `AcquireSceneModels` and `InstantiatePreparedScene` (main thread; returns the roots, destroys
  created entities on error). `LoadSceneFile` runs all three.
- In-memory snapshots (models as raw handles, no refcount): `SnapshotEntities` /
  `RestoreEntities(Original = same UUIDs + parent + sibling index | Duplicate = new UUIDs, appended,
  links and script entity references remapped)`, `SnapshotEntityState` / `ApplyEntityState`
  (components only; missing ones are removed) and `ApplyEntityStateDiff` (JSON diff for
  multi-selection edits).
- Construction-owned entities (`ConstructionOwned`) and streamed entities (`StreamedLevel`) are never
  written to scene files.

### Camera

`CameraComponent{fovY, nearPlane, primary}`; `Scene::FindPrimaryCamera` (primary, smallest UUID);
`CameraFromWorld`. The player renders through the primary camera, otherwise through the saved editor
camera.

## Prefabs

- A `.uprefab` is the scene format of one subtree (paths relative to the prefab, root transform
  identity). An instance has `PrefabInstance{prefab (absolute), built, unresolved}` on its root and
  `PrefabLink{instance, source}` on members.
- **Overrides** are JSON merge patches (RFC 7396) of the components against the canonical prefab
  JSON (scratch-entity round trip, absolute paths); script entity references are mapped to members.
- In scene files an instance is stored as root (uuid/name/transform) + `"prefab": {file, members
  [[source, uuid]], overrides {source: patch | null}}`; members are not stored, children below
  members are (their parent is the member UUID).
- `Rebuild` works in place (stable handles, only changed components replaced, missing members
  created, extra members destroyed, non-member children rescued). API: `CreatePrefab`,
  `InstantiatePrefab`, `PrefabOverrides`, `PrefabOverriddenKeys`, `ApplyPrefabInstance` (other
  instances keep their overrides), `RevertPrefabOverrides` (key/entity/instance),
  `UnlinkPrefabInstance`, `RefreshPrefabInstances` (mtime cache; the editor polls once per second).
  A missing prefab file keeps the instance data (`unresolved`) and writes it back unchanged.

### Nested prefabs and variants (file format v2; v1 still loads)

- A prefab file entry with `"prefab": {…}` is a nested instance (same format as in scene files).
  `ParsePrefab` **expands** nested instances on load: member UUIDs from the list or derived stably
  (`DeriveUuid(instance root, source)`, splitmix64); per entity `PrefabOrigin{nest, source}`, per
  instance `PrefabNested{file, root, asset, map}`. Cache entries track transitive dependencies
  (mtime/size); `InvalidatePrefab` runs after every write. Cycles and depth > 16 are load errors.
- In a scene a nested instance still belongs to its outermost instance (one `PrefabInstance`), so
  capture/rebuild/scene files are unchanged. Changes of an inner prefab reach all levels except where
  overridden. `PrepareSceneFile` collects models of nested prefabs recursively.
- Writing (`WriteAndLink`) keeps instances nested; writing an instance root into another file creates
  a **variant** (base + overrides + own entities). A prefab that would contain itself is flattened at
  that spot with a warning.
- `PrefabLevels(scene, e)` (outermost first), `ApplyPrefabLevel(…, level, …)` (level 0 =
  `ApplyPrefabInstance`; inner levels transfer component values of existing entities only),
  `CreatePrefabVariant(file, base)`, `PrefabBase(file)`, `PrefabNestedFiles(file)`. Apply rebuilds
  all instances whose prefab is or contains the file.

## Level streaming

- Components: `LevelStreamingVolume{level, halfExtents, loadMargin, unloadMargin}` (hysteresis: load
  inside box + load margin, unload outside box + load + unload margin), `StreamingSource` (otherwise
  the camera is the source), `StreamedLevel{level}` (runtime, on the roots of a loaded sub-level;
  never saved).
- `LevelStreamer(jobs, assets?, events?)` (main thread): per level Unloaded → Preparing (parse on the
  pool) → Loading (models) → Ready → Loaded (entities created in one step) or Failed (retried after a
  new request or re-entering a volume). `Update(scene, fallbackSource)` evaluates volumes, advances
  states, activates in sorted key order and unloads unwanted levels (unload hook = `ScriptSystem::
  EndPlayFor`). `LevelStreamedEvent`, `UnloadAll`, `Reset`, `Levels()`. Cancelled parses become
  orphans; the destructor waits.
- `LevelLoader` (Open Level with a loading screen): `Begin / Update / Cancel / Take`, `Progress()`
  0.1 parse → 0.2 + 0.7 · ready models → 1.
- Cross-level references by UUID: entity variable overrides into a not-yet-loaded level stay pending
  and resolve on load; unloading makes them null.
- Player: sub-levels stream around the camera or a `StreamingSource`; Open Level loads in the
  background while the old level is paused, behind `ProjectSettings::loadingScreen` (a UI scene; a
  widget tagged `LoadingProgress` shows progress) or a built-in text + bar.
- Editor: edit mode only previews (read-only, not saved, `volumesEnabled = false`); Play streams
  around the editor camera. See [editor.md](editor.md).

## Decals over time

`UpdateDecals(scene, dt)` (`Scene/Decals.h`) ages decals with a lifetime and destroys them after the
fade-out. Rendering is described in [rendering.md](rendering.md#decals).

## Skeletal animation playback

- Models carry skeletons (joints, inverse bind matrices) and clips (`Assets/Animation.h`). The pose
  sampler supports STEP, LINEAR and CUBICSPLINE.
- `Animator` component on the `ModelInstance` root: clip, speed, loop/once, blend target, root
  motion; the first clip starts automatically. `UpdateAnimations` writes only changed poses and skips
  roots driven by an active `AnimationController` or a simulating ragdoll. `FindModelInstanceRoot`
  (nearest ancestor with `ModelInstance`) is used instead of stored pointers, so prefab instances and
  rebuilt entities animate too.
- GPU skinning runs in the mesh, depth and shadow passes (joint palettes ping-pong for motion
  vectors); per-frame skin bounds feed frustum and occlusion culling.
- Blueprint nodes: Play/Stop, Set Clip, Set Speed, Looping, Blend, Root Motion, Is Playing.
- State machines, blend spaces and IK are in [animation.md](animation.md).

## Runtime UI

- `UiCanvas` and `UiWidget` components (`UI/UiLayout.h`, `UI/UiSystem.h`): parent-relative anchors,
  offsets and pivot, a design resolution scaled into the viewport, stable z-order. Widget types:
  text, image, panel, button, checkbox, slider, progress bar.
- `UiSystem::Update(scene, UiInput)` handles mouse, keyboard and gamepad navigation and raises
  Blueprint events (On UI Clicked, On UI Value Changed, On UI Checked Changed). The player feeds
  `UiInputFromWindow`; the editor feeds viewport input while playing (clicks on widgets skip picking).
- The player's pause menu (Escape) and options menu are built with it (see [editor.md](editor.md)).
