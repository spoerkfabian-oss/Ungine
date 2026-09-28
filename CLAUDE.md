# Kontext: C++20/Vulkan Game Engine (Stand nach Phase 8)

Übergabe-/Gedächtnisdokument. Nach jeder Phase aktualisieren.

## Arbeitsweise (Wünsche des Users)
- Antworten kurz und bündig, token-schonend. Vor neuen Aufgaben Unklarheiten erfragen.
- Eigene Arbeit prüfen, Self-Review mit "bereits behoben" / "noch offen" liefern.
- Rolle: Principal Software Engineer / Game Engine Architect. Lieferung phasenweise als Git-Commits (kein ZIP).
- Entwicklungs-Branch laut Session-Vorgabe; nie auf andere Branches pushen.

## Ziel & Rahmen
- Modulare 3D-Engine: C++20, Vulkan 1.3 (volk, vk-bootstrap, VMA), GLFW, GLM, cgltf, stb_image; ImGui folgt.
- Ziel: Windows/MSVC, Deps per CMake FetchContent (gepinnt in `cmake/Dependencies.cmake`, alle `SYSTEM`, CMake ≥ 3.25).
- Ziel-GPU: GTX 1070 Ti (Pascal, hat dedizierte Transfer-Queue). Keine Mesh-Shader.
- Originalauftrag: Core (Plattform, Events, ECS, async Resource Manager), Vulkan (PBR, CSM, Tone Mapping, Bloom, SSAO, Forward+/Deferred, Frustum Culling), Physik/Scene (BVH/Octree, Jolt), Tooling (ImGui-Editor).

## Verzeichnisstruktur
```
CMakeLists.txt  cmake/{Dependencies,Shaders}.cmake  CLAUDE.md
engine/include/Engine/
  Core/      Application, Window, Input, Log, ThreadPool
  Events/    Events, EventBus
  ECS/       Entity, Registry (header-only)
  Scene/     Components, Scene, Camera
  Assets/    AssetHandle, AssetManager, Model, GltfLoader, Primitives (MakePlane)
  Scene/     + Frustum (Aabb, TransformAabb)
  Renderer/  Renderer, SceneRenderer, Environment, ShadowCascades, Vulkan/{VkCommon,VkUtils,VulkanContext,Swapchain,Buffer,Image,Upload,Bindless,Pipeline}
engine/src/...   engine/shaders/ (mesh.*, depth_normal.frag, shadow.vert, shadow_mask.frag, fullscreen.vert, sky.frag, tonemap.frag, ibl_*.comp, bloom_*.comp, gtao*.comp, luminance_histogram.comp, exposure_average.comp, include/{bindless,frame,mesh_common,pbr,sky,ibl_common,shadow,surface,exposure}.glsl)
sandbox/src/main.cpp   tests/{Test.h,CoreTests.cpp,GpuTests.cpp}
assets/models/{WaterBottle,MetalRoughSpheresNoTextures,BoxTextured}.glb + LICENSE.md (CC0 / CC-BY 4.0; kein NC-Material ins Repo)
```

