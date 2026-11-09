# Ungine

A C++20 / Vulkan 1.3 game engine with an Unreal/Unity-style editor, Blueprint visual scripting, Jolt
physics, 3D audio, navigation, animation, particles, terrain, node-graph materials and a player for
finished games.

- **Users:** this file (installing, building, working with the editor).
- **Developers:** [CLAUDE.md](CLAUDE.md) (rules and documentation map), [docs/](docs/README.md)
  (technical reference), [MEMORY.md](MEMORY.md) (current state), [ROADMAP.md](ROADMAP.md).

## Install

Every tag `v<version>` (e.g. `v0.37.0`) builds tested packages with GitHub Actions (*Release*
workflow) and attaches them to a GitHub release. Pushes to `main` and manual runs
(*Actions → Release → Run workflow*) keep them as workflow artifacts for 14 days.

| Package | Contents |
|---|---|
| `Ungine-<version>-windows-x64.exe` | Setup: Start menu and desktop shortcut **Ungine Editor**, `.ungineproj` files open in the editor on double-click, uninstall via "Apps & features" |
| `Ungine-<version>-windows-x64.zip` | Portable, no installation (`bin/UngineEditor.exe`) |
| `Ungine-<version>-linux-x64.tar.gz` | Unpack and run `bin/UngineEditor`; `bin/install-desktop-integration.sh` registers the menu entry, icon and file type |
| `SHA256SUMS.txt` | Checksums |

Running Ungine needs a Vulkan 1.3 driver (a current GPU driver). Before publishing, every package is
tested: Release CPU tests, then a project from the Basic template is cooked with the packaged
`UngineCook` (on Linux the cooked game is also started).

## Build from source

Requirements: CMake ≥ 3.25, Visual Studio 2022 (or GCC 13 / Clang on Linux), the Vulkan SDK (for
`glslc`) and a Vulkan 1.3 driver. All libraries are downloaded by CMake.

