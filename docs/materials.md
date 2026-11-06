# Node-graph materials

glTF materials stay the default; `.umat` graphs and `.uminst` instances override them per submesh.

## Graph (`Material/MaterialGraph.h`, `.umat` JSON v1)

- **Settings:** `blendMode` Opaque/Masked/Translucent/Additive, `shadingModel` Lit/Unlit, `twoSided`,
  `castShadows`, `alphaCutoff`, `maxDisplacement` (world position offset bound for culling).
- **Parameters:** Scalar, Vector, Texture (with `TextureKind`), group, slider range.
- **Nodes** `{id, type, position, param, value, textureKind, values of unconnected inputs}`, links (one
  per input), comments. Paths are absolute in memory, relative on disk.
- About 55 node types (`MaterialNodeTypes()`): Output; Constant/2/3/Color; Param.Scalar/Vector/Texture;
  Texture.Sample; inputs (TexCoord with tiling, world/object position, vertex normal, camera
  position/vector, time, screen position and pixel depth — pixel stage only); math (Add … Modulo,
  Power, Lerp, Clamp, Step, SmoothStep, Saturate, OneMinus, Abs, Floor, Ceil, Frac, Sin, Cos, Sqrt,
  Negate, Sign); vector (Dot, Cross, Normalize, Length, Distance, Append, Component Mask, Split);
  utility (Fresnel, Panner, Rotator, Desaturate, Noise, Normal Strength, Sphere Mask, If).
- Widths 1–4 per pin; generic pins follow their widest input (scalars widen; different vector widths
  are an error); implicit defaults (UV0, time, normal, world position).
- **Instances** `.uminst` `{parent (.umat or .uminst), overrides}` → `ResolveMaterialParameters` (the
  leaf wins).

## Code generation

`GenerateMaterialCode` emits one local per node output in topological order (cycles, type errors,
unknown parameters and pixel-only nodes in the vertex path are errors; unconnected nodes are warnings)
and two functions in `material.generated.glsl`: `EvaluateMaterialOffset` (vertex: world position
offset, textures sampled at LOD 0) and `EvaluateMaterial` (pixel).

- Parameter block layout (`MaterialSlot`): all parameters, then the textures of Texture.Sample nodes,
  finally the alpha cutoff. Changing values never changes code, so instances share shaders.
- Shader variants per blend mode (`MaterialShaderVariants`): Vertex (`mesh.vert`), Prepass and
  PrepassPicking (`depth_normal{,_id}.frag`), Lit (`mesh.frag`), Blend (`mesh_blend.frag`),
  ShadowVertex/ShadowMask. These are the **existing engine shaders** compiled with
  `CUSTOM_MATERIAL` (+ `MATERIAL_BLEND_MODE`, `MATERIAL_UNLIT`, `MATERIAL_TWO_SIDED`), with
  `material_common.glsl` and `material_surface.glsl` (normal via the TBN, masked discard).
- Lit materials go through the same `ShadeSurface` as glTF materials (a lit graph with glTF values
  renders identically within ±1/255); Unlit = emissive (+ fog).
- World position offset moves the current and previous-frame position (`materialTime.y`,
  `prevModel`), so motion vectors are correct.

## Runtime compilation (`Renderer/ShaderCompiler.h`)

- glslang `CompileGlsl` / `CompileEngineShader` with defines and virtual includes, target **SPIR-V
  1.5** (1.6 turns `discard` into DemoteToHelperInvocation, which needs an extra feature).
- Sources: `ShaderSourceDirectory()` = env `UNGINE_SHADER_SOURCE_DIR` → the source tree in a build tree
  → `<ShaderDirectory>/source` (the build copies `engine/shaders` there, install puts it in
  `bin/shaders/source`, packaging copies it). `ShaderSourceHash()` (FNV over all sources; reset by
  shader hot reload).

## Assets (`AssetManager::LoadMaterial`, `MaterialAsset`)

- Worker (`src/Material/MaterialBuild`): resolve the instance chain (≤ 16, cycles detected, all files
  watched), generate code; key = code + defines + variants + source hash + version → memory cache
  (weak, shares shaders between instances) → disk cache `<texture cache>/../Shaders/<key>.<variant>.spv`
  (or `AssetManagerDesc::shaderCacheDirectory`) → glslang.
- Main thread: acquire the slot textures (table entry as float bits in the block; empty = White or
  FlatNormal); Ready only when they are loaded (no flicker). Revision + `MaterialVersion()` (also on
  failure/release). Hot reload by mtime of the whole chain, `Reload`, `ProvideMaterial /
  ProvideMaterialInstance` (unsaved editor versions reload every material whose chain contains the
  file); a renderer shader generation change reloads all. `Materials()` lists state, errors and cache
  hits.

## Component

`MaterialOverrides{materials (per submesh, "" = glTF material), parameters [{name, value, submesh
(-1 = all)}]}` (JSON `materialOverrides`, relative paths, undo, prefab overrides, reference repair).
Call `MarkChanged` after edits.

## GPU scene and pipelines

- `GpuScene` keeps **material slots** (handle, settings, shader set, draw count). Batches carry the
  slot and displacement; the batch key = winding | submesh | slot | material revision. Buckets are
  dynamic (see [rendering.md](rendering.md#gpu-driven-rendering)). `GpuDraw::params` points to the
  per-draw parameter block (material block + entity overrides); only changed values rewrite the block,
  changed materials rebuild the draws. Culling boxes grow by `displacement`.
- `SceneRenderer::ResolveMaterial(path)` holds one reference per path (re-checked against the scene
  every 120 frames); `PrepareMaterialPipelines` builds prepass, picking, lit (`EQUAL`), blend
  (alpha/additive) and shadow pipelines from SPIR-V per shader set, freed after 240 unused frames. A
  failed material drops its draws. `FrameUniforms::materialTime` (x = time, y = previous frame),
  `materialParams`.

## Blueprint nodes (category Rendering)

Set Material Scalar/Vector Parameter (target and all meshes below it, submesh −1 = all), Set Material
(path; empty = glTF), Clear Material Parameters. The cooker cooks parameter/node/instance textures
with their kinds and warns about graph errors and missing or outside parents; shaders compile at
runtime.

## Editor

- **Material** window (`MaterialEditor`): toolbar (Save, Revert, Undo/Redo, Frame, compile state),
  side bar (settings, parameters with rename/type/values/textures via "…" or drag & drop, "Place
  node", details of the selected node incl. unconnected input values, problems with click-to-select
  and shader errors), node canvas (pan with middle/right mouse, zoom at the cursor, snap, multi-select,
  drag links between pins, drop on empty space → palette with auto-connect, Alt/Ctrl-click or
  right-click on an input disconnects, right-click → palette with search/Enter, Delete removes,
  texture drop creates Texture.Sample), preview (sphere/box/plane, ground, orbit). Instance mode:
  parent selection (drop) and override checkboxes per parameter.
- Changes go live to the preview and the scene via Provide; an undo step is recorded when no widget is
  active; Save all and the quit prompt include it.
- Inspector (Mesh Renderer): **Materials** (slot per submesh with the glTF material name, drop target,
  "x", click opens) and **Material parameters** (override checkbox per scalar/vector parameter).
- Content browser: types MAT/MINST, "+ New → Material / Material instance", context menu *Create
  instance* / *Assign to selection*, drag & drop onto a mesh in the viewport (`Editor::AssignMaterial`,
  undo). Reference repair covers `.uminst` parents, texture paths in `.umat` and `materialOverrides`.
- Example: `Content/Materials/Pulse.umat` + `Pulse_Warm.uminst` in the Basic template.