## Architektur & Konventionen
- **Application:** Member-Reihenfolge `EventBus → Input → Window → VulkanContext → Renderer → ThreadPool → AssetManager` (Zerstörung umgekehrt). Loop: `Input::NewFrame → PollEvents → EventBus::Flush → (minimiert: WaitEvents) → FixedUpdate(1/60, Akkumulator) → AssetManager::Update → OnUpdate → BeginFrame/OnRender/EndFrame`. Frame-Time auf 0,25 s begrenzt.
- **Events:** typisiertes Pub/Sub, RAII-`Subscription`, `true` = konsumiert, re-entrancy-sicher, `Enqueue` threadsicher (Zustellung bei `Flush`).
- **ThreadPool:** feste Worker (hw−1), FIFO, `Submit → future` (Exceptions via future), `Enqueue` fire-and-forget (Exceptions geloggt); Dtor arbeitet Queue ab, dann Join.
- **Vulkan-Context:** 1.3 dynamicRendering + sync2; 1.2 BDA, Descriptor Indexing (UpdateAfterBind, UpdateUnusedWhilePending), Timeline-Semaphores, hostQueryReset; 1.0 Anisotropie, depthClamp, fillModeNonSolid, MDI. Dedizierte Transfer-Queue, sonst Fallback = Graphics-Queue. `ValidationErrorCount()` zählt Validation-Errors (Smoke-Tests).
- **Frames:** `kFramesInFlight = 2`; `FrameContext.depthTexture` = Bindless-Slot des Renderer-Depth-Buffers (gesampelt in DEPTH_READ_ONLY_OPTIMAL; Barrier am Frame-Start wartet auch auf Compute/Fragment-Reads des Vorframes); pro Frame Pool, Fence, Image-Acquired-Semaphore; Render-Finished-Semaphores pro Swapchain-Image. Fence-Reset erst nach erfolgreichem Acquire. Resize → `vkDeviceWaitIdle` + Recreate. Present-Barriere: dstStage = COLOR_ATTACHMENT_OUTPUT (muss im Signal-Stage-Mask liegen).
- **Koordinaten:** negativer Viewport-Y-Flip (+Y oben, glTF-CCW bleibt), Reverse-Z (Clear 0, `GREATER_OR_EQUAL`), `D32_SFLOAT`, Swapchain sRGB BGRA8.
- **Bindless:** Set 0: `0` texture2D[], `1` sampler[], `2` image2D[] (rgba16f), `3` textureCube[] (256), `4` image2DArray[] (rgba16f, 256; Cube-Faces für Compute). Eine Pipeline-Layout, 128 B Push-Constants (`STAGE_ALL`). Default-Sampler 0 LinearRepeat, 1 LinearClamp, 2 NearestClamp, 3 ShadowCompare (GREATER_OR_EQUAL, Border = Tiefe 0 = beleuchtet). In Compute nur `SampleTextureLod` (keine impliziten Ableitungen). **Add/Remove threadsicher (Mutex).**
- **Buffer-Zugriff:** Vertex Pulling per BDA, kein Vertex-Input-State.
- **Deferred Destruction:** `DeferRelease`/`DeferCall` (nur Main-Thread) → frei, wenn der Frame-Slot nach Fence-Wait wieder dran ist. UploadQueue-Ressourcen erst nach `IsReady(ticket)` freigeben.
- **Transient-Allocator:** 8 MB/Frame, host-visible + BDA (`PushTransient<T>`).
- **UploadQueue (Phase 5):**
  - Beliebiger Thread: `CreateBuffer/CreateTexture2D(desc, UploadTicket&)` → Staging (VMA) + memcpy im Aufrufer-Thread, Copy-Recording unter Mutex in den offenen Batch. Ticket = Timeline-Wert des Batches (max-akkumuliert).
  - Main: `Renderer::BeginFrame` ruft `Submit()` (Batch → Transfer-Queue, signalisiert Timeline) und nach `vkBeginCommandBuffer` `RecordAcquires(cmd)`: fertige Batches → Acquire-Barrieren (QFOT) + Mip-Blits (Graphics) in den Frame-Cmd; Frame-Submit wartet auf den (bereits erreichten) Timeline-Wert (ALL_COMMANDS).
  - Transfer-Seite nur Mip 0; bei QFOT Release-Barrieren am Batch-Ende. Ohne QFOT (gleiche Queue) globale Memory-Barrier am Batch-Ende (sonst syncval-False-Positive über den Present→Acquire-Semaphore-Pfad).
  - `IsReady(t)` = Acquire in einem Frame aufgezeichnet. `Flush()` = blockierend (Startup/Shutdown). `ImmediateSubmit` = blockierend auf Graphics.
- **Assets (Phase 5):**
  - `AssetHandle<T>` (Index + Generation, 0 = null), `ModelHandle`. Handles halten Assets **nicht** am Leben.
  - `CreateModel(ModelData)` für generierte Geometrie (gleicher Lebenszyklus, nicht gecacht, Key leer).
  - `AssetManager` (Main-Thread): `LoadModel(path)` cached nach normalisiertem absolutem Pfad (Refcount+1), Job auf ThreadPool: `LoadGltf` + `BuildModel` (GPU-Ressourcen, Bindless-Slots, Upload). `Update()` pro Frame: Ergebnisse → Uploading → Ready (bei `IsReady`), Events `AssetLoadedEvent<Model>`/`AssetFailedEvent<Model>` (nach den Schleifen publiziert). `Release` bei 0: freigeben (Graveyard, falls Upload noch läuft; Orphan, falls Job noch läuft). Dtor: Jobs abwarten, `Flush()`, alles freigeben.
  - `MeshRenderer{ModelHandle, meshIndex}`; `SceneRenderer` löst Handles über den AssetManager auf (nullptr → nicht zeichnen).
  - `InstantiateModel(scene, handle, model)` – eine Entity pro Node (Eltern vor Kindern).
