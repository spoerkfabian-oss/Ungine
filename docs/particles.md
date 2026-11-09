# Particles

## Effect data (`Particles/ParticleEffect.h`)

`.uparticle` (JSON v1): `warmup` and emitters `{name, enabled, maxParticles (≤ 100 000), localSpace,
duration (0 = endless), loop, delay (first cycle only), modules [{type, enabled, fields}], render}`.

Modules are a `std::variant` of 22 parameter structs in four stages:

| Stage | Modules |
|---|---|
| Spawn | SpawnRate (scale curve over the duration), SpawnBurst (cycles, interval, random count), SpawnPerDistance |
| Initialize | Shape (point, sphere, hemisphere, box, cone, circle; surface only), Velocity (cone spread, radial, inherit emitter velocity), Lifetime, Size (uniform or per axis), Color (random between two colors × HDR intensity), Rotation + rate, SpriteFrame |
| Update | Gravity, Drag, Force/Wind (world or local), CurlNoise (curl of a sine vector potential, divergence-free), Attractor (falloff, kill radius), SpeedLimit, SizeOverLife (curve), ColorOverLife (gradient), SubUv (over lifetime or fps), Collision (physics raycast; restitution, friction, kill, max hits, layer mask), KillVolume (box/sphere, inverted) |
| Events | SubEmitter on Spawn/Death/Collision/Trail → another emitter of the same effect (count or rate, velocity inheritance; trail particles carry the parent's ID as ribbon ID) |

- `ParticleCurve` (Linear/Smooth/Constant keys), `ParticleGradient` (linear RGBA keys).
- `ParticleFields.h`: `VisitParticleFields(module, visitor)` with hints (range, degrees, color,
  emitter name) — one field list drives both JSON and the editor UI.
- Loading clamps values, skips unknown modules and makes emitter names unique.
  `ValidateParticleEffect` reports missing sub-emitter targets, cycles, emitters without spawn
  modules and mesh renderers without a mesh. Texture/mesh paths are absolute in memory and relative to
  the effect file on disk (also across folders with `..`).
- **Render settings** (`ParticleRender`): Sprite/Mesh/Ribbon/None, alpha or additive, texture
  (empty = soft round sprite), sprite sheet columns × rows, alignment Camera/Velocity (stretched per
  m/s)/Axis, soft-edge distance (scene depth), camera fade, sorting, mesh (file + index, align to
  velocity, simple sun/sky shading), ribbon UV stretch/tile.

## Simulation (`ParticleWorld`)

`ParticleWorld(jobs?, assets?, physics?)`, main-thread API:

- `Update(scene, dt)` syncs `ParticleSystem` components (auto-play starts, path or seed changes
  restart, scales/tint are live), simulates all instances in parallel (`ParallelFor`: jobs via
  `Enqueue`, the main thread works along; context held by `shared_ptr`), builds world-space render
  data and removes finished one-shots.
- Per emitter: update (age, forces, motion, collision via `PhysicsWorld::Raycast` from the jobs,
  kill volumes, trail events) → spawn (sub-emitter requests, rate/distance spread along the emitter's
  motion with sub-frame age, bursts). Long frames are split into steps ≤ 0.1 s; warmup runs in
  1/30 s steps.
- PCG32 per instance (seed from the component or sequential; `SetRandomSeed` for tests) → results
  are identical with and without jobs.
- API: `Play / Stop(immediate) / IsActive / ParticleCount`, `SpawnAt` (one-shot, returns an ID) /
  `StopOneShot`, `Reset`, `SetPaused`, `Effect(path)` (cache, mtime check once per second, restart
  only when content changed), `Provide` (unsaved editor version), `Stats()`. Textures and models are
  acquired once per path until the destructor (destroy before the AssetManager).

## Rendering (`ParticleRenderer`, private to the SceneRenderer)

Drawn after transparent meshes in the lighting pass (HDR, depth read-only and sampled for soft
particles). Batches are frustum-culled and sorted back to front; alpha particles are sorted by
distance per emitter. Data is copied into host-visible buffers per frame slot (growing, freed
deferred). Pipelines for alpha and additive (premultiplied, ONE/ONE): sprites (6 vertices per
instance, billboard in the vertex shader, flipbook UV), ribbons (6 vertices per segment, width across
tangent and view direction), mesh particles (geometry pool index buffer, `vkCmdDrawIndexed` per
submesh, axis/angle or velocity alignment, material base color × particle color, back-face culling,
no depth writes). Particles are unlit except for emissive color and fog. Stats: batches, particles
drawn, draw calls.

## Component and Blueprint nodes

`ParticleSystem{effect, autoPlay, speed, seed, spawn/size/velocity/lifetime scale, tint}` (JSON
`particleSystem`, path relative to the scene, undo, prefab overrides, reference repair, cooker).

Nodes (category Particles): Spawn Emitter at Location (effect, location, rotation°, scale → handle),
Stop Spawned Emitter, Play/Stop Particles, Is Particles Active (+ count), Set Particle Scale
(spawn/size/velocity/lifetime/speed), Set Particle Tint.

The cooker cooks effect textures as Color KTX2 and warns about missing or outside textures/meshes and
validation problems.

## Editor

- `EditorContext::particles`: effects simulate in edit mode too; Play/Stop/New/Open reset the world;
  pause and debugger halts pause it.
- **Particle Effect** window (`ParticleEditor`): toolbar (Save Ctrl+S, Revert, Undo/Redo, Restart,
  Pause, Speed, Ground), warmup, emitter list (enable, select, reorder, duplicate, delete, rename
  updates sub-emitter references), stage sections with "+" module picker, module stack (enable,
  reorder within the stage, remove, generic field widgets with range drags, HDR colors, curve widget,
  gradient widget), render section (texture/mesh via "…" or drag & drop), problems in yellow; live
  preview with its own scene, SceneRenderer and ParticleWorld (orbit camera, ground toggle, stats).
- Changes go live to the preview and, via `Provide`, to the scene (unsaved); an undo step is recorded
  when no widget is active. Saving does not restart running instances. Opening another effect with
  unsaved changes is refused. Save all and the quit prompt include it.
- Inspector "Particle System", Add component, Hierarchy "+ Add → Particle System", content type PFX
  (+ New → Particle effect, double-click opens, place/assign, drag & drop into the viewport), viewport
  overlay "FX", stats section.
- Template effects: `Campfire.uparticle` (Basic), `SparkFountain.uparticle` with collision and trails
  (Physics).
