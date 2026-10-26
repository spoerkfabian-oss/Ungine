#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/Texture.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Renderer/Environment.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/ShadowAtlas.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Engine/Renderer/Vulkan/Pipeline.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/SpatialIndex.h"

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Engine {

class AssetManager;
class Frustum;
class GpuCulling;
class GpuScene;
class ParticleRenderer;
class TerrainRenderer;
class ParticleWorld;
class Scene;

enum class Tonemapper : std::uint32_t { PbrNeutral, Aces, None, Count }; // mirrors tonemap.frag

// tonemap.frag. HiZ: occlusion pyramid (level CullingSettings::hizDebugLevel), Culling: draws that
// only the late (occlusion) pass found visible are tinted orange. Velocity: motion vectors.
// Reflections: the screen-space reflection buffer (rgb, confidence as brightness overlay).
enum class DebugView : std::uint32_t { None, AmbientOcclusion, Normals, LightClusters, ShadowAtlas, HiZ, Culling, Lod,
                                       Velocity, Reflections, Count };

[[nodiscard]] const char* ToString(Tonemapper tonemapper);
[[nodiscard]] const char* ToString(DebugView view);

struct SceneLighting {
    SkySettings sky;                // sun + procedural sky (also drives the IBL maps)
    float       iblIntensity = 1.0f;
};

struct PostSettings {
    float      exposure        = 1.0f; // manual exposure, or compensation on top of auto exposure
    Tonemapper tonemapper      = Tonemapper::PbrNeutral;
    bool       bloom           = true;
    float      bloomStrength   = 0.04f;  // lerp weight of the blurred image
    float      bloomRadius     = 0.005f; // upsample tent radius, in UV units
    bool       autoExposure    = true;
    float      exposureKey     = 0.30f;  // scene average (geometric mean) maps to this; 0.18 = classic mid-gray
    float      adaptationSpeed = 1.5f;   // 1/s, eye adaptation towards the current average
    float      minLogLuminance = -10.0f; // histogram range (log2 luminance)
    float      maxLogLuminance = 6.0f;
    DebugView  debugView       = DebugView::None;
};

// Temporal anti-aliasing: sub-pixel jitter of the camera (Halton 2/3, 8 positions), motion vectors
// from the depth prepass (objects incl. skinning, camera; particles from their velocity), history
// reprojection with variance clipping, sharpening in the tone mapping pass.
struct TaaSettings {
    bool  enabled   = true;
    float feedback  = 0.9f;  // history weight: higher = smoother, more ghosting on fast changes
    float sharpness = 0.25f; // unsharp mask in the tone mapping pass (0 = off)
};

// Screen-space reflections: mirror rays through a closest-depth pyramid (half resolution), color
// from last frame's image, temporal accumulation, roughness-dependent blur. Replaces the IBL
// reflection where a ray found something (confidence), the IBL stays the fallback.
struct SsrSettings {
    bool          enabled      = true;
    float         intensity    = 1.0f;
    float         maxRoughness = 0.5f;  // rougher surfaces keep the IBL reflection only
    float         thickness    = 0.3f;  // meters behind a surface that still count as a hit
    float         maxDistance  = 40.0f; // meters
    std::uint32_t maxSteps     = 64;    // traversal steps per ray
    float         feedback     = 0.85f; // temporal history weight
};

// Height fog (analytic, everywhere) + volumetric fog (froxel grid in view space up to
// volumetricDistance: sun with cascade shadows, punctual lights with their shadows, sky ambient,
// FogVolume components, temporal reprojection). Beyond the grid the analytic height fog continues.
struct FogSettings {
    bool      enabled       = false;
    glm::vec3 albedo{0.8f, 0.85f, 0.9f}; // scattering color
    float     density       = 0.02f; // extinction per meter at baseHeight
    float     baseHeight    = 0.0f;
    float     heightFalloff = 0.15f; // per meter: density = d * exp(-falloff * (y - baseHeight))
    float     startDistance = 0.0f;  // meters from the camera without fog (analytic part)
    float     maxOpacity    = 1.0f;  // cap of the analytic fog (keeps the sky visible)
    float     anisotropy    = 0.6f;  // Henyey-Greenstein g: > 0 scatters forward (glow around the sun)
    float     sunScale      = 1.0f;
    float     ambientScale  = 1.0f;  // sky irradiance
    float     lightScale    = 1.0f;  // punctual lights (volumetric only)
    bool      volumetric    = true;
    float     volumetricDistance = 64.0f; // meters covered by the froxel grid
    float     temporalFeedback   = 0.9f;  // history weight of the froxel grid

    bool operator==(const FogSettings&) const = default;
};

// Ground-truth ambient occlusion (screen space, indirect light only).
struct AoSettings {
    bool          enabled      = true;
    float         radius       = 0.5f;  // world units
    float         falloff      = 0.6f;  // fraction of the radius over which occluders fade out
    float         power        = 1.5f;  // contrast of the final visibility
    std::uint32_t sliceCount   = 2;     // directions per pixel
    std::uint32_t stepsPerSide = 4;     // horizon samples per direction and side
    float         sharpness    = 20.0f; // denoise edge stopping (relative depth difference)
};

// Clustered forward shading of punctual lights (Light components).
inline constexpr std::uint32_t kClusterGridX         = 16; // mirrored in lights.glsl
inline constexpr std::uint32_t kClusterGridY         = 9;
inline constexpr std::uint32_t kClusterGridZ         = 24; // logarithmic depth slices
inline constexpr std::uint32_t kClusterMaxLights     = 256; // per cluster; longer lists are truncated
inline constexpr std::uint32_t kMaxVisibleLights     = 4096; // after CPU frustum culling

struct LightSettings {
    bool  enabled    = true;
    float clusterFar = 300.0f; // view distance covered by the depth slices; farther pixels use the last one
};

// Shadows of point and spot lights: one D32 atlas, re-rendered every frame. The most important
// shadow-casting lights (screen size of their range) get tiles: spot = 1, point = 6 (cube faces).
struct LocalShadowSettings {
    bool          enabled      = true;
    std::uint32_t atlasSize    = 4096; // power of two; changing it recreates the atlas
    std::uint32_t maxLights    = 8;    // shadowed lights per frame
    std::uint32_t maxTileSize  = 1024; // tile of a light that fills the screen (point lights: half per face)
    std::uint32_t minTileSize  = 128;
    float         depthBias    = 1.0f; // rasterizer constant bias
    float         slopeBias    = 2.0f; // rasterizer slope-scaled bias
    float         normalBias   = 1.5f; // receiver offset along its normal, in shadow texels
    float         filterRadius = 1.5f; // PCF radius, in shadow texels
};

// Visibility determination and draw submission.
struct CullingSettings {
    // GPU: compute culling of every draw (instance x submesh) per view, instances of the same
    // submesh batched into one indirect command, vkCmdDrawIndexedIndirectCount per state bucket.
    // CPU: BVH queries + one vkCmdDrawIndexed per visible submesh (fallback / comparison).
    bool          gpuDriven     = true;
    bool          occlusion     = true;  // GPU: two-phase Hi-Z occlusion culling of the camera view
    bool          freeze        = false; // GPU: keep culling with the camera + Hi-Z of the moment it was set (debug)
    std::uint32_t hizDebugLevel = 0;     // DebugView::HiZ
    // Mesh LODs (both paths, every view): the coarsest level whose simplification error, seen from
    // the camera, stays below lodPixelError pixels. Shadows use the camera's choice.
    bool          lod           = true;
    float         lodPixelError = 1.0f;
    std::int32_t  forceLod      = -1;    // debug: >= 0 draws that level (clamped per submesh)
    // GPU + occlusion: skip sun cascade casters whose shadow volume lies behind the camera Hi-Z
    // (cascades are then rendered after the depth prepass).
    bool          shadowOcclusion = true;
};

// Projected decals (Decal components): clustered like the lights, applied to opaque surfaces
// before lighting, in sort order.
inline constexpr std::uint32_t kClusterMaxDecals = 32;   // per cluster, mirrored in lights.glsl
inline constexpr std::uint32_t kMaxVisibleDecals = 1024; // after CPU frustum culling

struct DecalSettings {
    bool enabled = true;
};

// Mirrors GpuDecal in lights.glsl.
struct GpuDecal {
    glm::mat4  worldToDecal{1.0f};
    glm::vec4  color{1.0f};
    glm::vec4  emissive{0.0f};     // w: normal strength
    glm::uvec4 textures{~0u};      // texture table entries: albedo, normal, mask, emissive
    glm::vec4  params{0.0f};       // roughness, metallic, material opacity, edge fade
    glm::vec4  fade{0.0f};         // cos start, cos end
    glm::vec4  tangent{0.0f};      // decal +X (world)
    glm::vec4  axis{0.0f};         // projection direction (world)
    glm::vec4  bounds{0.0f};       // world center, radius
};
static_assert(sizeof(GpuDecal) == 192);

// Volumetric fog boxes (FogVolume components).
inline constexpr std::uint32_t kMaxFogVolumes  = 64;  // visible per frame
inline constexpr std::uint32_t kFogTileSize    = 16;  // froxel width / height in pixels, mirrored in fog.glsl
inline constexpr std::uint32_t kFogSlices      = 64;  // depth slices (quadratic distribution)

// Mirrors GpuFogVolume in fog.glsl.
struct GpuFogVolume {
    glm::mat4 worldToLocal{1.0f};
    glm::vec4 halfExtents{0.0f};   // xyz, w: edge fade (entity units)
    glm::vec4 albedoDensity{0.0f}; // rgb albedo, a density
    glm::vec4 emissive{0.0f};
    glm::vec4 bounds{0.0f};        // world center, radius
};
static_assert(sizeof(GpuFogVolume) == 128);

// Mirrors GpuShadowView in lights.glsl.
struct GpuShadowView {
    glm::mat4 viewProj{1.0f};
    glm::vec4 rect{0.0f};   // atlas UV offset + size
    glm::vec4 params{0.0f}; // x: texel world size per unit distance
};
static_assert(sizeof(GpuShadowView) == 96);

// Mirrors GpuLight in lights.glsl.
struct GpuLight {
    glm::vec3     position{0.0f};
    float         range = 0.0f;
    glm::vec3     color{0.0f};    // color * intensity
    float         spotScale  = 0.0f;
    glm::vec3     direction{0.0f, 0.0f, -1.0f};
    float         spotOffset = 1.0f;
    float         cosOuter   = -1.0f;
    float         sinOuter   = 0.0f;
    std::uint32_t type       = 0;
    std::uint32_t shadow     = ~0u; // first GpuShadowView, ~0u: unshadowed
};
static_assert(sizeof(GpuLight) == 64);

// Editor support, off by default: entity IDs per pixel (R32_UINT, written by the prepass) for
// mouse picking, and an outline around the visible pixels of chosen entities.
struct SelectionOverlay {
    bool                picking = false;
    std::vector<Entity> outlined; // needs picking
    glm::vec4           outlineColor{1.0f, 0.55f, 0.1f, 1.0f}; // rgb, a = opacity
};

struct SceneRenderStats {
    std::uint32_t drawCalls   = 0; // CPU: draw commands recorded; GPU: indirect multi-draws recorded
    std::uint32_t culled      = 0; // submeshes rejected by frustum (and occlusion) culling (camera)
    std::uint32_t shadowDraws = 0; // over all cascades (GPU: draws emitted by the culling pass)
    std::uint64_t triangles   = 0;
    std::uint32_t lights      = 0; // punctual lights after frustum culling (sent to the GPU)
    std::uint32_t decals      = 0; // decals after frustum culling
    std::uint32_t fogVolumes  = 0; // FogVolume boxes in the froxel grid this frame
    std::uint32_t terrains         = 0; // Terrain components drawn
    std::uint32_t terrainNodes     = 0; // CDLOD patches of the camera
    std::uint32_t terrainTriangles = 0;
    std::uint32_t foliageInstances = 0; // camera (uploaded)
    std::uint32_t foliageShadowInstances = 0;
    std::uint32_t foliageDrawCalls = 0; // prepass + shadows + lighting
    glm::uvec3    fogGrid{0u};     // froxel grid (0 = volumetric fog off)
    std::uint32_t lightsTotal = 0; // Light components in the scene
    std::uint32_t shadowedLights = 0; // lights with atlas tiles this frame
    std::uint32_t shadowTiles    = 0; // atlas views (spot 1, point 6)
    std::uint32_t shadowTilesRendered = 0; // views re-rendered this frame (the rest came from the cache)
    std::uint32_t localShadowDraws = 0; // CPU: draw calls; GPU: indirect multi-draws
    // CPU side
    double        cpuSpatialMs  = 0.0; // SpatialIndex::Sync (scene changes -> BVH)
    double        cpuGpuSceneMs = 0.0; // GPU scene update: instance / draw record changes + upload recording
    double        cpuCullingMs  = 0.0; // camera + light queries, draw item setup
    std::uint32_t drawItems     = 0;   // CPU path: draws (instance x submesh) submitted by any view
    // GPU scene / GPU-driven culling (counters are kFramesInFlight frames old)
    bool          gpuDriven       = false;
    std::uint32_t instances       = 0; // mesh instances on the GPU
    std::uint32_t draws           = 0; // draw records (instance x submesh)
    std::uint32_t batches         = 0; // unique submesh + winding combinations (indirect commands at most)
    std::uint32_t gpuTested       = 0;
    std::uint32_t gpuFrustumCulled = 0;
    std::uint32_t gpuOccluded     = 0;
    std::uint32_t gpuEarly        = 0; // drawn by the early pass (visible last frame)
    std::uint32_t gpuLate         = 0; // newly visible, drawn by the late pass
    std::uint32_t gpuCommands     = 0; // indirect commands written, all views
    std::uint32_t lodDraws        = 0; // camera draws with a LOD > 0 or cross-fading (GPU: counter, CPU: this frame)
    std::uint32_t transparentDraws = 0; // alpha-blended draws (sorted CPU pass, both paths)
    std::uint32_t shadowOccluded  = 0; // GPU: cascade casters culled against the camera Hi-Z
    std::uint32_t particleBatches   = 0; // emitter instances drawn (after frustum culling)
    std::uint32_t particlesDrawn    = 0; // sprites + mesh instances + ribbon points
    std::uint32_t particleDrawCalls = 0;
    std::uint32_t geometryVertices = 0, geometryVertexCapacity = 0; // geometry pool use
    std::uint32_t geometryIndices  = 0, geometryIndexCapacity  = 0;
    float         exposure         = 1.0f; // applied exposure (auto exposure: a few frames old)
    float         averageLuminance = 0.0f; // adapted scene luminance (auto exposure only)
};

// Where the final, tone mapped image goes: the swapchain image or an editor viewport texture.
// Must be in COLOR_ATTACHMENT_OPTIMAL on entry and is left in it. All scene targets (depth, HDR,
// AO, bloom, ...) are sized to `extent`.
struct RenderOutput {
    VkImage     image  = VK_NULL_HANDLE;
    VkImageView view   = VK_NULL_HANDLE;
    VkFormat    format = VK_FORMAT_UNDEFINED;
    VkExtent2D  extent{};
};

// Per frame:
//   (IBL regeneration if the sky changed, compute)
//   GPU scene update: changed instances / draw records / batches (compute scatter)
//   GPU culling (GPU path): frustum culling of every draw for the camera, cascades and local
//     shadow views, instance batching, indirect commands (compute)
//   cascaded shadow maps: depth-only per cascade (indirect draws, or CPU-culled draws)
//   local light shadows: atlas tiles for the most important spot / point lights
//   depth + view-normal prepass: early draws (visible last frame) -> Hi-Z -> late culling against
//     it -> newly visible draws (GPU path with occlusion; CPU path: BVH-culled draws)
//   GTAO + depth-aware denoise (compute)
//   clustered light assignment (compute, 16 x 9 x 24 froxels)
//   forward PBR pass (sun + cluster lights, depth test EQUAL, no overdraw) -> HDR target (RGBA16F)
//   sky pass (analytic sky + sun disk where depth is still at infinity)
//   bloom: 13-tap downsample chain + tent upsample (compute, half resolution)
//   auto exposure: luminance histogram + adapted geometric mean (compute)
//   tone mapping pass (+ bloom composite, debug views) -> swapchain image
class SceneRenderer {
public:
    // `assets` also loads the decal textures (held until they go unused, released in the dtor).
    SceneRenderer(Renderer& renderer, const VulkanContext& ctx, AssetManager& assets);
    ~SceneRenderer(); // defers GPU destruction through the renderer

    SceneRenderer(const SceneRenderer&)            = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    void Render(const FrameContext& frame, Scene& scene, const CameraData& camera); // into the swapchain image
    void Render(const FrameContext& frame, Scene& scene, const CameraData& camera, const RenderOutput& output);

    SceneLighting                         lighting;
    PostSettings                          post;
    ShadowSettings                        shadows;
    AoSettings                            ao;
    LightSettings                         lights;
    LocalShadowSettings                   localShadows;
    SelectionOverlay                      overlay;
    CullingSettings                       culling;
    TaaSettings                           taa;
    DecalSettings                         decals;
    SsrSettings                           ssr;
    FogSettings                           fog;
    // Particle effects to draw (ParticleWorld::RenderData, after its Update); null: none.
    const ParticleWorld*                  particles = nullptr;

    // Picking (overlay.picking): the entity under pixel (x, y) of the output, top-left origin.
    // The answer arrives kFramesInFlight frames later: poll TakePickResult() every frame.
    void RequestPick(std::uint32_t x, std::uint32_t y) { m_PickRequest = glm::uvec2(x, y); }
    // Engaged once per request: the entity (NullEntity = background / destroyed meanwhile).
    [[nodiscard]] std::optional<Entity> TakePickResult() { return std::exchange(m_PickResult, std::nullopt); }
    [[nodiscard]] const SceneRenderStats& Stats() const { return m_Stats; }
    // Camera cut (teleport, level change, play / stop): temporal history (TAA, reflections) restarts.
    void ResetHistory()
    {
        m_HistoryValid    = false;
        m_FogHistoryValid = false;
    }
    // Bounds of the rendered scene (BVH), synced at the start of every Render: queries and raycasts.
    [[nodiscard]] const SpatialIndex&     Spatial() const { return m_Spatial; }

private:
    // A list of draw records for one view of the CPU path, uploaded as the pass's visible list.
    struct DrawList {
        std::vector<std::uint32_t> draws;
        VkDeviceAddress            address = 0;
    };

    void EnsureTargets(VkExtent2D extent);
    void EnsureShadowMap();
    // CPU path: draws of the meshes the BVH finds in the frustum (+ within `sphere` if radius > 0).
    void CreatePipelines();  // all but the per-format tone mapping ones (TonemapPipeline)
    void RebuildPipelines(); // shader hot reload: also environment, GPU scene, culling
    // Camera: opaque + masked draws (LOD cross-fades as two entries). Shadow: every draw, one LOD.
    // Transparent: alpha-blended draws only, sorted back to front.
    enum class Gather : std::uint8_t { Camera, Shadow, Transparent };
    void GatherDraws(const Frustum& frustum, const glm::vec4& sphere, DrawList& out, Gather mode);
    void DrawTransparent(VkCommandBuffer cmd, const DrawList& list, VkDeviceAddress frameAddress, std::uint32_t flags);
    void DrawCpuCamera(VkCommandBuffer cmd, const DrawList& list, VkDeviceAddress frameAddress, bool countStats,
                       std::uint32_t flags);
    void DrawCpuShadow(VkCommandBuffer cmd, const DrawList& list, VkDeviceAddress frameAddress, std::uint32_t cascade,
                       const Pipeline& plain, const Pipeline& masked, std::uint32_t& drawCounter);
    // GPU path: the indirect draws of a culled view.
    void DrawGpuCamera(VkCommandBuffer cmd, std::uint32_t view, VkDeviceAddress frameAddress, std::uint32_t flags);
    void DrawGpuShadow(VkCommandBuffer cmd, std::uint32_t view, VkDeviceAddress frameAddress, std::uint32_t cascade,
                       const Pipeline& plain, const Pipeline& masked);
    void CullGpu(VkCommandBuffer cmd, const CameraData& camera, const std::array<Cascade, kMaxCascades>& cascades,
                 std::uint32_t cascadeCount, VkExtent2D extent);
    [[nodiscard]] bool GpuPath() const;
    void CollectLights(Scene& scene, const Frustum& frustum);
    void CollectDecals(Scene& scene, const Frustum& frustum);
    [[nodiscard]] std::uint32_t DecalTexture(const std::string& path, TextureKind kind); // table entry, ~0u: none
    void ReleaseUnusedDecalTextures();
    void CullLights(VkCommandBuffer cmd, VkDeviceAddress frameAddress);
    void CollectFogVolumes(Scene& scene, const Frustum& frustum, const glm::vec3& cameraPosition);
    void EnsureFogVolumes(VkExtent2D extent); // froxel grid for this output size
    void ReleaseFogVolumes();
    // After light culling (and all shadow maps), before the lighting pass.
    void RenderFog(VkCommandBuffer cmd, VkDeviceAddress frameAddress);
    [[nodiscard]] std::uint32_t DebugTexture() const; // slot shown by the tone mapping debug view
    bool EnsureShadowAtlas(); // true: (re)created, contents undefined
    void ReleaseShadowAtlas();
    void AssignLocalShadows(const CameraData& camera);
    void RenderLocalShadows(VkCommandBuffer cmd, VkDeviceAddress frameAddress);
    void RenderShadows(VkCommandBuffer cmd, VkDeviceAddress frameAddress,
                       const std::array<Cascade, kMaxCascades>& cascades, std::uint32_t cascadeCount);
    void RenderPrepass(VkCommandBuffer cmd, VkExtent2D extent, VkDeviceAddress frameAddress, std::uint32_t frameIndex);
    void EnsurePickingTarget(VkExtent2D extent);
    void ReleasePickingTarget();
    void ReadPick(const Scene& scene, std::uint32_t frameIndex);
    [[nodiscard]] std::pair<VkDeviceAddress, std::uint32_t> PushOutlineBits(); // address, word count
    void RenderAmbientOcclusion(VkCommandBuffer cmd, VkExtent2D extent, const CameraData& camera);
    void RenderMain(VkCommandBuffer cmd, VkExtent2D extent, VkDeviceAddress frameAddress, std::uint32_t frameIndex);
    const Pipeline& TonemapPipeline(VkFormat outputFormat);
    void RenderTaa(VkCommandBuffer cmd, VkExtent2D extent, VkDeviceAddress frameAddress);
    // Before the lighting pass: needs depth, normals + roughness, motion vectors, last frame's image.
    void RenderSsr(VkCommandBuffer cmd, VkExtent2D extent, VkDeviceAddress frameAddress);
    void RenderBloom(VkCommandBuffer cmd);
    void RenderExposure(VkCommandBuffer cmd, std::uint32_t frameIndex, float deltaTime);
    void ReadExposure(std::uint32_t frameIndex);
    void ReleaseTargets();
    void ReleaseShadowMap();

    Renderer&           m_Renderer;
    AssetManager&       m_Assets;
    Environment         m_Environment;
    std::uint64_t       m_ShaderGeneration = 0; // Renderer::ShaderGeneration the pipelines were built for
    Pipeline            m_Mesh; // cull mode + front face are dynamic
    Pipeline            m_MeshBlend; // alpha-blended, depth test without writes
    Pipeline            m_Sky;
    std::vector<std::pair<VkFormat, Pipeline>> m_Tonemap; // one per output format, built on demand
    Pipeline            m_Shadow, m_ShadowMasked; // depth only / alpha-tested
    Pipeline            m_BloomDown, m_BloomUp;
    Pipeline            m_Prepass, m_Gtao, m_GtaoDenoise, m_Histogram, m_ExposureAverage;
    Pipeline            m_LightCull;
    Pipeline            m_Taa;
    Pipeline            m_SsrHiZ, m_SsrTrace, m_SsrResolve, m_SsrBlur;
    Pipeline            m_FogInject, m_FogIntegrate;
    Buffer              m_Clusters; // per-cluster counts + light index lists (written by light_cull.comp)

    // Frame targets (recreated on resize).
    Image                      m_Depth; // reverse-Z D32, sampled by GTAO in DEPTH_READ_ONLY_OPTIMAL
    std::uint32_t              m_DepthSlot = 0;
    Image                      m_Hdr;
    std::uint32_t              m_HdrSlot = 0;
    Image                      m_Bloom; // half resolution, one level per mip
    std::vector<ImageView>     m_BloomViews;
    std::vector<std::uint32_t> m_BloomSampled, m_BloomStorage;
    Image                      m_Normals; // view space + roughness, written by the prepass
    Image                      m_Velocity; // UV motion since last frame, written by the prepass
    std::uint32_t              m_VelocitySlot = 0;
    // TAA history, ping-pong in GENERAL: this frame resolves into [m_HistoryIndex], reads the other.
    std::array<Image, 2>         m_History;
    std::array<std::uint32_t, 2> m_HistorySampled{}, m_HistoryStorage{};
    std::uint32_t                m_HistoryIndex = 0;
    bool                         m_HistoryValid = false;
    bool                         m_HistoryLayoutReady = false;
    std::uint32_t                m_ResolvedSlot = 0; // this frame's image for bloom / exposure / tone mapping
    std::uint64_t                m_FrameCounter = 0;
    std::optional<glm::mat4>     m_PrevViewProj; // last frame's camera without jitter
    bool                         m_PrevHdrValid = false; // m_Hdr holds last frame's image (SSR without TAA)
    // Screen-space reflections (half resolution, GENERAL): raw rays, temporal history ping-pong,
    // blur temp; the result ends up in m_SsrRaw (sampled by the lighting pass).
    Image                        m_SsrRaw, m_SsrTemp;
    std::array<Image, 2>         m_SsrHistory;
    std::uint32_t                m_SsrRawSampled = 0, m_SsrRawStorage = 0, m_SsrTempSampled = 0, m_SsrTempStorage = 0;
    std::array<std::uint32_t, 2> m_SsrHistorySampled{}, m_SsrHistoryStorage{};
    std::uint32_t                m_SsrIndex = 0;
    // Volumetric fog (GENERAL): scattering + extinction per froxel (temporal ping-pong) and the
    // integrated in-scattering + transmittance front to back.
    std::array<Image, 2>         m_FogInjectVolume;
    Image                        m_FogIntegrated;
    std::array<std::uint32_t, 2> m_FogInjectSampled{}, m_FogInjectStorage{};
    std::uint32_t                m_FogIntegratedSampled = 0, m_FogIntegratedStorage = 0;
    std::uint32_t                m_FogIndex = 0;
    bool                         m_FogHistoryValid = false;
    bool                         m_FogLayoutReady  = false;
    bool                         m_FogThisFrame    = false;
    float                        m_FogPrevDistance = 0.0f; // grid depth of the history
    FogSettings                  m_FogHistorySettings;     // settings the history was built with
    std::vector<GpuFogVolume>    m_FogVolumes;
    bool                         m_SsrHistoryValid = false;
    bool                         m_SsrLayoutReady = false;
    bool                         m_SsrThisFrame = false;
    Buffer                       m_SsrPyramid;
    std::uint32_t                m_SsrLevels = 0;
    Image                      m_AoRaw, m_Ao;
    std::uint32_t              m_NormalSlot = 0;
    std::uint32_t              m_AoRawSampled = 0, m_AoRawStorage = 0, m_AoSampled = 0, m_AoStorage = 0;

    // Auto exposure: histogram + adapted state (GPU), per-frame-slot readback for the stats.
    Buffer                                    m_LuminanceHistogram, m_ExposureState;
    std::array<Buffer, kFramesInFlight>       m_ExposureReadback;
    std::array<bool, kFramesInFlight>         m_ExposureReadbackValid{};
    bool                                      m_ExposureInitialized = false;
    std::optional<std::chrono::steady_clock::time_point> m_LastFrameTime;

    // Shadow map: one layer per cascade (recreated when the resolution changes).
    Image                                   m_ShadowMap;
    std::vector<ImageView>                  m_ShadowViews;
    std::array<std::uint32_t, kMaxCascades> m_ShadowSlots{};

    SpatialIndex                m_Spatial;
    std::unique_ptr<GpuScene>   m_GpuScene;   // persistent instances / draw records (both paths draw them)
    std::unique_ptr<GpuCulling> m_GpuCulling;
    std::unique_ptr<ParticleRenderer> m_Particles;
    std::unique_ptr<TerrainRenderer>  m_Terrain;
    bool                        m_ShadowOcclusion = false; // this frame: cascades culled against the Hi-Z
    glm::vec4                   m_LodCamera{0.0f}; // this frame's LOD selection (see SelectLod)
    std::uint32_t               m_LodForced = 0;
    DrawList                    m_TransparentDraws; // sorted back to front
    DrawList                    m_CameraDraws; // CPU path: prepass + lighting pass
    bool                        m_GpuFrame = false; // this frame uses the GPU path
    // Frozen culling (debug): camera of the moment `culling.freeze` was set.
    std::optional<glm::mat4>    m_FrozenViewProj;
    // Picking: entity IDs, per-slot single-pixel readback.
    Image                                m_EntityIds;
    std::uint32_t                        m_EntityIdSlot = 0;
    Pipeline                             m_PrepassPicking; // + entity ID attachment
    std::array<Buffer, kFramesInFlight>  m_PickReadback;
    std::array<bool, kFramesInFlight>    m_PickPending{};
    std::optional<glm::uvec2>            m_PickRequest;
    std::optional<Entity>                m_PickResult;

    std::vector<GpuDecal> m_Decals; // this frame's visible decals, in sort order
    Buffer                m_DecalClusters; // per-cluster counts + decal index lists (light_cull.comp)
    struct DecalTextureEntry {
        TextureHandle handle;
        std::uint64_t lastUsed = 0; // m_FrameCounter
    };
    std::unordered_map<std::string, DecalTextureEntry> m_DecalTextures; // kind + '|' + path
    std::vector<GpuLight> m_Lights; // this frame's visible lights
    std::vector<bool>     m_LightCastsShadows; // parallel to m_Lights
    std::vector<Entity>   m_LightEntities;     // parallel to m_Lights

    // Local light shadow atlas. Views are chosen and packed every frame.
    struct ShadowTile {
        glm::mat4     viewProj{1.0f};
        glm::uvec2    offset{0};   // texels
        std::uint32_t size = 0;
        glm::vec3     lightPosition{0.0f};
        float         lightRange = 0.0f;
        float         texelScale = 0.0f; // texel world size per unit distance
        bool          render     = true; // not cached: draw this frame
        std::uint32_t cacheEntry = 0;
        std::uint32_t face       = 0;
        std::uint32_t gpuView    = 0; // GPU path: culling view of this tile (render only)
    };
    // Shadow cache: each shadowed light keeps its atlas tiles (buddy allocator) across frames; a view
    // is re-rendered only when its matrix changes or a caster in the light's range changed.
    struct ShadowCacheEntry {
        Entity                    light = NullEntity;
        std::uint32_t             size  = 0;
        std::uint32_t             views = 0;
        std::array<glm::uvec2, 6> offsets{};
        std::array<glm::mat4, 6>  viewProj{};
        std::array<bool, 6>       valid{};
        glm::vec3                 position{0.0f};
        float                     range = 0.0f;
    };
    std::vector<ShadowCacheEntry> m_ShadowCache;
    ShadowTileAllocator           m_TileAllocator;
    glm::vec2                     m_CachedBias{-1.0f};
    Image                      m_ShadowAtlas;
    std::uint32_t              m_ShadowAtlasSlot = 0;
    bool                       m_ShadowAtlasWritten = false; // has left UNDEFINED (debug view)
    Pipeline                   m_LocalShadow, m_LocalShadowMasked;
    std::vector<ShadowTile>    m_ShadowTiles;
    std::vector<GpuShadowView> m_ShadowViewData;
    SceneRenderStats      m_Stats;
};

} // namespace Engine
