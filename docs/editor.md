# Editor, player, cook tool and projects

The engine core does not depend on ImGui; everything editor-related lives in the `Editor` library
(`editor/`) and the apps (`apps/`).

## Projects (`Core/Project.h`, `Core/Platform.h`)

- `.ungineproj` (JSON v1): name, start scene (relative), window size/fullscreen/vsync, audio settings,
  input map, navigation settings, save version, loading screen.
- `Project::Create(location, name, template)` copies the template (everything except
  `template.json`), creates `Content/` and `Saved/`. `Load / Save / Relative / StartScene /
  ContentDirectory / SavedDirectory`, `ProjectTemplates()` (each folder in `TemplateDirectory()` with
  a `template.json`), recent list (`recent_projects.json`, max 20, missing files dropped).
- Platform helpers: executable path/directory, `UserConfigDirectory` (`%APPDATA%/Ungine` or XDG),
  `HomeDirectory`, `LaunchProcess` (CreateProcessW / posix_spawn; own children are reaped),
  `OpenInFileBrowser`, `SiblingExecutable`, `ShaderDirectory()` (env `UNGINE_SHADER_DIR` →
  `<exe>/shaders` → build tree), `TemplateDirectory()` (`<exe>/templates` → source tree),
  `CommandLineUtf8` (Windows: `CommandLineToArgvW`), UTF-8 path helpers, `Log::OpenFile`.
- `Window`: fullscreen/maximized in `WindowDesc`, `SetFullscreen`, `SetSize`, `SetIcons`, procedural
  `EngineIcon`, `CancelClose`.

## UngineEditor (`apps/editor`)

- Without an argument the **project browser** (`ProjectLauncher`, its own `Application`) opens: recent
  projects with a context menu, new project from a template (name + location via a folder dialog),
  Open…. Then the **editor** runs as a second `Application` in the same process with the project:
  working directory = project root, caches and layout under `Saved/`, start scene opened, title
  "Project – Scene* – Ungine Editor". Closing with unsaved changes asks (`Editor::ConfirmQuit` + modal
  Save all / Discard / Cancel).
- On Windows the GUI apps link with `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup` (no console) and embed
  the icon resource (`resources/windows/ungine.rc`).

## Editor layout and panels

- `ImGuiLayer` (private): ImGui 1.92.9 docking, GLFW backend chained with the engine callbacks, Vulkan
  backend with dynamic rendering and its own descriptor pool, style linearized for the sRGB swapchain,
  no keyboard navigation (keys stay engine hotkeys). Viewport textures are freed after
  `kFramesInFlight + 1` rendered frames (own list; they must not outlive the backend).
- `Editor` (`EditorContext`: window, renderer, scene, assets, scene renderer, camera + optional
  physics, scripts, audio, particles, navigation, animation, streaming, project, layout file,
  model references): dock space with a default layout (DockBuilder).
- **Viewport:** scene rendered into a texture sized to the panel, only when visible; toolbar with
  gizmo modes (W/E/R, local/world), overlays (Lights, BVH with depth, Colliders, Audio, FX, Volumes,
  Nav), Anchor (joint frame), Game cam (view through the primary camera, gizmo off), debug views.
  ImGuizmo results become local TRS relative to the parent's world matrix. Picking by GPU ID buffer
  (two frames late), Ctrl/Shift-click toggles, box selection by dragging (bounds centers), F frames the
  selection's world bounds, Delete deletes. Hotkeys only when the viewport is hovered/focused.
  `FlyCamera::moveRequiresLook` (WASD only with the right mouse button).
- **Hierarchy:** tree, selection, drag & drop reparenting (world pose kept), context menu (create
  child, focus, delete, lights, prefab operations), "+ Add" menu (entities, lights, audio, particles,
  volumes, decals, fog, navigation, terrain). Changes are applied after drawing. Colors: prefab
  instances blue, streamed entities turquoise, constructed entities grey.
