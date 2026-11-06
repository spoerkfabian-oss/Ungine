# Physics

`PhysicsWorld(ThreadPool&, EventBus&, const AssetManager*)` wraps Jolt Physics v5.6.0 (pimpl; Jolt
appears only in `PhysicsWorld.cpp`, `PhysicsMaterial.cpp` and `Ragdoll.cpp`). Jolt is initialized
globally with a refcount; Jolt objects (group filters, materials) must be created after the Jolt
allocator is registered and released before it is.

## Components

- `RigidBody`: Static / Kinematic / Dynamic, mass, damping, gravity factor, allow sleeping,
  `continuous` (CCD via LinearCast), user layer.
- `Collider`: Box (half extents), Sphere (radius), Capsule (radius + half height along Y), Mesh;
  center, `rotation` (shape orientation), `trigger`, `material` (`.uphysmat`) and, for mesh colliders,
  `meshMaterials` (glTF material name → `.uphysmat`). Friction/restitution fields apply only without a
  material.
- `CharacterController`: radius, height, max slope, step height, jump speed (feet = entity
  position; takes precedence over a RigidBody), rotation mode (None/Movement/Camera/Script) + turn
  speed, crouch height, mass, push strength, slide on steep slopes, moving platforms.
- `Joint` + `JointMotor`, `Ragdoll` + `RagdollBone` (see below).
- All components implement `operator==` (change detection) and are serialized and part of undo
  snapshots.

## World

- Job system: `JobSystemWithBarrier` on the engine ThreadPool (jobs via `Enqueue`; the main thread
  works inside the barrier wait, so a busy pool cannot stall it; the destructor waits for pool
  closures). Limits: 65 536 bodies/pairs, 20 480 contacts, 32 MB temp allocator.
- Layers: object layer = `userLayer << 1 | moving`; a collision matrix in `PhysicsSettings`
  (`SetLayerCollision`/`LayersCollide` validate indices); static vs. static never collides.
- **`Sync(scene)`**: `UpdateTransforms`, then O(bodies) over the physics views. Bodies are (re)created
  on component, model or world-scale changes. Scale goes into the shape (box/mesh per axis, sphere/
  capsule by the largest axis; `center`/`rotation` via RotatedTranslatedShape). Degenerate scales
  (≈ 0, NaN, Inf) refuse the body (retry every sync, one warning per entity); negative scale is
  ignored (`abs`). Externally moved entities: static → teleport + wake bodies in the bounds,
  dynamic → teleport + zero velocity, kinematic → `MoveKinematic` during the step (pushes dynamics).
  Mesh colliders use the model's collision triangles (unscaled MeshShape cached per model + mesh,
  `ScaledShape`), wait until the model is Ready, and turn dynamic bodies static.
