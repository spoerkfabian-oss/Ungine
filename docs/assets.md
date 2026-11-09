# Assets, cooking and packaging

## AssetManager basics

- Handles (`AssetHandle<T>`, `ModelHandle`, `TextureHandle`, `SoundHandle`, `MaterialHandle`) are
  index + generation; 0 is null. **Handles do not keep assets alive** — the reference count does.
- `LoadModel(path)` caches by normalized absolute path (refcount + 1) and loads on the ThreadPool.
  `Update()` (main thread, every frame) moves results through Uploading → Ready and publishes
  `AssetLoadedEvent<T>`, `AssetReloadedEvent<T>` and `AssetFailedEvent<T>` after its loops.
- `Release` at refcount 0 frees the asset (graveyard while an upload is still running, orphan while
  a job is still running). The destructor waits for jobs, flushes uploads and frees everything.
- `CreateModel(ModelData)` registers generated geometry (same lifecycle, not cached, not saveable,
  not reloadable). `CreatePrimitive(PrimitiveDesc)` creates cached plane/box/sphere/capsule
  primitives that scenes can save. `Source(handle)` tells file, primitive or nothing.
- `MeshRenderer{ModelHandle, meshIndex}` references a mesh; the SpatialIndex and GpuScene resolve it
  with `ResolveMesh` (Ready → model, Failed → placeholder, otherwise nothing yet).
- `InstantiateModel(scene, handle, model)` creates one entity per glTF node (parents first) with a
  `ModelInstance` root and `ModelNodeRef`s. `RefreshModelInstances` re-matches nodes by name after a
  reload (renamed nodes become new entities).

## Models

Model job: `LoadGltf` → `OptimizeMeshes` → `BuildModelGeometry` (worker) → main thread: acquire
textures → `BuildModelMaterials` (materials and `GpuSubmesh` records; CPU copy in
`Model::gpuSubmeshes`). **A model is Ready only when none of its textures is still loading.**

- **meshoptimizer:** vertex cache, overdraw, vertex fetch (only for exclusive vertex ranges; skin
  influences are remapped too). **LODs:** up to 4 levels for submeshes with ≥ 256 triangles, target
  × 0.5 from LOD 0, locked borders, absolute object-space error, stop below 10 % savings.
- glTF support: meshes, PBR metallic-roughness materials (incl. normal/occlusion/emissive, alpha
  modes, double-sided), `KHR_lights_punctual` point/spot lights, skins (joints, inverse bind
  matrices, JOINTS_0/WEIGHTS_0) and animation tracks (STEP/LINEAR/CUBICSPLINE). Not supported:
  COLOR_0, samplers, UV sets > 0, morph targets, basisu/webp, specular/clearcoat/transmission.
- External `.bin` files and images use UTF-8 file callbacks (Unicode paths work on Windows).
- Physics gets a CPU copy of collision positions/indices (`Model::collisionPositions/Indices`).
- **Placeholder:** a failed model renders as a 1 m box with the error texture (slightly emissive);
  physics does not use it.

## Textures

- `LoadTexture(path, TextureKind)` with kinds Color (sRGB), Linear and Normal, cached per path +
  kind (the same file as Color and Linear = two assets). glTF images become sources: external files
  go through `LoadTexture`, embedded ones are shared by content hash (FNV-1a) across models and
  reloads. Models hold texture references.
- `CookTexture` (worker): PNG/JPG/KTX2 → CPU mips (sRGB filtered in linear space, normals
  renormalized) → **BC7** (Color/Linear, bc7enc) or **BC5** (Normal, rgbcx; the shader reconstructs
  Z) → KTX2 (own reader/writer, no supercompression); RGBA8 without compression or without BC
  support. Disk cache `<cacheDirectory>/<hash>.ktx2`, key = source bytes + kind + settings + cooker
  version; LRU pruning by mtime above 4 GB. `VulkanContext::SupportsBC()`.
- **Texture table:** materials store table entries; `FrameUniforms::textureTable` maps them to
  bindless slots (copied in `BeginFrame` when changed). Entries 0–3 are White, Black, FlatNormal and
  **Error** (8×8 magenta checkerboard). While loading, a neutral default is used; failed color
  textures show the error texture. Entries are freed late (`FreeTextureEntry`).

## Reload, retry, hot reload and revisions

- `Reload(handle)` loads new content as *pending* and swaps it in only when uploaded (and its
  textures are ready). A failed reload keeps the old content (error text + `AssetFailedEvent`).
  Failed assets can be retried. Texture reloads only swap the table entry.
- `AssetManagerDesc` (`ApplicationDesc::assets`): texture settings (compress, cache directory,
  quality), mesh optimize settings, `hotReload`, `pollSeconds` 0.5 → mtime polling of sources and
  dependencies (external `.bin`), debounced (a change must be stable for one poll); missing files load
  when they appear.
- `Revision(handle)` changes whenever `ResolveMesh` would answer differently (Ready, Failed, reload);
  `ContentVersion()` is global. SpatialIndex, GpuScene and PhysicsWorld follow revisions.
