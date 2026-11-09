# Animation state machines and IK

Clip playback and GPU skinning are described in [scene.md](scene.md#skeletal-animation-playback).
This document covers animation graphs (`engine/include/Engine/Animation/`).

## Animation graph data (`AnimGraph.h`, `.uanim` JSON v1)

- **Parameters:** Float, Int, Bool, Trigger (with defaults).
- **Layers:** Override or Additive, weight (+ optional weight parameter), bone mask with
  `includeChildren`, entry state.
- **States:** Clip, BlendSpace1D or BlendSpace2D; speed (+ speed parameter), loop, samples
  `{clip, position, speed}`, notifies `{time, name, sound, effect, bone, volume}`, canvas position.
- **Transitions:** from (empty = Any State), to, duration, conditions (Greater/Less/…/IsTrue/IsFalse/
  Triggered), `hasExitTime`/`exitTime` (unwrapped normalized time), `canInterrupt`.
- **IK targets:** TwoBone, FootPlacement, LookAt, Fabrik; weight or weight parameter.
- `rootMotionBone`, `previewModel`. Paths are absolute in memory, relative to the graph on disk.
- `MakeDefaultAnimGraph`, `Load/Parse/SaveAnimGraph`, `ValidateAnimGraph(graph, model?)` (unknown
  parameters/states/clips/bones, duplicate names, blend-space samples).

## Player (`AnimGraphPlayer`)

- `Bind(graph, model)` → one state machine per layer. Any-State transitions are checked first;
  triggers are consumed by the transition that fires.
- Linear crossfades; an interrupted transition blends from a frozen pose.
- 1D and 2D blend spaces are phase-synchronized (rate = Σ w · speed / duration); 2D uses a
  Bowyer-Watson Delaunay triangulation (outside the hull: nearest edge).
- Additive layers are relative to the rest pose; masks per node.
- Notifies fire once per crossing per loop (≤ 8 per update). Root motion = motion of the root-motion
  bone in its parent space (the bone itself stays at rest).
- `LayerStatus`, `BlendWeights`, `DrivenNodes`.

## IK (`AnimIk.h`, model space via `PoseSpace`)

- **Two-bone:** analytic, pole vector or current bend, `keepEndRotation`.
- **FABRIK:** chains keep their bone lengths.
- **Look-at:** rotation distributed over the chain, `maxAngle`.
- **Foot placement:** physics raycast down per foot (the actor's own bodies are ignored), animated
  foot height relative to the ground is kept, pelvis lowered to the lowest foot (clamped), feet tilted
  to the ground normal, smoothed over time.

## Component and system

- `AnimationController{graph, enabled, speed, applyRootMotion, ik}` on the `ModelInstance` root (JSON
  `animationController`, path relative to the scene, undo, prefab overrides, reference repair).
  `UpdateAnimations` skips roots with an active controller.
- `AnimationSystem(assets | resolver, physics?, events?)`: graph cache per path (mtime once per
  second, `Provide` for unsaved editor versions), parameters remembered before binding, `Prepare`
  binds immediately (name checks for scripts), IK raycasts ignore the actor (nearest ancestor with a
  CharacterController or NavAgent), root motion → character input or entity transform, notifies →
  `AnimNotifyEvent` (+ sound/particles with `fireNotifyActions`), `SetFloat / SetBool / SetInt /
  SetTrigger / SetLayerWeight / SetIk*`, `Inspect`, `Stats`.

## Blueprint nodes (category Animation)

Set Anim Float/Bool/Int/Trigger, Get Anim Parameter, Get Anim State, Set Anim Layer Weight,
Set/Clear IK Target (position or entity), Set IK Weight/Enabled, event On Anim Notify (name, state,
location; fired on the controller and its ancestors). The target is the entity itself or the first
controller below it. The cooker checks notify sounds/effects and reports validation problems.

## Editor

- **Animation Graph** window (`AnimGraphEditor`): toolbar (Save, Revert, Undo/Redo, Restart, Pause,
  Speed), side bar (graph, preview model, root-motion bone, parameters with preview values and Fire,
  layers, IK targets, problems), state machine canvas (right-click: state/transition, drag, entry, Any
  State), details (state: clip or blend space with plot and samples, notifies; transition: duration,
  exit time, conditions), live preview (own scene + SceneRenderer + AnimationSystem, orbit camera).
- Changes go live to the scene via `Provide`; one undo step per commit. Inspector "Animation
  Controller" (live layer states and parameters while playing), Add component, content type ANIM
  (+ New → Animation graph, double-click, Assign to selection), stats section.
- Edit mode does not animate (only Play and the graph window preview).
- Example: the fox in the Physics template (`Animations/Fox.uanim`: locomotion blend space
  Survey/Walk/Run by `Speed`, step notifies, foot IK; `Wander.ugraph` sets `Speed`).