```
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

The Visual Studio start project is **UngineEditor**. Packages: `cpack -C Release` in the build
directory (NSIS setup if `makensis` is installed, plus a ZIP; on Linux a `.tar.gz`). The version comes
from `CMakeLists.txt` and can be overridden with `-DENGINE_PACKAGE_VERSION=…`. Linux builds, tests and
options: [docs/build-and-test.md](docs/build-and-test.md).

## Working with the editor

### Projects

1. Start **UngineEditor** → project browser: recent projects, a new project from a template (*Blank*,
   *Basic Scene*, *Physics Playground*) or *Open project…*.
2. A project folder holds `<Name>.ungineproj`, `Content/` (scenes, Blueprints, models, textures,
   sounds, …) and `Saved/` (caches and layout; not part of the game).
3. **Content browser:** double-click opens scenes/Blueprints/assets or places models; drag files into
   the viewport (models, prefabs, sounds, effects, terrains, materials onto meshes) or onto the
   selection (Blueprints). *+ New* creates folders, Blueprints, scenes and every asset type.
   *Import file…* copies models (`.glb`, `.gltf` with local files), textures and sounds into the
   current folder. The selection shows a texture, model/material or audio preview. Right-click offers
   *Move to folder…*, *Rename…* (references in scenes, Blueprints, prefabs and other assets are
   repaired) and *Delete* (asks first; cannot be undone).
4. **Play** (Ctrl+P) simulates physics, Blueprints, audio, particles and AI in the editor; *Stop*
   restores the scene. Undo (Ctrl+Z) also covers renderer, physics and project settings and the
   viewport view (overlays, debug view, camera fields, focus with F); view and project settings do not
   mark the scene as changed.
5. **Build → Build & Run** (Ctrl+B) saves everything and starts the game in **UnginePlayer**.
   **Build → Package project…** creates a standalone game folder (`<Name>.exe`, shaders, project,
   `Content.upak` = cooked content, `BuildReport.txt`); the *Package report* window lists warnings
   (missing references) and errors. Without the editor: `UngineCook <project> <target>`.
6. The player renders through the primary **Camera** component of the start scene (*File → Project
   settings…*). In the editor, *Game cam* shows that view.

### Blueprints (visual scripting)

- **Basics:** in the *Blueprint* tab, right-click searches nodes, drag pins to connect, variables on
  the left, details below. Attach a script: Inspector → *Add component* → *Script (Blueprint)*.
- **Functions and variables:** *Graphs* → *+ Function* (inputs, outputs, local variables, *Pure*);
  call them through the node search (*Call …*). Variables can be arrays, maps, enums, structs and
  nested containers (arrays of arrays, maps of arrays, …). The checkbox next to a variable makes it
  *instance editable* (value per entity in the inspector, also references to other entities).
- **Inheritance:** right-click a Blueprint in the content browser → *Create child blueprint* (or
  *Class → Parent*); override inherited functions with *Override*, set defaults of inherited variables
  under *Inherited variables*, call the parent version with *Call Parent*. *Is A* / *Cast To* check
  classes. Renaming a variable or function updates child classes; *Move to parent class* moves it up.
  Functions may call themselves (recursion).
- **More building blocks:** *+ New* → *Enum*, *Struct*, *Interface* (edited in the *Blueprint types*
  window) and *Blueprint library* (functions/macros for every Blueprint, called as
  `Library.Function`). In the side bar: *+ Macro* (several exec pins, delays allowed), *Custom Events*
  with parameters, *Event Dispatchers* (Call/Bind; parameters must match), *Interfaces*, *Timelines*
  (curve editor: double-click adds a key, drag moves it). Nodes include *Switch*, *Select*, *Tween*
  (Move/Rotate/Scale To), *Save Game*, *Open Level*, *Input Actions* (Project Settings → *Input*),
  math/vector/string/array utilities and *Async Load Model*. Latent nodes such as *Delay* and
  *AI Move To* also work inside functions.
- **Construction Script** event: runs in the editor after every change and before BeginPlay; what it
  creates is not saved (grey in the hierarchy).
- **Debugger:** F9 sets a breakpoint (optional condition such as `health < 10` and hit count in the
  details panel); in Play the game halts there; F5 continues, F10 steps over, F11 into, Shift+F11 out;
  values and the call stack are in the *Debug* section and pin tooltips.
- **Comfort:** double-click a link = reroute node, Shift+W/A/S/D aligns, M toggles the minimap, C wraps
  the selection in a comment, Ctrl+F searches all Blueprints, right-click → *Collapse to function /
  macro*.

### Prefabs

Hierarchy → right-click → *Create prefab…* saves a subtree as `.uprefab`. Place prefabs by
double-click or drag & drop; changes on an instance are *overrides* (revert one by one in the
inspector), *Apply to prefab* writes them to all instances, *Unlink* breaks the connection. Prefabs
can be nested (the inspector shows the levels under *From*; *Apply to…* writes into the outer or an
inner prefab). *Variants* (*Create variant* in the content browser, or *Create prefab variant…* on an
instance) take every change of their base except their own overrides. Blueprints spawn prefabs with
*Spawn Prefab*; *Print String* output appears in the top-left corner of the player.

### Physics

Add *Rigid Body*, *Collider* and *Character Controller* components in the inspector. *+ New → Physics
material* creates a `.uphysmat` (friction, bounciness, surface name, sound absorption) for colliders,
or per glTF material on mesh colliders. Raycasts and collision events report surface, contact point,
normal and impulse (*On Hit* with a minimum impulse, *On Collision Stay* every step). *Add component →
Joint* connects two bodies (Fixed, Hinge, Slider, Ball, Distance, Cone, Swing-Twist, 6DOF) with limits,
motor and break force; the *Anchor* toolbar button moves the joint frame with the gizmo. *Add
component → Ragdoll (from skeleton)* on an animated model creates bones with capsules and joints;
*Simulate* (or Blueprint *Set Ragdoll Simulating*) lets it fall. Characters support rotation modes,
crouching, moving platforms and sliding on steep slopes.

### Audio

Copy sounds (WAV/OGG/MP3/FLAC) into `Content/`. Double-click previews them; dragging one into the
viewport creates an **Audio Source** (3D, loop, bus, range, occlusion). **Reverb Zone** and **Audio
Listener** via *Add component*. *+ New → Sound cue* creates a `.ucue` with weighted variants, random
volume/pitch, no direct repeats, priority and an instance limit; a cue works wherever a sound does.
Renderer panel → *Audio*: mixer (Master/World/Music/UI/Ambient), occlusion (several rays; walls damp by
their physics material), voice limits, **snapshots** (named bus mixes, blended by Blueprint) and
**ducking** (e.g. UI lowers music); saved with the project. Blueprint category *Audio*; paths are
relative to the project folder, e.g. `Content/Sounds/impact.wav`.

### Particles

*+ New → Particle effect* creates a `.uparticle` and opens the particle editor: emitters with module
stacks (Spawn, Initialize, Update, Events — e.g. spawn rate, shape, velocity, gravity, curl noise,
color/size over life, collision, sub-emitters) and render settings (sprites with sprite sheets and soft
edges, meshes, ribbons for trails; alpha or additive). The preview and the scene update live; *Save*
writes the file. Place an effect by double-click or drag & drop (component *Particle System*);
Blueprint nodes *Spawn Emitter at Location*, *Play/Stop Particles*, *Set Particle Scale/Tint*.
Examples: campfire (Basic), spark fountain with collision and trails (Physics).

### Navigation (AI)

Hierarchy → *+ Add* → *Nav Mesh Bounds* around the walkable area: the navmesh is built from the static
colliders (and terrains) inside and follows every change (*Navigation* window: overlay, *Bake*, test
path via *Pick start/end* + click). It is saved as `.unav` next to the scene. *Nav Area* marks surfaces
(cost, *Blocked*), *Nav Link* connects points (jumps, ladders), *Nav Obstacle* cuts out moving objects.
Characters get a *Nav Agent* and walk with the Blueprint node *AI Move To* (they avoid each other; with
a Character Controller through physics). Agent profiles and area types: *File → Project settings →
Navigation*. Example: the wanderer in the Physics template.

### Animation

Imported animated models play their first clip automatically; the **Animator** inspector selects
clip, speed, looping, blend target and root motion. *+ New → Animation graph* (`.uanim`) opens the
*Animation Graph* window: parameters (Float/Int/Bool/Trigger), layers (bone mask, override or
additive), states (a clip or a 1D/2D blend space; right-click the canvas), transitions (right-click a
state; conditions, blend time, exit time), notifies (sound/effect at a bone) and IK targets (feet on
the ground, two-bone, look-at, FABRIK). Assign it with *Assign to selection* (component *Animation
Controller*); drive it with *Set Anim Float/Bool/Trigger*, react with *On Anim Notify*. Example: the
fox in the Physics template.

### Runtime UI

Add a *UI Canvas* and child *UI Widget* components: parent-relative anchors and offsets with a design
resolution that scales to the game view. Text, images, panels, buttons, checkboxes, sliders and
progress bars; Blueprint events *On UI Clicked*, *On UI Value Changed*, *On UI Checked Changed*. The
player opens its pause menu with Escape (keyboard, mouse and gamepad navigation).

### Levels and streaming

Hierarchy → *+ Add* → *Streaming Volume*, choose the level file in the inspector. In the game the
sub-level loads additively when the camera (or an entity with *Streaming Source*) enters the box and
unloads after leaving the box plus the *Unload margin*. Blueprints: *Load/Unload Stream Level*, *Is
Level Loaded*, *Level Loaded/Unloaded* events; references to entities in an unloaded level are empty
and become valid again when it loads. The **Levels** window shows read-only previews (edit a level by
opening its file). *Open Level* loads in the background behind a loading screen; customize it under
*Project Settings → Loading screen* (a scene with a UI canvas; a progress bar tagged `LoadingProgress`
shows the progress).

### Options and save games

The player's pause menu has *Options*: graphics (fullscreen, VSync, shadow quality, AO, bloom), audio
(bus volumes) and controls (rebind keys with a conflict hint, reset), saved per user
(`Settings.json`). Blueprint categories *Options* and *Input → Remap Action*. *Project Settings → Save
version* versions save games: older slots fire *Migrate Save Game* when loaded; slots of a newer game
version are refused (not read, not overwritten).

### Rendering features

The renderer panel holds lighting, post processing, shadows, AO, *Anti-aliasing (TAA)* and
*Reflections (SSR)* (both on by default) and *Fog*. *+ Add → Decal* projects textures (albedo,
normal, mask, emissive) along −Z onto surfaces in its box (*Sort order*, *Lifetime*; Blueprint *Spawn
Decal at Location*). *Fog* enables height fog; *Volumetric* adds light shafts from the sun and lamps
(with shadows); *+ Add → Fog Volume* places local (optionally glowing) fog boxes. All settings are saved
with the scene; the *Volumes* toolbar toggle shows decal and fog boxes.

### Terrain

*+ Add → Terrain* (or *+ New → Terrain*; place the `.uterrain` by double-click or drag & drop) creates
a 128 m terrain. The *Terrain* window (with a terrain selected) has brushes *Raise/Lower/Smooth/
Flatten* (Shift+click picks the flatten height), *Paint* (choose the layer in the list), *Holes* and
*Foliage*; Ctrl inverts; every stroke is one undo step. Layers have albedo/normal/ORM textures, tint and
tiling; steep slopes are projected triplanar. Foliage layers are procedural grass blades, texture cards
or meshes (density, size, wind, view distance). Heightmaps (16-bit PNG/RAW) via *Import/Export*. The
terrain collides, feeds the navmesh and is saved with the scene; Blueprint *Get Terrain Height* returns
height and normal.

### Materials

*+ New → Material* creates a node graph (`.umat`), *Create instance* (or *+ New → Material instance*)
an instance (`.uminst`) that overrides parameters. In the *Material* window: right-click the canvas
for the node palette (type, Enter), drag pins to connect, Alt-click disconnects, Delete removes; on the
left blend mode (Opaque/Masked/Translucent/Additive), Lit/Unlit, two-sided, parameters
(Scalar/Vector/Texture) and the selected node's values; on the right the preview (sphere/box/plane).
*World Position Offset* moves vertices (set *Max displacement* for culling). Assign a material by
dragging it onto a mesh in the viewport or into *Mesh Renderer → Materials* per submesh; parameters
can be overridden per object. Blueprints: *Set Material*, *Set Material Scalar/Vector Parameter*,
*Clear Material Parameters*. Shaders are compiled on first load (cached in `Saved/Cache/Shaders`); the
Basic template shows a pulsing example material.

## Development

`Sandbox` is the engine's demo and test application. Tests: `EngineTests` (CPU), `EngineGpuTests` and
smoke tests via CTest with `-DENGINE_GPU_TESTS=ON`. See [docs/build-and-test.md](docs/build-and-test.md)
and [CLAUDE.md](CLAUDE.md).

## Licenses of bundled content

Sample models are Khronos glTF sample assets under CC0 or CC-BY 4.0 (see
[assets/models/LICENSE.md](assets/models/LICENSE.md)); sample sounds are synthesized and CC0 (see
[assets/sounds/LICENSE.md](assets/sounds/LICENSE.md)).