- **Datenlayouts (C++ ↔ GLSL):** `Vertex` 48 B (tangent.w = 0 → keine Tangente, Shader leitet Frame per Derivaten ab: (∇u, −∇v, N) = glTF-Konvention B = cross(N,T)·w), `GpuMaterial` 80 B (inkl. normalScale, occlusionStrength), `FrameUniforms` (frame.glsl: viewProj/view/proj/invViewProj, Kamera, Sonne, sky.xy = Sky-/IBL-Intensität, ibl = Slots + Mip-Anzahl), `DrawData` (model + normalMatrix, per Transient, 16-B-aligned), `MeshPush` (4 BDA-Adressen + materialIndex + cascade). `FrameUniforms` enthält auch Kaskaden (viewProj[4], Splits, Texelgröße, Slots, Parameter).
- **SceneRenderer (Phase 6):** `Environment::Update` (Compute, nur bei geänderten SkySettings) → PBR-Forward in HDR-Target (RGBA16F, eigenes Image, Resize → DeferRelease) + Depth → Sky-Pass (Fullscreen-Dreieck bei Depth 0, analytischer Himmel + Sonnenscheibe) → Tonemap-Pass (texelFetch, PBR Neutral / ACES fitted / None, Exposure) in die Swapchain. Eine Mesh-Pipeline mit dynamischem Cull-Mode/Front-Face (Double-Sided, negative Determinante → CW). Frustum Culling pro Submesh (CPU, AABB-Weltbox nach Arvo). `lighting` (SkySettings + iblIntensity), `post` (exposure, tonemapper), `Stats()` (draws, culled, tris).
- **CSM (Phase 7):** `ShadowSettings` (Default 4 × 4096², maxDistance 60, λ 0,8, Bias konst./Slope, Normal-Offset in Texeln, PCF-Radius, Blend 10 %, Debug-Tint). `ComputeCascades`: Practical Split, Bounding-Sphere pro Slice (rotationsinvariant, Radius quantisiert), Texel-Snapping im Licht-Raum, Reverse-Z-Ortho (`OrthoReverseZ`). Shadow-Map = D32-Array (4 Layer), Layer-Views als Attachment + Bindless-Slot. Pass: kein Culling, Depth-Clamp (Pancaking), negativer Depth-Bias (Reverse-Z), Alpha-Mask-Pipeline, Culling pro Kaskade ohne Near-Plane (`Frustum::FromViewProjection(vp, false)`), nicht geflippter Viewport (UV = NDC·0,5+0,5). Sampling (`shadow.glsl`): Kaskade per View-Tiefe, 16 Poisson-Taps × 2×2 HW-PCF, IGN-Rotation, Cross-Fade zur nächsten Kaskade, geometrische Normale für den Offset.
- **Prepass + GTAO (Phase 8):** Depth-Normal-Prepass (mesh.vert mit `invariant gl_Position` + depth_normal.frag: Alpha-Test, Normal Mapping, View-Space-Normalen RGBA16F). Danach Depth → DEPTH_READ_ONLY; Hauptpass mit Depth-Test EQUAL, ohne Write, storeOp NONE; `DrawVisible()` gemeinsam für beide Pässe (gleiches Culling/Reihenfolge). GTAO (Compute, volle Auflösung, XeGTAO-Slice-Formel, 2 Slices × 4 Schritte/Seite, quadratische Schrittverteilung, Falloff, IGN-Rauschen) → 5×5-Bilateral-Denoise (relative Tiefendifferenz). AO-Targets RGBA16F in GENERAL. `AoSettings` (radius in Welt-Einheiten, falloff, power, slices, steps, sharpness). mesh.frag: `min(Material-AO, GTAO)` auf Diffuse-IBL, spekulare Okklusion nach Lagarde auf Specular-IBL; direktes Licht unberührt. Normal-Mapping-Code in `surface.glsl`.
- **Auto-Exposure (Phase 8):** Luminanz-Histogramm (256 Bins, log2-Bereich −10…+6, Shared-Memory-Atomics) → Average-Pass (geometrisches Mittel ohne Schwarz-Bin, exponentielle Adaption, Histogramm-Reset) → `ExposureState` (BDA) → Tonemap. Key 0,3 (0,18 = klassisches Mittelgrau, wirkt hier ~1 Blende zu dunkel), `post.exposure` = Kompensation. Readback pro Frame-Slot (nach Fence-Wait gelesen) → `Stats().exposure/averageLuminance`. Debug-Views im Tonemap (AO, Normalen).
- **Bloom (Phase 7):** halbe Auflösung, bis 6 Mips, RGBA16F in GENERAL (Sampled-Slots mit GENERAL registriert). 13-Tap-Downsample (erste Stufe Karis-Average), 3×3-Tent-Upsample additiv; Compute mit globalen Memory-Barrieren. Tonemap: `mix(hdr, bloom, strength)` vor Exposure. `PostSettings`: bloom, bloomStrength 0,04, bloomRadius 0,005.
- **PBR (mesh.frag):** glTF-Metallic-Roughness, GGX + höhenkorreliertes Smith + Schlick, Lambert, Roughness-Floor 0,045; IBL Split-Sum + Multiple-Scattering (Fdez-Agüera); AO nur auf indirektes Licht.
- **Environment (IBL):** Env-Cube 256² (9 Mips per Blit) → Irradiance 32² (512 Cos-Samples) + Prefiltered 128² (5 Mips = Roughness 0…1, 256 GGX-Samples, Filtered Importance Sampling) + BRDF-LUT 256² (einmalig). Sky-Funktion in `sky.glsl` für Skybox und IBL identisch (IBL ohne Sonnenscheibe). WAR-Barrieren gegen noch laufende Frames.
- **ECS/Scene:** Entity 64 Bit, Sparse-Set-Pools; jede Entity hat Name/Transform/WorldTransform/Hierarchy; `UpdateTransforms` iterativ ohne Dirty-Flags.

