# Rendering

The `SceneRenderer` renders a `Scene` through a camera into the swapchain or into any `RenderOutput`
(image, view, format, extent; layout `COLOR_ATTACHMENT_OPTIMAL` in and out). All scene targets
(depth, HDR, AO, bloom, velocity, IDs) are sized to the output extent; the tonemap pipeline is
created lazily per output format. A SceneRenderer must render at most once per frame (per-frame
buffers such as particle data are overwritten).

## Frame order

1. Environment/IBL update (compute, only when the sky settings change)
2. Depth/normal prepass, early phase (normals + roughness, velocity, entity IDs) → Hi-Z
3. GPU culling late phase → prepass late phase
4. Shadows: cascades (after the prepass, casters culled against the camera Hi-Z), local shadow atlas
5. GTAO
6. Light and decal clustering (`light_cull.comp`)
7. SSR, volumetric fog
8. Lighting pass: opaque (depth `EQUAL`), terrain, foliage, sky, sorted transparents, particles
9. TAA resolve
10. Bloom and auto exposure (on the resolved image)
11. Tonemap (+ sharpening, debug views, selection outline), text overlay

`ResetHistory()` (camera cut) resets TAA, reflections and fog history; the editor and player call it
on Play/Stop/New/Open and level changes.

## Shading

- **PBR** (`pbr.glsl`, `mesh_shading.glsl`): glTF metallic-roughness, GGX + height-correlated Smith
  + Schlick, Lambert diffuse, roughness floor 0.045; IBL split-sum with multiple scattering
  (Fdez-Agüera). AO applies to indirect light only.
- **`ShadeSurface`** (`include/shading.glsl`) is shared by meshes, terrain and custom materials:
  decals, sun and local shadows, clustered lights, IBL, SSR, AO, fog, cascade debug.
- **Normal mapping** (`surface.glsl`): tangents when present, otherwise a derivative-based frame.
- **Environment** (`Environment`): analytic sky (`sky.glsl`, same function for skybox and IBL; IBL
  without the sun disc) → env cube 256² (9 mips) → irradiance 32² (512 cosine samples) + prefiltered
  128² (5 mips = roughness 0…1, 256 GGX samples, filtered importance sampling) + BRDF LUT 256² (once).
  Regenerated completely whenever the sun changes.
- **Tonemap:** PBR Neutral, ACES fitted or none; exposure; bloom mix
  (`mix(hdr, bloom, strength)` before exposure).

## Shadows

### Cascaded shadow maps

`ShadowSettings` (default 4 × 4096², max distance 60, λ 0.8, constant/slope bias, normal offset in
texels, PCF radius, 10 % blend band, debug tint). `ComputeCascades`: practical split, bounding sphere
per slice (rotation invariant, quantized radius), texel snapping in light space, Reverse-Z ortho.
D32 array map, layer views as attachments. Pass: no culling, depth clamp (pancaking), negative depth
bias, alpha-mask pipeline. Sampling: cascade by view depth, 16 Poisson taps × 2×2 hardware PCF, IGN
rotation, cross-fade to the next cascade, geometric normal for the offset.

### Local light shadows

- `Light::castShadows` (default on). `LocalShadowSettings`: atlas 4096² D32 (power of two; recreated
  on change, freed when disabled), max 8 lights, tiles 128–1024 px, bias, normal bias 1.5 texels,
  filter radius 1.5 texels.
- Candidates are visible shadow-casting lights ranked by the screen size of their range sphere.
  Spot lights get one tile, point lights six cube faces at half size; tiles halve until they fit.
- Projections: Reverse-Z perspective with infinite far, near = 0.5 % of the range; spot FOV
  = 2 · outer (max 85°), point lights 90° per face; both widened by the PCF border so filtering stays
  inside the tile (no cube seams).
- **Cache:** `ShadowTileAllocator` (buddy/quadtree) keeps each light's tiles. A view is re-rendered
  only when its view-projection changes, the bias changes, a `ChangedRegion` intersects the light's
  range, or the atlas was recreated. The atlas stays in `SHADER_READ_ONLY` between frames; only
  re-rendered tiles are cleared. Lights outside the view/budget lose their cache.
- Sampling: face by major axis, normal offset scaled by texel size × distance, 16 rotated Poisson taps
  × hardware PCF, clamped to the tile.

## Ambient occlusion, exposure, bloom