- Sounds and materials have the same handle/refcount/reload/revision model (see
  [audio.md](audio.md) and [materials.md](materials.md)).

## Shader hot reload and pipeline cache

- `ShaderHotReload` reads glslc depfiles (`*.spv.d`), polls sources and includes (debounced),
  recompiles with the build flags (`ENGINE_GLSLC`, Debug `-g -O0`) into `.tmp` → rename and fixes
  the depfile target; on error the old SPIR-V stays (`LastError()`). It blocks the main thread and
  needs glslc + the build tree. `Renderer::SetShaderHotReload / ShaderGeneration /
  NotifyShadersChanged`; the SceneRenderer, Environment, GpuScene and GpuCulling rebuild their
  pipelines (build everything first, then swap). ImGui pipelines are excluded.
- `VkPipelineCache` persisted to `pipelines.bin` (sandbox: `asset_cache/`, projects: `Saved/`).

## Virtual file system and pak files

- `Vfs::Mount(pak, root)` (`Core/FileSystem.h`): paths below `root` come from the pak (newest mount
  first, otherwise the file system); `shared_mutex`, thread-safe. Every content reader goes through
  `Vfs::Read / ReadText / Locate / ModifiedTime / ListFiles`: assets (bytes + mtime), glTF buffers and
  images, scenes and prefabs, Blueprints, the script registry, sounds (miniaudio `ma_vfs` callbacks
  over pak ranges, streaming included).
- Pak format v1 (`.upak`, uncompressed): 40-byte header (`UPAK`, version, count, flags, index
  offset/size/hash), data aligned to 16 bytes, index at the end; FNV-1a 64 per entry (checked on every
  read) and for the index (checked on open). Entries are paths relative to the project root
  (`Content/...`, case-insensitive on Windows); entries with `..`, `.`, empty components, `\` or `:`
  are refused by `PakWriter::Add` and by `PakFile::Open`, and `ListFiles` only returns files inside
  the mount root; cooked data lives under `:cooked/…`
  (`ReadCooked`, `ReadCookedFor(file, category)`). `PakWriter` deletes the file unless `Finish`
  succeeds.

## Cooker

`CookProjectContent(project, pak, CookOptions)` (`Assets/ContentCooker.h`) packs everything below
`Content/` (except dot files and `.tmp`; symlinks whose target lies outside `Content/` are skipped
with a warning), sorted, reproducibly:

- glTF/GLB → `LoadGltf` + `OptimizeMeshes` (LODs) → `SerializeModelData` (`UMDL` v1 + hash of the
  mesh settings) as `:cooked/models/<path>`; the raw `.gltf/.glb/.bin` files are left out.
- Textures used by models (kind from the material), UI images, decal/terrain/particle/material
  textures with their kinds, and other images (as Color, with a warning) are encoded in parallel to
  `:cooked/textures/<TextureCookKey>.ktx2` (reuses the texture cache).
- Scenes, prefabs, cues, effects, animation graphs, materials and terrains are checked for missing
  references and references outside `Content/` (warnings); `.unav` and `.uterrain` go in raw.
- `CookReport` (entries, warnings, errors, counts; `Text()` = `BuildReport.txt`).
- At runtime `LoadModel` tries `LoadCookedModel` first (only if the mesh settings hash matches;
  `ModelData::optimized` skips re-optimization) and `CookTexture` looks in the pak, then the disk
  cache, then encodes (with a mounted pak this logs "not cooked into the pak").

## Packaging

- `PackageProject(project, player, out, error, report)`: copies the player as `<Name>(.exe)`,
  `shaders/` (incl. `shaders/source` for runtime material compiles) and the project file, deletes an
  old `Content/`, cooks `Content.upak` (cache `Saved/Cache/Textures`) and writes `BuildReport.txt`.
  It fails on cook errors or when the start scene / loading screen is not in the pak, and refuses
  the project folder itself as target.
- `PackageProject(..., callbacks)`: optional progress (0..1 + step) and cancel poll, both on the
  packaging thread; `CookOptions::cancelled` is polled between model files, texture jobs and pak
  entries, a cancelled cook reports `kCookCancelled` and deletes its unfinished pak.
- **UngineCook** (`apps/cook`, console): `UngineCook <project> <out> [--pak-only]`; exit 0 success,
  1 arguments/project, 2 cooking.
- The player mounts `<root>/Content.upak` at startup. The editor shows the **Package report**
  window after *Build → Package project…*.

## Content import (editor)

The content browser imports GLB, glTF (with local URI dependencies), PNG/JPEG/KTX2 and
WAV/OGG/MP3/FLAC asynchronously into the current folder with unique names; previews (texture,
model wireframe + material swatches, audio waveform) are cached through the AssetManager. Moving and
renaming repair references in recognized saved formats (scenes, prefabs, Blueprints, glTF URIs, cues,
effects, materials, animation graphs, collider materials, level paths, Blueprint class paths) and in
the open scene. See [editor.md](editor.md).