- **`Step(scene, dt)`**: Sync → kinematic moves → characters (`CharacterVirtual::ExtendedUpdate`:
  stairs, stick to floor, jump, air control, inner body for interaction) →
  `PhysicsSystem::Update(collisionSteps)` → write back active dynamics (world → local relative to the
  parent's world matrix, scale kept; finite checks refuse invalid poses) and characters, depth-sorted
  for hierarchies → `UpdateTransforms` → contact events.
- **Interpolation:** `Interpolate(scene, alpha)` blends between the last two steps (flag for bodies
  that fell asleep in between). The editor renders with the fixed-step alpha; pause uses 1.
- Physics settings (gravity, collision steps, air control, layer matrix, persist events,
  interpolation) are saved in scene files.

## Events and contact data

- `CollisionEvent{a, b, begin, trigger, contact}` published at the end of Step/Sync: counted per body
  pair (sub-shape contacts), Begin 0 → 1, End → 0; sleeping pairs are "suspended" instead of ended;
  body removal ends (the entity may already be destroyed).
- `ContactInfo{point, normal (a → b), relativeVelocity, approachSpeed, impulse, surfaceA, surfaceB}`.
  Samples are collected on Jolt threads and merged per pair on the main thread (Begin: strongest new
  contact; Persist: deepest contact of the step). The impulse is an estimate:
  approachSpeed · (1 + e) / (invMassA + invMassB). Characters report the same data (diff of
  `GetActiveContacts`).
- `CollisionPersistEvent` every step for touching pairs (`PhysicsSettings::persistEvents`).
- `JointBrokenEvent` when a joint breaks.

## Queries and control

`Raycast`, `SphereCast` (triggers ignored, one entity excluded, normal faces the query, layer mask;
hits report `surface` and `absorption`), `AddImpulse(At)`, `SetLinearVelocity`, `LinearVelocity`,
`SetCharacterInput(move, jump)` (jump is one-shot), `SetCharacterCrouch`, `SetCharacterYaw`,
`GetCharacterState` (ground state, ground velocity/entity, crouching, yaw), `Activity`, `HasBody`,
`Stats()`, `ForEachCollider` / `ForEachJoint` (debug drawing), `Reset()` (everything removed, no
events), `FitCollider(shape, min, max)`. Jolt queries are read-only thread-safe (particles raycast
from jobs).

## Physics materials

`.uphysmat` (JSON `{version, friction, restitution, surface, absorption}`; values clamped;
`Load/SavePhysicsMaterial` through the VFS). Jolt `SurfaceMaterial` per file; mesh shapes use a
`PhysicsMaterialList` per submesh. Combination: friction = sqrt(f1 · f2), restitution = max, applied
per sub-shape in the contact listener. Hot reload by mtime (1/s); a missing file logs a warning and
the body runs without material; repairing it rebuilds the shapes. `absorption` (default 0.6) is used
by audio occlusion.

## Joints

- Types: Fixed, Hinge, Slider, Ball, Distance, Cone, SwingTwist, SixDof. Body A = the entity (or
  `ownerBody` UUID), body B = `connectedBody` UUID (0 = world). Frame = entity world × (anchor,
  anchorRotation); X = hinge/slider/twist axis, Y = normal.
- Limits (min/max, cone/plane, soft limits with spring/damping), friction, motors (`JointMotor`
  Off/Velocity/Position, max force, spring; hinge, slider and swing-twist twist only; changed live via
  `ApplyMotor`), 6DOF axes Free/Limited/Locked with ranges, `breakForce/breakTorque` (lambda per
  substep → `enabled = false` + event), `collideConnected` (otherwise a `JointGroupFilter`).
- **Jolt order: body 1 = B, body 2 = A** — angles, positions and motors are A relative to B.
- Rebuilt on component changes (except motors), new body IDs, a teleported body, and in edit mode on
  a changed frame. Missing bodies → pending (`Stats().pendingJoints`). `GetJointState` (active,
  angle/position, force/torque). Joint UUIDs are remapped on duplicate, prefab and level
  instantiation.

## Ragdolls

`CreateRagdoll(scene, root, model, settings)` turns skin joints with entities into `RagdollBone`s:
`RigidBody` (mass by volume), a capsule along the bone (to the middle of the child bones; leaves get
half the parent length) and a SwingTwist joint to the parent bone (no collision between them).
`Ragdoll{simulate, mass}` on the root: simulating = dynamic (animation paused), otherwise kinematic
(follows the animation). Switching keeps the velocity. `RemoveRagdoll`, `SetRagdollSimulating`,
`RagdollBones`.

## Blueprint nodes

On Collision Begin/End/Stay (Other, Is Trigger, Point, Normal, Impulse, Surface, Other Surface),
On Hit (text parameter "Min Impulse"), On Joint Broken (Joint, Body A, Body B; fired on the joint
entity and both bodies), Raycast (incl. Surface), impulses/velocity, character input/crouch/yaw/state,
Set Joint Enabled, Connect Joint, Set Joint Motor, Get Joint State, Create Ragdoll, Set/Is Ragdoll
Simulating. Persist events reach scripts only for pairs with a script instance.

## Editor integration

Play/Pause/Step/Stop with snapshot restore (see [editor.md](editor.md)); edit mode calls `Sync` every
frame so bodies follow gizmo and inspector edits. Inspector sections for every component (Fit to
mesh, materials per glTF material, joint limits/motors, ragdoll rebuild), "Colliders" overlay (static
grey, kinematic yellow, active green, sleeping blue, character cyan, trigger magenta; joints with
frames, arcs and cones), "Anchor" toolbar mode to move the joint frame with the gizmo, PHYS content
type with a Physics material window, renderer panel "Physics".