## Build & Test
- Windows: VS-Generator, Startprojekt Sandbox. `Sandbox [model.glb] [--frames N]` (Default `assets/models/WaterBottle.glb`; Exit 0 ok / 1 Ladefehler / 2 Validation-Errors). Tasten: RMB+WASD/QE Kamera, T Tonemapper, −/= Exposure, Pfeile Sonne, B Bloom, P Schatten, C Kaskaden-Farben, O AO, V Debug-Ansicht (AO/Normalen), X Auto-Exposure. Sandbox erzeugt eine Bodenplatte (12 r) und setzt shadows.maxDistance = 10 r, ao.radius = 0,2 r.
- Tests: `EngineTests` (CPU), `EngineGpuTests` (echtes Vulkan-Device + Fenster). CTest: `-DENGINE_GPU_TESTS=ON` registriert GPU-Tests + `SandboxSmoke`.
- Headless (Linux-Container): `apt install glslc libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev mesa-vulkan-drivers vulkan-validationlayers xvfb`; `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGLFW_BUILD_WAYLAND=OFF`; Lauf mit `xvfb-run -a` (lavapipe), Sync-Validation via `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`.

## Phasenhistorie
1. CMake, Log, Window, Application-Loop, VulkanContext.
2. EventBus, Swapchain, Renderer (Frames in Flight, Resize).
3. Buffer, Image, Upload (Mips), Bindless, Pipeline-Builder, Shader-CMake, Deferred Release.
4. Depth, Input, FlyCamera, ECS, Scene-Graph, glTF-Loader, SceneRenderer, Transient-Allocator.
5. ThreadPool, UploadQueue (Transfer-Queue, Timeline, QFOT, Mips auf Graphics), threadsichere Bindless-Registry, AssetManager (Handles, Cache, Refcount, Events), Tests + Smoke-Test, Present-Barrier-Fix.
6. PBR (Cook-Torrance, Normal Maps inkl. Derivative-TBN), IBL aus prozeduralem Himmel (Compute), HDR-Target + Tone Mapping (PBR Neutral/ACES), Skybox, Frustum Culling, Normal-Matrix auf CPU, dynamisches Culling/Front-Face, Test-Modelle (CC0).
7. Cascaded Shadow Maps (stabil, PCF, Blend, Debug), Bloom (Compute), Primitives + `AssetManager::CreateModel`, Fix Bitangenten-Vorzeichen im Derivative-TBN.
8. Depth-Normal-Prepass (Hauptpass EQUAL), GTAO + Bilateral-Denoise, spekulare Okklusion, Histogramm-Auto-Exposure mit Adaption + Readback, Debug-Views.

