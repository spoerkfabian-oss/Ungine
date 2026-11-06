# Ungine technical documentation

Reference documentation for engine developers. The user guide is the [root README](../README.md);
the working rules for contributors and AI agents are in [CLAUDE.md](../CLAUDE.md); the current
project state and lessons learned are in [MEMORY.md](../MEMORY.md); planned work is in
[ROADMAP.md](../ROADMAP.md).

| Document | Contents |
|---|---|
| [architecture.md](architecture.md) | Goals, technology stack, source layout, core runtime (application loop, events, thread pool, ECS, transforms, BVH), Vulkan layer (context, frames, bindless, uploads, data layouts) |
| [rendering.md](rendering.md) | SceneRenderer frame graph, PBR/IBL, shadows, GTAO, exposure, bloom, clustered lights, GPU-driven culling, LODs, TAA, decals, SSR, fog, picking, text overlay |
| [assets.md](assets.md) | AssetManager, models, textures, LODs, hot reload, revisions, VFS/pak, cooker, packaging |
| [scene.md](scene.md) | Scene files, snapshots, prefabs (nested, variants), level streaming, skeletal animation playback, runtime UI |
| [physics.md](physics.md) | Jolt integration, colliders, characters, events, queries, materials, joints, ragdolls |
| [audio.md](audio.md) | AudioEngine, buses, voices, AudioSystem, occlusion, reverb, sound cues, priorities, snapshots, ducking |
| [scripting.md](scripting.md) | Blueprints: graph model, types, VM, functions, macros, libraries, interfaces, inheritance, latent nodes, debugger, construction scripts |
| [particles.md](particles.md) | Particle effects, simulation, rendering, editor |
| [navigation.md](navigation.md) | Recast/Detour navmeshes, crowd agents, bake files |
| [animation.md](animation.md) | Animation state machines, blend spaces, IK |
| [terrain.md](terrain.md) | Terrain data, CDLOD rendering, foliage, physics/navigation, sculpting tools |
| [materials.md](materials.md) | Node-graph materials, code generation, runtime shader compilation, per-material pipelines |
| [editor.md](editor.md) | Editor, project browser, player, cook tool, projects, options, undo |
| [build-and-test.md](build-and-test.md) | Building, apps and command lines, sandbox, test suites, CI, release workflow |
| [verification.md](verification.md) | What was verified per phase, and how |
| [limitations.md](limitations.md) | Known limitations per system (not regressions) |
| [history.md](history.md) | Phase history |

Conventions used in these documents:

- Paths are relative to the repository root unless stated otherwise.
- "Main thread" means the thread that owns the `Application` loop; most engine APIs are main-thread
  only unless they say otherwise.
- Sizes of GPU structs are given in bytes (B) and must match the GLSL declarations.