- **Prepass + GTAO:** `depth_normal.frag` (alpha test, normal mapping, view-space normals RGBA16F);
  the lighting pass tests depth `EQUAL` without writes. GTAO in full resolution (XeGTAO slice
  formula, 2 slices × 4 steps per side, quadratic step distribution, falloff, IGN noise) → 5×5
  bilateral denoise. `AoSettings` (radius in world units, falloff, power, slices, steps, sharpness).
  Material AO and GTAO are combined with `min`; specular occlusion after Lagarde.
- **Auto exposure:** 256-bin luminance histogram (log2 −10…+6, shared-memory atomics) → average pass
  (geometric mean without the black bin, exponential adaptation) → `ExposureState` (BDA) → tonemap.
  Key 0.3; `post.exposure` is compensation. Readback per frame slot → `Stats().exposure`.
- **Bloom:** half resolution, up to 6 mips, 13-tap downsample (Karis average on the first),
  3×3 tent upsample (additive). `PostSettings`: bloom, strength 0.04, radius 0.005.

## Clustered Forward+ lights

- `Light` component: point/spot, color, intensity in candela on the scale of the sun's illuminance
  (E = I/d²; sun = 3.0), range (0 = derived from intensity with cutoff 0.005), inner/outer cone
  half-angles. Position/direction from the world transform (local −Z). glTF `KHR_lights_punctual`
  point/spot lights are imported (directional lights are ignored with a warning).
- CPU frustum culling (max `kMaxVisibleLights` 4096) → `GpuLight` list.
- Cluster grid **16 × 9 × 24** (logarithmic depth from the near plane to `clusterFar` 300, last slice
  to infinity), 256 light indices per cluster (+ up to 32 decals). `light_cull.comp`: one thread per
  cluster, lights in shared memory, sphere-vs-AABB, spot cone test (Wronski).
- Debug view `LightClusters` (heatmap, white = full list).

## GPU-driven rendering

- **GeometryPool** (`Renderer::Geometry()`): four fixed buffers (vertices 4 M, indices 16 M,
  materials 64 k, submeshes 256 k; concurrent sharing), best-fit `RangeAllocator` per pool. A full pool
  is a load error. Models own `PoolRange`s that are freed with `DeferCall`.
- **GpuScene** (private, per SceneRenderer): persistent instances (one `GpuInstance` per
  `MeshRenderer`), draw records (instance × submesh), batches (submesh + winding + material slot →
  refcount; `instanceBase` = prefix sum). Changes come from the SpatialIndex; uploads only touch
  changed entries via `scatter.comp`; buffers grow by reallocation + full re-upload.
- **GpuCulling** (private): views per frame = camera early, camera late, cascades, local shadow tiles
  to re-render. `gpu_cull.comp` phase 0 (frustum, Arvo world AABB), phase 1 (camera late: frustum +
  Hi-Z, writes visibility), phase 2 (one `VkDrawIndexedIndirectCommand` per batch × view with atomic
  counters per bucket). Drawing uses `vkCmdDrawIndexedIndirectCount` per bucket.
- **Two-phase occlusion:** prepass early (records visible last frame) → Hi-Z (`hiz.comp`, min
  reduction, level 0 = half resolution) → late culling → prepass late. A box is occluded when its
  nearest depth is behind the Hi-Z minimum at the level where it covers ≤ 2×2 texels.
- **Buckets:** camera 0–3 (double-sided × mirrored), 4 blend (sorted on the CPU), `5 + 2·slot +
  mirrored` per custom material; shadow 0/1 (alpha-masked or not), `2 + slot` per custom material,
  `kBucketNone` = casts no shadow. Bucket offsets are prefix sums uploaded for the culling shader.
- **CPU path** (fallback/reference): BVH query → draw records → transient visible list →
  `vkCmdDrawIndexed`. Same shaders and data; image equality with the GPU path is tested.
- `CullingSettings`: gpuDriven (default on), occlusion, freeze (debug), Hi-Z debug level, LODs.
  Debug views `HiZ`, `Culling` (late draws tinted orange).

## LODs

Meshes get up to 4 LODs at import (see [assets.md](assets.md)). `SelectLod` (identical in GLSL and
C++) picks the coarsest level with `error · maxScale · pixelsPerUnit / lodPixelError ≤ distance −
radius`; all views choose by the camera. LOD changes cross-fade with a dither over a 20 % band (not in
shadows). `CullingSettings`: lod, lodPixelError (1 px, saved in scenes), forceLod. Debug view `Lod`.
Skinned meshes stay on LOD 0.

