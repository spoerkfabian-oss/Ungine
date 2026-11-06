# templates/

Project templates offered by the editor's project browser. Each subfolder with a `template.json`
is one template; `Project::Create` copies **everything in the folder except `template.json`** into the
new project, so keep other files (including documentation) out of the template folders — this README
lives one level up for that reason.

## template.json

```json
{
  "name": "Basic Scene",
  "description": "Shown in the project browser.",
  "startScene": "Content/Scenes/Main.scene.json"
}
```

`startScene` is relative to the project root and becomes the project's start scene.

## Templates

### Blank

An empty level with a floor and a camera (`Content/Scenes/Main.scene.json`).

### Basic (Basic Scene)

A small showcase: floor, a rotating humming cube (`Scripts/Rotator.ugraph`, `Sounds/hum.wav`), a
waving skinned banner (`Models/AnimatedBanner.gltf`), a falling sphere, lights, a camera, streamed
music (`Sounds/music_loop.wav`), a campfire effect (`Effects/Campfire.uparticle`), a pulsing material
and an instance of it (`Materials/Pulse.umat`, `Pulse_Warm.uminst`) and a sub-level
(`Scenes/Annex.scene.json`) streamed in by a volume.

### Physics (Physics Playground)

Press Space while playing: a Blueprint (`Scripts/RainOnSpace.ugraph`) drops spheres onto a box pyramid
and a ramp; impacts play `Sounds/impact.wav` (`Scripts/ImpactSound.ugraph`) in a reverberant hall. Also
a spark fountain with collision and trails (`Effects/SparkFountain.uparticle`), navmesh bounds with box
obstacles, and a wandering fox (`Models/Fox.glb`, `Animations/Fox.uanim`, `Scripts/Wander.ugraph`) that
walks/runs with foot IK.

## Notes

- Empty folders are kept with `.keep` files (they are copied too and are harmless).
- `templates/Physics/Content/Models/LICENSE.md` travels with the fox model into new projects (and
  their packages) on purpose: the model is CC-BY 4.0.
- Templates are installed to `bin/templates/` and found through `TemplateDirectory()` (next to the
  executable, otherwise the source tree).
- The smoke tests build projects from these templates with `tests/MakeProject.cmake`, so changes are
  covered by `UngineEditorSmoke`, `UnginePlayerSmoke`, `UnginePlayerBasicSmoke`,
  `UnginePlayerLevelSwitchSmoke` and `UnginePackagedPlayerSmoke`, and by CPU tests that load the
  template content.
- Template sounds are copies of `assets/sounds/`.
