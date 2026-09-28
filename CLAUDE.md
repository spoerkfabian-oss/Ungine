# Kontext: C++20/Vulkan Game Engine (Stand nach Phase 5)

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
  Assets/    AssetHandle, AssetManager, Model, GltfLoader
  Renderer/  Renderer, SceneRenderer, Vulkan/{VkCommon,VkUtils,VulkanContext,Swapchain,Buffer,Image,Upload,Bindless,Pipeline}
engine/src/...   engine/shaders/ (mesh.vert/.frag, include/)
sandbox/src/main.cpp   tests/{Test.h,CoreTests.cpp,GpuTests.cpp}   assets/models/BoxTextured.glb
```

## Architektur & Konventionen
- **Application:** Member-Reihenfolge `EventBus → Input → Window → VulkanContext → Renderer → ThreadPool → AssetManager` (Zerstörung umgekehrt). Loop: `Input::NewFrame → PollEvents → EventBus::Flush → (minimiert: WaitEvents) → FixedUpdate(1/60, Akkumulator) → AssetManager::Update → OnUpdate → BeginFrame/OnRender/EndFrame`. Frame-Time auf 0,25 s begrenzt.
- **Events:** typisiertes Pub/Sub, RAII-`Subscription`, `true` = konsumiert, re-entrancy-sicher, `Enqueue` threadsicher (Zustellung bei `Flush`).
- **ThreadPool:** feste Worker (hw−1), FIFO, `Submit → future` (Exceptions via future), `Enqueue` fire-and-forget (Exceptions geloggt); Dtor arbeitet Queue ab, dann Join.
- **Vulkan-Context:** 1.3 dynamicRendering + sync2; 1.2 BDA, Descriptor Indexing (UpdateAfterBind, UpdateUnusedWhilePending), Timeline-Semaphores, hostQueryReset; 1.0 Anisotropie, depthClamp, fillModeNonSolid, MDI. Dedizierte Transfer-Queue, sonst Fallback = Graphics-Queue. `ValidationErrorCount()` zählt Validation-Errors (Smoke-Tests).
- **Frames:** `kFramesInFlight = 2`; pro Frame Pool, Fence, Image-Acquired-Semaphore; Render-Finished-Semaphores pro Swapchain-Image. Fence-Reset erst nach erfolgreichem Acquire. Resize → `vkDeviceWaitIdle` + Recreate. Present-Barriere: dstStage = COLOR_ATTACHMENT_OUTPUT (muss im Signal-Stage-Mask liegen).
- **Koordinaten:** negativer Viewport-Y-Flip (+Y oben, glTF-CCW bleibt), Reverse-Z (Clear 0, `GREATER_OR_EQUAL`), `D32_SFLOAT`, Swapchain sRGB BGRA8.
- **Bindless:** Set 0: `0` texture2D[], `1` sampler[], `2` image2D[] (rgba16f). Eine Pipeline-Layout, 128 B Push-Constants (`STAGE_ALL`). Default-Sampler 0 LinearRepeat, 1 LinearClamp, 2 NearestClamp. **Add/Remove threadsicher (Mutex).**
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
  - `AssetManager` (Main-Thread): `LoadModel(path)` cached nach normalisiertem absolutem Pfad (Refcount+1), Job auf ThreadPool: `LoadGltf` + `BuildModel` (GPU-Ressourcen, Bindless-Slots, Upload). `Update()` pro Frame: Ergebnisse → Uploading → Ready (bei `IsReady`), Events `AssetLoadedEvent<Model>`/`AssetFailedEvent<Model>` (nach den Schleifen publiziert). `Release` bei 0: freigeben (Graveyard, falls Upload noch läuft; Orphan, falls Job noch läuft). Dtor: Jobs abwarten, `Flush()`, alles freigeben.
  - `MeshRenderer{ModelHandle, meshIndex}`; `SceneRenderer` löst Handles über den AssetManager auf (nullptr → nicht zeichnen).
  - `InstantiateModel(scene, handle, model)` – eine Entity pro Node (Eltern vor Kindern).
- **Datenlayouts (C++ ↔ GLSL):** `Vertex` 48 B, `GpuMaterial` 80 B, `FrameUniforms`, `MeshPush` (≤128 B).
- **SceneRenderer:** ein Forward-Pass, 2 Pipelines (Backface/Double-Sided), Lambert + Ambient/AO/Emissive, Alpha-Mask per discard. `Stats()`.
- **ECS/Scene:** Entity 64 Bit, Sparse-Set-Pools; jede Entity hat Name/Transform/WorldTransform/Hierarchy; `UpdateTransforms` iterativ ohne Dirty-Flags.

## Build & Test
- Windows: VS-Generator, Startprojekt Sandbox. `Sandbox [model.glb] [--frames N]` (Default `assets/models/BoxTextured.glb`; Exit 0 ok / 1 Ladefehler / 2 Validation-Errors).
- Tests: `EngineTests` (CPU), `EngineGpuTests` (echtes Vulkan-Device + Fenster). CTest: `-DENGINE_GPU_TESTS=ON` registriert GPU-Tests + `SandboxSmoke`.
- Headless (Linux-Container): `apt install glslc libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev mesa-vulkan-drivers vulkan-validationlayers xvfb`; `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGLFW_BUILD_WAYLAND=OFF`; Lauf mit `xvfb-run -a` (lavapipe), Sync-Validation via `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`.

## Phasenhistorie
1. CMake, Log, Window, Application-Loop, VulkanContext.
2. EventBus, Swapchain, Renderer (Frames in Flight, Resize).
3. Buffer, Image, Upload (Mips), Bindless, Pipeline-Builder, Shader-CMake, Deferred Release.
4. Depth, Input, FlyCamera, ECS, Scene-Graph, glTF-Loader, SceneRenderer, Transient-Allocator.
5. ThreadPool, UploadQueue (Transfer-Queue, Timeline, QFOT, Mips auf Graphics), threadsichere Bindless-Registry, AssetManager (Handles, Cache, Refcount, Events), Tests + Smoke-Test, Present-Barrier-Fix.

## Verifikation (Stand Phase 5)
- GCC 13 und Clang: `-Wall -Wextra -Wpedantic` ohne Warnungen im Engine-Code.
- lavapipe (Mesa, Vulkan 1.4) + Khronos-Validation 1.3.275 inkl. Synchronization-Validation: Sandbox, EngineGpuTests → 0 Errors. Screenshot geprüft (texturierte Box).
- ASan/UBSan: alle Tests grün (einzige Leak-Meldung aus llvmpipe-JIT-Threads). TSan: 0 Races im Engine-Code (2 Meldungen intern in `libvulkan_lvp.so`).
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
10. Kein Frustum Culling, `inverse()` pro Vertex, kein HDR/Tone Mapping.
11. `UpdateTransforms` ohne Dirty-Flags.

## Nächste Schritte (Vorschlag)
- **Phase 6:** PBR (Cook-Torrance, Normal Maps mit Derivative-TBN-Fallback, IBL), HDR-Target + Tone Mapping, Frustum Culling (AABBs pro Submesh vorhanden), danach CSM.
- Später: Bloom, SSAO, Forward+/Deferred, BVH/Octree, Jolt, ImGui-Editor (Frame-Diagnostik, Asset-Status).