## Transparency

Alpha-blended submeshes go through `mesh_blend.frag`, sorted back to front per object; shadows use the
alpha test. Custom translucent materials are sorted together with glTF transparents.

## TAA

`TaaSettings` (enabled, feedback 0.9, sharpness 0.25). Halton(2,3) jitter (8 positions) as a
clip-space translation; `FrameUniforms` also carry `prevViewProj` and `unjitteredViewProj` (culling
uses the unjittered one). The prepass writes a velocity target (R16G16, UV delta); `prevModel` and
`prevJointOffset` give motion for moved and skinned meshes; particles/transparents write velocity by
coverage, opaque meshes and the sky do not. `taa.comp`: YCoCg variance clipping (3×3), Catmull-Rom
history (5 taps), velocity of the nearest depth, sky by camera reprojection, Karis weights. Debug view
`Velocity`. Pixel-exact image comparisons in tests must disable TAA.

## Decals

`Decal` component (half extents, albedo/normal/mask/emissive textures, color, emissive,
roughness/metallic + material opacity, normal strength, angle fade, edge fade, sort order,
lifetime/fade-out/age). CPU frustum culling, sorted by sort order then UUID, up to 1024 visible;
`light_cull.comp` fills per-cluster decal lists (32 per cluster, order preserved); `decals.glsl`
applies them in `ShadeSurface` before lighting (opaque only). Textures come from the AssetManager
texture table and are released after 300 unused frames. `UpdateDecals(scene, dt)` ages and destroys
decals (player, editor Play, sandbox).

## Screen-space reflections

`SsrSettings` (intensity, max roughness 0.5, thickness, max distance, steps, feedback). Closest-depth
pyramid (`ssr_hiz.comp`, full-resolution level 0), Hi-Z trace in half resolution with a rotating 2×2
phase (`ssr_trace.comp`), color from the previous frame (TAA history, or HDR without TAA), temporal
resolve with neighborhood clamp (`ssr_resolve.comp`), separable roughness blur (`ssr_blur.comp`).
Lighting blends IBL radiance by confidence × intensity. Debug view `Reflections`.

## Fog

- **Height fog** (`FogSettings`, off by default): density · exp(−falloff · (y − baseHeight)),
  closed-form integral along the ray, start distance, max opacity, color = albedo · (sun · HG phase +
  sky ambient). Applied to meshes, sky and particles via `fog.glsl`.
- **Volumetric fog** (`volumetric`, distance 64 m): froxel grid ⌈W/16⌉ × ⌈H/16⌉ × 64 slices
  (quadratic depth distribution), RGBA16F 3D images (bindless 5/6). `fog_inject.comp`: density
  (height fog + `FogVolume` boxes with edge fade, up to 64), sun × cascade shadow (one PCF tap) × HG,
  clustered lights × local shadows × HG, sky ambient, emission; jittered depth and temporal
  reprojection (history reset on settings change). `fog_integrate.comp`: front-to-back per column
  (energy-conserving, Hillaire). Beyond the grid the analytic fog continues.
- Shadow maps are read from compute shaders too (barriers include the compute stage).

## Picking and selection outline

`SceneRenderer::overlay` (picking, outlined entities, color). `depth_normal_id.frag` writes the entity
slot + 1 into an R32_UINT target; `RequestPick(x, y)` copies one pixel into a per-frame readback
buffer and `TakePickResult()` returns it `kFramesInFlight` frames later. The outline is computed in
the tonemap pass from a bitset of selected slots (edge where selection differs in a 5×5 pattern).

## Text overlay and profiling

- `TextOverlay`: stb_easy_font quads from the transient buffer, alpha blended, shadowed; used by the
  player for Print String output and loading screens.
- `GpuProfiler` (`Renderer::Profiler()`): timestamp query pool per frame slot (48 scopes), RAII
  `GpuScope`; results are read `kFramesInFlight` frames later without stalls.
- Shader hot reload and the pipeline cache are described in [assets.md](assets.md).

## Debug views and stats

Debug views (tonemap): AO, normals, light clusters, shadow atlas, Hi-Z, culling, LOD, velocity,
reflections. `SceneRenderer::Stats()` reports draws, culling counters, lights, shadow tiles, GPU
culling counters (two frames old), particles, terrain/foliage and fog statistics.