- **Inspector:** name, TRS with an Euler cache (no flips), a section per component, "Add component",
  multi-selection edits (`ApplyEntityStateDiff`, one undo step; shows the primary's values), prefab
  header (file, levels, overrides with per-key revert, Apply/Apply to…/Revert all/Unlink/Create
  variant), instance variables, tags.
- **Renderer panel:** lighting/sun, post, shadows, local shadows, AO, lights, culling, TAA, SSR, decals,
  fog, physics, audio, camera. **Stats:** CPU frame graph, GPU timings, draw/cull stats, VMA budgets,
  validation errors, physics/audio/particles/navigation/animation sections. **Assets:** hot-reload
  switches, shader status, upload queue, tabs Models/Textures/Sounds with previews, memory, reload/
  retry/release.
- **Content browser** (with a project): folders and typed files (Scene, BP, Model, Tex, SND, CUE, PFB,
  PFX, PHYS, ANIM, TER, MAT, MINST, TYPE); double-click opens or places, drag & drop into the viewport
  (models, prefabs, sounds/cues, effects, terrains, materials onto meshes) or onto the selection
  (Blueprints); "+ New" (folder, Blueprint, scene, library, enum, struct, interface, physics material,
  sound cue, particle effect, animation graph, terrain, material, material instance); import, move,
  rename (with reference repair), delete (confirmation; not undoable); set as start scene; assign to
  selection; create child blueprint/variant/instance.
- Other windows: Blueprint, Blueprint types, Find in Blueprints, Particle Effect, Animation Graph,
  Material, Sound Cue, Physics material, Terrain, Navigation, Levels, Project Settings (incl. input,
  navigation, loading screen, save version), Package report, file dialog (filters, places, overwrite
  prompt, folder mode).

## Undo/redo (`History`)

- Max 256 steps; commands reference entities by UUID. Component edits are before/after state
  snapshots; the inspector detects changes by comparing snapshots per frame (an active widget is one
  session until released). One gizmo drag = one step (multi-selection moves by the same world delta
  around the primary's pivot). Create, delete (subtrees with parent and sibling index), duplicate
  (Ctrl+D), reparent (world pose kept), prefab operations (subtree snapshots; writing the prefab file
  itself is not undone).
- **Settings undo** (`TrackSettings`, every frame after the panels): baselines for renderer settings
  (scene-file JSON without debug flags), physics settings, project settings and the view state
  (overlays, Game cam, debug view). A difference with no active widget becomes one step ("Renderer
  settings", "Physics settings", "Project settings", "Viewport view"). Camera: only jumps (Focus) and
  camera fields in the renderer panel; flying is not undoable. `EditCommand::sceneChange` is false for
  project/view/camera steps, so `History::Dirty` (the "*" marker) counts scene changes only.
- Asset windows (Blueprint, Material, Particle, Animation, Sound Cue, Physics material) keep their own
  undo.

## Scenes in the editor

File menu New/Open/Save/Save As (Ctrl+N/O/S/Shift+S), unsaved-changes prompt, Open checks the file in
a scratch scene first, status line in the menu bar. Save all saves scene, project, open asset windows
and terrains. `EditorContext::modelRefs` owns all models the editor acquired (released on New/Open).

## Play mode

- Play/Pause/Step/Stop (Ctrl+P): Play commits pending edits, snapshots all roots and resets physics;
  Stop clears the scene, restores the snapshot (selection by UUID) and resets all systems. While
  playing: no history, undo/redo off, saving blocked, New/Open stop first; viewport border green
  (playing), orange (paused), red (breakpoint).
- Systems in Play: physics `FixedUpdate` + interpolation, `ScriptSystem` (begin after physics sync,
  input only with viewport focus), audio, particles, navigation, animation, level streaming around the
  editor camera, decals ageing, runtime UI input. Open Level in Play is synchronous and lasts until
  Stop.
- The editor's own public API (used by tests): selection, duplicate/delete, undo/redo, scene files,
  Play/Stop, `SimulateMouse/Key/Text`, package report, and per-system helpers (e.g. `AssignMaterial`,
  `CreateTerrain`, `BakeNavigation`, `CreateChildBlueprint`, `CreatePrefabVariantOf`,
  `ExpandAudioMixer`, `ExpandRenderingSections`, `UndoLabel`).

## Build menu

- **Build & Run** (Ctrl+B): Save all, set a start scene if none, launch `UnginePlayer <project>`
  (it must sit next to the editor).
- **Package project…**: folder dialog → `PackageProject` → **Package report** window (result, counts,
  errors, warnings, pak entries, Show folder).

## UnginePlayer (`apps/player`)

- Argument = project file, or the `*.ungineproj` next to the executable (packaged game; mounts
  `Content.upak` next to it). Loads the start scene with renderer and physics settings; runs physics
  (fixed step + interpolation), scripts, audio, particles, navigation, animation, decals, runtime UI
  and level streaming; renders through the primary camera. Esc opens the pause menu, F11 toggles
  fullscreen. Caches in `Saved/` (development) or `UserConfigDirectory()/Games/<Name>`; log file
  `player.log`; prints audio/particle/navigation/animation stats on exit.
- Open Level loads asynchronously behind a loading screen (see [scene.md](scene.md#level-streaming)).
- **Options** (`Core/GameOptions.h`, `apps/player/OptionsMenu.h`): `UserSettings` (fullscreen,
  vsync, window size, shadow quality 0–3, AO, bloom, bus volumes/mute, remapped actions/axes) with
  project defaults, saved as `Settings.json` (`{version, graphics, audio, input}`) per user.
  `Input()` = project map + overrides; `RemapAction / RemapAxis`, `Conflicts`, `ResetInput /
  ResetToDefaults`. `ApplyOptions`: fullscreen, `Window::SetSize`, `Renderer::SetVSync`, caps shadow/
  AO/bloom settings of the level (quality 0 = no sun and local shadows), bus volumes, input map. The
  pause menu has Graphics/Audio/Controls tabs (key capture, conflict hint, paging, resets). Editor
  Play applies only the input part. Blueprint nodes in categories Options and Input.

## UngineCook (`apps/cook`)

Console tool: `UngineCook <project> <out> [--pak-only]` (see [assets.md](assets.md#packaging)).

## Sandbox (`apps/sandbox`)

Developer demo for rendering, physics, audio and scripts; F1 toggles the editor. Command line and
hotkeys: [build-and-test.md](build-and-test.md#sandbox).
