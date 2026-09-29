#pragma once
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
#include <utility>
#include <vector>

namespace Engine {

class AssetManager;
class Frustum;
class GpuCulling;
class GpuScene;
class Scene;

enum class Tonemapper : std::uint32_t { PbrNeutral, Aces, None, Count }; // mirrors tonemap.frag

// tonemap.frag. HiZ: occlusion pyramid (level CullingSettings::hizDebugLevel), Culling: draws that
// only the late (occlusion) pass found visible are tinted orange.
enum class DebugView : std::uint32_t { None, AmbientOcclusion, Normals, LightClusters, ShadowAtlas, HiZ, Culling, Count };

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
};

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
    SceneRenderer(Renderer& renderer, const VulkanContext& ctx, const AssetManager& assets);
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

    // Picking (overlay.picking): the entity under pixel (x, y) of the output, top-left origin.
    // The answer arrives kFramesInFlight frames later: poll TakePickResult() every frame.
    void RequestPick(std::uint32_t x, std::uint32_t y) { m_PickRequest = glm::uvec2(x, y); }
    // Engaged once per request: the entity (NullEntity = background / destroyed meanwhile).
    [[nodiscard]] std::optional<Entity> TakePickResult() { return std::exchange(m_PickResult, std::nullopt); }
    [[nodiscard]] const SceneRenderStats& Stats() const { return m_Stats; }
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
    void GatherDraws(const Frustum& frustum, const glm::vec4& sphere, DrawList& out);
    void DrawCpuCamera(VkCommandBuffer cmd, const DrawList& list, VkDeviceAddress frameAddress, bool countStats);
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
    void CullLights(VkCommandBuffer cmd, VkDeviceAddress frameAddress);
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
    void RenderMain(VkCommandBuffer cmd, VkExtent2D extent, VkDeviceAddress frameAddress);
    const Pipeline& TonemapPipeline(VkFormat outputFormat);
    void RenderBloom(VkCommandBuffer cmd);
    void RenderExposure(VkCommandBuffer cmd, std::uint32_t frameIndex, float deltaTime);
    void ReadExposure(std::uint32_t frameIndex);
    void ReleaseTargets();
    void ReleaseShadowMap();

    Renderer&           m_Renderer;
    const AssetManager& m_Assets;
    Environment         m_Environment;
    Pipeline            m_Mesh; // cull mode + front face are dynamic
    Pipeline            m_Sky;
    std::vector<std::pair<VkFormat, Pipeline>> m_Tonemap; // one per output format, built on demand
    Pipeline            m_Shadow, m_ShadowMasked; // depth only / alpha-tested
    Pipeline            m_BloomDown, m_BloomUp;
    Pipeline            m_Prepass, m_Gtao, m_GtaoDenoise, m_Histogram, m_ExposureAverage;
    Pipeline            m_LightCull;
    Buffer              m_Clusters; // per-cluster counts + light index lists (written by light_cull.comp)

    // Frame targets (recreated on resize).
    Image                      m_Depth; // reverse-Z D32, sampled by GTAO in DEPTH_READ_ONLY_OPTIMAL
    std::uint32_t              m_DepthSlot = 0;
    Image                      m_Hdr;
    std::uint32_t              m_HdrSlot = 0;
    Image                      m_Bloom; // half resolution, one level per mip
    std::vector<ImageView>     m_BloomViews;
    std::vector<std::uint32_t> m_BloomSampled, m_BloomStorage;
    Image                      m_Normals; // view space, written by the prepass
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
