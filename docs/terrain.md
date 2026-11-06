# Terrain

## Data (`Terrain/TerrainData.h`)

`.uterrain` = "UTRN" v1: JSON settings followed by raw data; paths relative to the file.

- Square height grid (`resolution` samples, uint16 × heightScale / 65535, row-major z/x).
- Splat map `splatResolution`² × 8 weights (interleaved).
- Hole mask (per cell, +x/+z of the sample).
- Up to 8 `TerrainLayer`s (albedo/normal/ORM textures, tint, tiling in meters, roughness/metallic/
  normal strength) and `triplanarSlope`.
- Up to 8 `FoliageLayer`s (Grass or Mesh, texture or mesh + index, density per m², width/height, scale
  range, base/tip color, wind, view distance, max slope, align to normal, shadows, seed, density map
  at splat resolution).
- Helpers: `MakeFlatTerrain`, `SanitizeTerrain`, `TerrainHeight / Normal / Hole` (terrain space =
  meters from the −X/−Z corner), `RaycastTerrain` (half-cell steps + bisection, holes skipped),
  `Save/LoadTerrain` (VFS), `Import/ExportHeightmap` (8/16-bit PNG via stb and an own 16-bit PNG
  encoder; RAW/R16 little endian; other sizes are resampled bilinearly).

## Component

`Terrain{data, castShadows, collision, navigation, lodDistance 24, foliageDensity, foliageDistance}`:
the grid is centered on the entity's world X/Z, height 0 = world Y; rotation and scale are ignored.
JSON `terrain` (path relative to the scene), undo, prefab overrides, reference repair, cooker checks.

## TerrainStore

Process-wide, main thread: one shared copy per path (renderer, physics, navigation, editor), mtime
check once per second (a changed file swaps everything), `Provide` (unsaved), `MarkDirty(path, kinds,
rect)` (heights/splat/holes/foliage/settings; kinds 0 = only "edited" for live values),
`Revision / ChangesSince` (union of normalized rectangles; > 256 edits or settings → full), `Save`,
`Dirty`, `Reload`, `Clear`.

## Rendering (`src/Renderer/TerrainRenderer`)

- CDLOD quadtree of 32-quad patches (power-of-two patch count, min/max per leaf patch); level l
  covers `lodDistance · 2^l`; vertices morph to the next coarser grid by camera distance.
- Heights from an R16 texture (bilinear), normals by central differences, splat from two RGBA8 maps,
  holes from an R8 map (`texelFetch`, `discard`).
- Passes: early prepass (feeds Hi-Z; picking returns the terrain entity), cascades (nodes per cascade
  frustum, LOD by camera), lighting (depth `EQUAL`). Only changed rectangles are uploaded (own staging
  buffers with a reuse barrier).
- `terrain.frag`: weighted layers, triplanar projection above `triplanarSlope` (three projections with
  detail normals), then the shared `ShadeSurface`. Layer textures come from the texture table and are
  released after 300 unused frames.

## Foliage (`include/foliage.glsl`)

- Scattered per 16 m chunk on the CPU (jittered grid, spacing 1/√density, density map as probability,
  hash RNG per seed/chunk/cell → deterministic); the list is shuffled so every prefix is a uniform
  thinning. Cached until the density-map rectangle or scatter settings change.
- Instance = terrain position + 8-bit yaw/scale/key/tint (16 B). **Height, holes and slope are applied
  in the shader** from the height field, so sculpting moves foliage without rescattering.
- Per view: chunks in range (frustum + min/max box) → prefix by distance (thinned to 30 % beyond 40 %
  of the view distance, the last 15 % shrink); instances of all views in one host buffer per frame
  slot (≤ 1 M).
- Kinds: procedural blades (15 vertices, bent, normal tilted towards the ground normal), crossed
  texture cards (12 vertices, alpha test), pool meshes (`vkCmdDrawIndexed` per submesh, LOD 0).
- Wind (direction + gusts, amplitude ∝ height²) with motion vectors; shadows per layer flag in the
  cascades.

## Physics and navigation

- Jolt `HeightFieldShape` (static, layer 0) per terrain entity when `collision` is on. A sample has no
  collision only when all four cells around it are holes (Jolt removes triangles per vertex, so
  single-cell holes stay solid). Height/hole changes rebuild the shape (outside a step at most 4×/s);
  moving the entity calls SetPosition.
- Navigation source per terrain when `navigation` is on (see [navigation.md](navigation.md)); the
  signature hashes heights/holes/origin (bake-stable); edits dirty only their rectangle (checked ≤ 2×/s).

## Editor (`editor/src/TerrainTools.cpp`)

- **Terrain** window (when a terrain is selected): Save/Revert (dirty indicator), Import/Export
  heightmap, tools Select/Raise/Lower/Smooth/Flatten/Paint/Holes/Foliage (`TerrainBrush`: radius,
  strength, falloff, layer, erase, flatten target; Shift+click picks the height, Ctrl inverts), layer
  list (selection = paint target, textures via "…" or drag & drop, tint/tiling/material, add/remove
  with weight redistribution), foliage list, size/height scale/triplanar settings.
- Strokes: left-drag in the viewport (gizmo and selection off), brush circle on the surface. **One
  undo step per stroke** (`CaptureTerrainRegion` before/after in the touched rectangle); field edits
  commit one step when no widget is active.
- Saving the scene (or Save all) saves changed terrains; New/Open discard unsaved terrain edits.
  Hierarchy "+ Add → Terrain" (`CreateTerrain`: `Content/Terrains/Terrain*.uterrain`, 128 m, 257²,
  one layer + an empty grass layer), content type TER (+ New → Terrain, double-click/drag & drop
  places it). Terrain editing is disabled while playing.
- API: `CreateTerrain`, `PlaceTerrain`, `TerrainBrushSettings`, `SetTerrainToolEnabled`,
  `TerrainToolActive`, `ApplyTerrainStroke`, `SaveTerrains`.

## Blueprint and cooker

*Get Terrain Height* (location → world height, normal, found, terrain; not found over holes/outside).
The cooker cooks layer textures as Color/Normal/Linear and foliage cards as Color, checks meshes and
packs `.uterrain` raw.
