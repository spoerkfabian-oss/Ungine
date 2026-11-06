# Ungine roadmap

Each phase closes a documented engine gap as a verifiable, separately deliverable unit: one pull
request from `branch` (unless the owner asks to stack phases), with documentation updated in the same
PR. A phase is done only when the data format, runtime, editor/player path, error behavior and tests
exist.

## Rules for every phase

- Check existing interfaces first, then implement vertically: data format → runtime → editor/player →
  documentation.
- Record dependencies and acceptance criteria before starting; ask the owner about real ambiguities.
- No silent data-format breaks: new scene/asset fields are optional when reading.
- When a GPU or Windows runtime is not available, list the unverified paths when delivering.

## Completed phases

Phases 1–20 built the engine core, renderer, asset pipeline, physics, audio, editor, projects and
Blueprints (see [docs/history.md](docs/history.md)). The roadmap phases since then:

| Phase | Scope | State |
|---|---|---|
| 21 | Skeletal animation: glTF skins and clips, animator (clip, loop, speed, blend, root motion), GPU skinning with skin bounds, Blueprint nodes, animated banner in the Basic template | Merged (PR #11, repaired in #12) |
| 22 | Runtime UI: canvas/widgets with anchors and scaling, text/image/panel/button/checkbox/slider/progress bar, mouse/keyboard/gamepad navigation, Blueprint events, pause menu | Merged (PR #11/#12) |
| 23 | Content import and previews, reference repair on move/rename, safe delete | Merged (PR #11/#12) |
| A | Hardening: GPU tests for 21–23, randomized lifecycle audits and fixes | Merged (PR #13) |
| 24 | Level and area streaming: async scene loading, streaming volumes, Blueprint load/unload, UUID cross-references, async Open Level with loading screens, editor Levels window | Merged (PR #14) |
| 25 | Cooked builds: VFS + pak, cooker, `UngineCook`, packaged player; savegame versions with migration; options menu with input remapping | Merged (PR #15) |
| 26 | Physics and character tools: physics materials (per triangle via glTF materials), contact data and events, joints with limits/motors/breaking, ragdolls, character rotation/platforms/crouch/sliding | Merged (PR #16) |
| 27 | Audio: sound cues, voice priority and limits, snapshots, ducking, multi-ray occlusion with material absorption (HRTF deliberately left out) | Merged (PR #17) |
| 28 | Particles: module stacks, deterministic CPU simulation, sprites/meshes/ribbons, sub-emitters, collision, particle editor | Merged (PR #18) |
| 29 | Navigation: Recast/Detour tiled navmeshes per agent profile, runtime rebuilds, bake files, areas/links/obstacles, DetourCrowd agents, AI Blueprint nodes, editor tools | Merged (PR #19) |
| 30 | Animation state machines: `.uanim` graphs, blend spaces, layers with masks, notifies, IK (two-bone, foot placement, look-at, FABRIK), Fox example | Merged (PR #20) |
| 31 | TAA with motion vectors, clustered decals, Hi-Z SSR, height and volumetric fog | Merged (PR #21) |
| 32 | Terrain: CDLOD, splat layers with triplanar, holes, foliage, height-field physics, navmesh source, sculpt/paint tools with undo | Merged (PR #21) |
| 33 | Material editor: node-graph materials, instances, blend/shading models, world position offset, per-submesh overrides, runtime shader compilation | Merged (PR #22) |
| 34 | Nested prefabs and variants; undo for renderer/physics/project settings and the editor view | Merged (PR #23) |
| 35 | Blueprints: inheritance like UE (parent classes, defaults, overrides with Call Parent, Is A / Cast To), recursion, nested containers | Merged (PR #24) |
| 36 | Blueprints: latent nodes in functions, dispatcher signature checks, refactoring (rename in child classes, move to parent, class paths on move), ~55 utility nodes, async model loading | Merged (PR #24) |
| 37 | Installers per CI: release workflow building and testing the Windows setup + ZIP and the Linux TGZ, GitHub releases on tags | Merged (PR #24); NSIS fix in PR #25 |

## Candidates for future phases

Not scheduled; to be discussed with the owner:

- Verification on real hardware (GTX 1070 Ti, queue ownership transfer path) and on Windows (installer,
  file association, process launch) — highest priority, needs the owner's machine.
- Rendering: meshlets/cluster culling, Hi-Z for shadow views, mip streaming, a deferred path, PCSS,
  area lights, temporal AO, TAA upscaling.
- Content: material functions and static switches, custom materials on terrain/foliage/particles,
  GPU particles, mesh LODs for skinned meshes, morph targets, retargeting.
- Tools: undoable delete in the content browser, IK and nav-link gizmos, bone overlay, a progress UI for
  packaging, compressed/signed paks and patching.
- Known limitations per system are listed in [docs/limitations.md](docs/limitations.md).