## Verifikation (Stand Phase 8)
- GCC 13 und Clang: `-Wall -Wextra -Wpedantic` ohne Warnungen im Engine-Code.
- lavapipe (Mesa, Vulkan 1.4) + Khronos-Validation 1.3.275 inkl. Synchronization-Validation: Sandbox (3 Modelle), EngineGpuTests (inkl. Render-Pfad, IBL-Regeneration bei Sonnenänderung, Culling, Schatten-/Bloom-Toggles, Shadow-Map-Neuanlage zur Laufzeit, generierte Modelle, AO/Auto-Exposure-Toggles, Debug-Views, Exposure-Readback) → 0 Errors. Screenshots geprüft (WaterBottle, Spheres vs. Khronos-Referenz, PBR Neutral vs. ACES, Schatten, Kaskaden-Debug, Bloom an/aus, AO-Debug-Ansicht, Exposure-Key 0,18 vs. 0,3). CPU-Tests: Kaskaden-Splits, Ortho, Slice-Abdeckung, Stabilität (Texel-Snapping).
- ASan/UBSan: alle Tests grün (einzige Leak-Meldung aus llvmpipe-JIT-Threads). TSan: 0 Races im Engine-Code (3 Meldungen zwischen lavapipe-internen Threads).
- **Nicht getestet:** MSVC-Build; echte GPU; **QFOT-Pfad** (lavapipe hat nur eine Queue-Family).

## Offene Punkte / Einschränkungen
1. **Auf der GTX 1070 Ti mit Validation (+ Sync-Validation) laufen lassen** – prüft den QFOT-Pfad (höchste Priorität). MSVC `/W4` prüfen.
2. Resize nutzt `vkDeviceWaitIdle` (Stall); `VK_EXT_swapchain_maintenance1` wäre besser.
3. Minimiert gestartetes Fenster → 0×0-Swapchain nicht abgefangen. Uploads werden während Minimierung nicht submitted.
4. Event-Handler müssen kopierbar sein (`std::function`).
5. Upload: ein Staging-Buffer pro Upload, ganzes Modell gleichzeitig gestaged (kein Budget/Ring-Buffer); Mip-Blits kosten Frame-Zeit auf der Graphics-Queue; laufendes Parsing nicht abbrechbar (nur Skip beim Shutdown).
6. AssetManager: nur Models (Texturen als eigene Assets fehlen); fehlgeschlagene Loads bleiben bis `Release` gecacht (kein Retry/Hot-Reload).
7. Texturen nur RGBA8 (kein BCn/KTX2); Storage-Images im GLSL fest `rgba16f`.
8. Kein `VkPipelineCache`; Shader über absoluten Build-Pfad (kein Asset-Packaging).
9. glTF: Blend unsortiert/opak; ignoriert COLOR_0, Sampler, UV-Sets > 0, Skins, Animationen, Morph-Targets, basisu/webp; externe `.bin` per `fopen` (Nicht-ASCII-Pfade unter Windows).
10. AO nur Screen-Space (keine Off-Screen-Occluder, kein Temporal-Filter → bei Bewegung leichtes Rauschen möglich); GTAO in voller Auflösung ohne Depth-MIP-Kette (große Radien = Cache-Misses); Auto-Exposure ohne Zentrumsgewichtung. Schatten nur für die Sonne (keine Punkt-/Spotlights); kein Contact-Hardening (PCSS), keine Schatten-Caches (alle Kaskaden jedes Frame); Kaskade 0 kann bei weiter Kamera sehr klein sein (λ-Tuning). Bloom ohne Lens-Dirt. Sonnenscheibe (40× Radiance) dominiert den Bloom.
11. `UpdateTransforms` ohne Dirty-Flags. Culling auf der CPU, linear über alle Entities (kein BVH, kein GPU-Culling).
12. IBL-Regeneration bei jeder Sonnenänderung komplett (auf GPU ~ms; auf lavapipe langsam). Himmel ist ein einfaches analytisches Modell (kein Hosek/Bruneton).
13. Blend-Materialien weiterhin opak; Specular-/Clearcoat-/Transmission-Extensions nicht unterstützt.

## Nächste Schritte (Vorschlag)
- **Phase 9 (Vorschlag):** ImGui-Editor-Grundlage (Frame-Diagnostik mit GPU-Timestamps, Regler für Licht/Post/Schatten/AO, Asset-Status) – oder Forward+ (Clustered Lights, Punkt-/Spotlights).
- Später: Forward+/Deferred, BVH/Octree, Jolt, ImGui-Editor (Frame-Diagnostik, Asset-Status, Licht-/Tonemap-Regler).
