#pragma once
#include "Engine/Renderer/Environment.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Engine/Renderer/Vulkan/Pipeline.h"
#include "Engine/Scene/Camera.h"

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Engine {

class AssetManager;
class Frustum;
class Scene;
struct Mesh;
struct Model;

enum class Tonemapper : std::uint32_t { PbrNeutral, Aces, None, Count }; // mirrors tonemap.frag

enum class DebugView : std::uint32_t { None, AmbientOcclusion, Normals, LightClusters, Count }; // mirrors tonemap.frag

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
    float         pad        = 0.0f;
};
static_assert(sizeof(GpuLight) == 64);

struct SceneRenderStats {
    std::uint32_t drawCalls   = 0;
    std::uint32_t culled      = 0; // submeshes rejected by frustum culling (camera)
    std::uint32_t shadowDraws = 0; // over all cascades
    std::uint64_t triangles   = 0;
    std::uint32_t lights      = 0; // punctual lights after frustum culling (sent to the GPU)
    std::uint32_t lightsTotal = 0; // Light components in the scene
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
//   cascaded shadow maps: depth-only per cascade, culled against each cascade
//   depth + view-normal prepass (frustum-culled per submesh)
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
    [[nodiscard]] const SceneRenderStats& Stats() const { return m_Stats; }

private:
    // One instantiated mesh this frame, shared by the shadow and main passes.
    struct DrawItem {
        const Model*    model = nullptr;
        const Mesh*     mesh  = nullptr;
        glm::mat4       world{1.0f};
        VkDeviceAddress drawData = 0;
        VkFrontFace     frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    };

    void EnsureTargets(VkExtent2D extent);
    void EnsureShadowMap();
    void CollectDrawItems(Scene& scene);
    void CollectLights(Scene& scene, const Frustum& frustum);
    void CullLights(VkCommandBuffer cmd, VkDeviceAddress frameAddress);
    void RenderShadows(VkCommandBuffer cmd, VkDeviceAddress frameAddress,
                       const std::array<Cascade, kMaxCascades>& cascades, std::uint32_t cascadeCount);
    void DrawVisible(VkCommandBuffer cmd, const Frustum& frustum, VkDeviceAddress frameAddress, bool countStats);
    void RenderPrepass(VkCommandBuffer cmd, VkExtent2D extent, const Frustum& frustum, VkDeviceAddress frameAddress);
    void RenderAmbientOcclusion(VkCommandBuffer cmd, VkExtent2D extent, const CameraData& camera);
    void RenderMain(VkCommandBuffer cmd, VkExtent2D extent, const Frustum& frustum, VkDeviceAddress frameAddress);
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

    std::vector<DrawItem> m_DrawItems;
    std::vector<GpuLight> m_Lights; // this frame's visible lights
    SceneRenderStats      m_Stats;
};

} // namespace Engine
