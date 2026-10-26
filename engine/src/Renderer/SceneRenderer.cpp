#include "Engine/Renderer/SceneRenderer.h"
#include "GpuCulling.h"
#include "GpuScene.h"
#include "ParticleRenderer.h"
#include "TerrainRenderer.h"
#include "Engine/Particles/ParticleWorld.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Model.h"
#include "Engine/Core/Platform.h"
#include "Engine/Scene/Components.h"
#include "Engine/Renderer/ShadowAtlas.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Frustum.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <numeric>
#include <utility>

namespace Engine {

namespace {
constexpr VkFormat      kHdrFormat     = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat      kShadowFormat  = VK_FORMAT_D32_SFLOAT;
constexpr std::uint32_t kMaxBloomMips  = 6;

struct FrameUniforms { // mirrors FrameData in frame.glsl
    glm::mat4  viewProj;
    glm::mat4  view;
    glm::mat4  proj;
    glm::mat4  invViewProj;
    glm::mat4  prevViewProj;
    glm::mat4  unjitteredViewProj;
    glm::vec4  cameraPosition;
    glm::vec4  sunDirection;
    glm::vec4  sunRadiance;
    glm::vec4  sky;
    glm::uvec4 ibl;
    glm::mat4  cascadeViewProj[kMaxCascades];
    glm::vec4  cascadeSplits;
    glm::vec4  cascadeTexel;
    glm::uvec4 shadowMaps;
    glm::vec4  shadowParams;
    glm::uvec4 shadowInfo;
    glm::uvec4 aoInfo;
    glm::vec4  clusterParams; // x: slice scale, y: slice bias, zw: clusters per pixel
    glm::vec4  clusterDepth;  // x: near, y: far
    glm::uvec4 lightInfo;     // x: light count
    glm::uvec4 localShadowInfo;   // x: atlas slot, y: atlas size
    glm::vec4  localShadowParams; // x: normal bias (texels), y: PCF radius (texels)
    glm::uvec4 hizInfo;           // xy: Hi-Z level 0 size, z: levels, w: debug level
    glm::vec4  taaInfo;           // xy: jitter (NDC), z: frame time, w: frame counter
    glm::uvec4 decalInfo;         // x: decal count
    glm::uvec4 ssrInfo;           // x: reflection slot, y: enabled
    glm::vec4  ssrParams;         // x: intensity, zw: 1 / output size
    glm::vec4  fogColor;          // rgb: albedo, w: density at the base height
    glm::vec4  fogHeight;         // x: base height, y: falloff, z: start distance, w: max opacity
    glm::vec4  fogLight;          // x: anisotropy, y: sun, z: ambient, w: punctual lights
    glm::uvec4 fogInfo;           // x: enabled, y: integrated volume slot, z: volume count, w: volumetric
    glm::vec4  fogGrid;           // x: grid depth, y: slices, zw: screen UV -> grid UV
    VkDeviceAddress lights;
    VkDeviceAddress clusters;
    VkDeviceAddress shadowViews;
    VkDeviceAddress vertices;  // geometry pool
    VkDeviceAddress materials;
    VkDeviceAddress submeshes;
    VkDeviceAddress instances; // GPU scene
    VkDeviceAddress jointMatrices;
    VkDeviceAddress draws;
    VkDeviceAddress hiz;
    VkDeviceAddress textureTable; // material texture entry -> bindless slot
    VkDeviceAddress prevJointMatrices;
    VkDeviceAddress decals;
    VkDeviceAddress decalClusters;
    VkDeviceAddress fogVolumes;
};

struct MeshPush { // mirrors MeshPush in mesh_common.glsl
    VkDeviceAddress frame;
    VkDeviceAddress visible; // draw record per instance (gl_InstanceIndex)
    std::uint32_t   cascade;
    std::uint32_t   flags;   // MESH_TINT_LATE
};
constexpr std::uint32_t kMeshTintLate = 1;
constexpr std::uint32_t kMeshTintLod  = 2;
static_assert(sizeof(MeshPush) <= kPushConstantSize);

struct TaaPush { // mirrors TaaPush in taa.comp
    VkDeviceAddress frame;
    std::uint32_t   current;
    std::uint32_t   history;
    std::uint32_t   target;
    std::uint32_t   depth;
    std::uint32_t   velocity;
    float           feedback;
    std::uint32_t   valid;
    std::uint32_t   pad;
    glm::uvec2      size;
};

// Halton (2, 3) jitter positions in pixels, [-0.5, 0.5).
glm::vec2 HaltonJitter(std::uint64_t frame)
{
    const auto halton = [](std::uint32_t index, std::uint32_t base) {
        float f = 1.0f, r = 0.0f;
        for (std::uint32_t i = index; i > 0; i /= base) {
            f /= static_cast<float>(base);
            r += f * static_cast<float>(i % base);
        }
        return r;
    };
    const auto index = static_cast<std::uint32_t>(frame % 8) + 1;
    return {halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
}

struct SsrPush { // mirrors SsrPush in ssr.glsl
    VkDeviceAddress frame;
    VkDeviceAddress pyramid;
    glm::uvec2      fullSize;
    glm::uvec2      size;
    std::uint32_t   levels;
    std::uint32_t   depth;
    std::uint32_t   normals;
    std::uint32_t   velocity;
    std::uint32_t   source;
    std::uint32_t   history;
    std::uint32_t   target;
    std::uint32_t   mode;
    float           maxRoughness;
    float           thickness;
    float           maxDistance;
    float           feedback;
};
static_assert(sizeof(SsrPush) <= kPushConstantSize);

struct SsrHiZPush { // mirrors SsrHiZPush in ssr_hiz.comp
    VkDeviceAddress src;
    VkDeviceAddress dst;
    glm::uvec2      srcSize;
    glm::uvec2      dstSize;
    std::uint32_t   depthSlot;
    std::uint32_t   fromDepth;
};

struct FogPush { // mirrors FogPush in fog_inject.comp / fog_integrate.comp
    VkDeviceAddress frame;
    std::uint32_t   source;
    std::uint32_t   target;
    std::uint32_t   valid;
    std::uint32_t   gridX;
    std::uint32_t   gridY;
    std::uint32_t   gridZ;
    float           feedback;
    float           prevDistance;
};
static_assert(sizeof(FogPush) == 40);

struct TonemapPush { // mirrors TonemapPush in tonemap.frag
    VkDeviceAddress state;
    std::uint32_t   hdrTexture;
    std::uint32_t   tonemapper;
    float           exposure;
    std::uint32_t   bloomTexture;
    float           bloomStrength;
    std::uint32_t   autoExposure;
    std::uint32_t   debugView;
    std::uint32_t   debugTexture;
    VkDeviceAddress frame; // light clusters view
    std::uint32_t   idTexture;
    std::uint32_t   outlineWords;
    VkDeviceAddress outline;
    glm::vec4       outlineColor;
    float           sharpness; // TAA unsharp mask, 0 = off
};

struct LightCullPush { // mirrors CullPush in light_cull.comp
    VkDeviceAddress frame;
    VkDeviceAddress clusters;
    VkDeviceAddress decalClusters;
};

constexpr std::uint32_t kClusterCount = kClusterGridX * kClusterGridY * kClusterGridZ;
constexpr std::uint32_t kLightCullGroupSize = 128; // light_cull.comp
static_assert(kClusterCount % kLightCullGroupSize == 0);

struct GtaoPush { // mirrors GtaoPush in gtao.comp
    std::uint32_t depth;
    std::uint32_t normals;
    std::uint32_t dst;
    std::uint32_t sliceCount;
    glm::vec2     invSize;
    glm::vec2     tanHalfFov;
    float         nearPlane;
    float         radius;
    float         falloff;
    float         power;
    std::uint32_t stepsPerSide;
};

struct DenoisePush { // mirrors DenoisePush in gtao_denoise.comp
    std::uint32_t src;
    std::uint32_t depth;
    std::uint32_t dst;
    std::uint32_t pad;
    glm::vec2     invSize;
    float         nearPlane;
    float         sharpness;
};

struct HistogramPush { // mirrors HistogramPush in luminance_histogram.comp
    VkDeviceAddress histogram;
    std::uint32_t   hdr;
    float           minLogLuminance;
    float           invLogLuminanceRange;
    std::uint32_t   pad;
    glm::uvec2      size;
};

struct AveragePush { // mirrors AveragePush in exposure_average.comp
    VkDeviceAddress histogram;
    VkDeviceAddress state;
    float           minLogLuminance;
    float           logLuminanceRange;
    float           pixelCount;
    float           adaptation;
    float           key;
    float           compensation;
};
static_assert(sizeof(TonemapPush) <= kPushConstantSize && sizeof(GtaoPush) <= kPushConstantSize &&
              sizeof(AveragePush) <= kPushConstantSize);

constexpr VkFormat kNormalFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kVelocityFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kEntityIdFormat = VK_FORMAT_R32_UINT;
constexpr VkFormat kAoFormat     = VK_FORMAT_R16G16B16A16_SFLOAT; // matches the rgba16f storage binding

struct BloomPush { // mirrors BloomPush in bloom_*.comp
    std::uint32_t src;
    std::uint32_t dst;
    glm::uvec2    dstSize;
    glm::vec2     srcTexel;
    std::uint32_t karis;
    float         radius;
};

VkRenderingAttachmentInfo Attachment(VkImageView view, VkImageLayout layout, VkAttachmentLoadOp load)
{
    VkRenderingAttachmentInfo a{};
    a.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a.imageView   = view;
    a.imageLayout = layout;
    a.loadOp      = load;
    a.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    return a;
}

void BeginRendering(VkCommandBuffer cmd, VkExtent2D extent, const VkRenderingAttachmentInfo* color,
                    const VkRenderingAttachmentInfo* depth, std::uint32_t colorCount = 1)
{
    VkRenderingInfo info{};
    info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea           = {{0, 0}, extent};
    info.layerCount           = 1;
    info.colorAttachmentCount = color ? colorCount : 0u;
    info.pColorAttachments    = color;
    info.pDepthAttachment     = depth;
    vkCmdBeginRendering(cmd, &info);
}

// Execution + memory dependency between compute passes (and on to the tone mapping read).
void MemoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask  = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask  = dstStage;
    barrier.dstAccessMask = dstAccess;
    VkDependencyInfo dep{};
    dep.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkExtent2D MipExtent(VkExtent2D base, std::uint32_t mip)
{
    return {std::max(base.width >> mip, 1u), std::max(base.height >> mip, 1u)};
}
} // namespace

const char* ToString(DebugView v)
{
    switch (v) {
    case DebugView::None:             return "Off";
    case DebugView::AmbientOcclusion: return "AO";
    case DebugView::Normals:          return "Normals";
    case DebugView::LightClusters:    return "Light clusters";
    case DebugView::ShadowAtlas:      return "Shadow atlas";
    case DebugView::HiZ:              return "Hi-Z";
    case DebugView::Culling:          return "Culling (late = orange)";
    case DebugView::Lod:              return "LOD (1 green, 2 yellow, 3 red)";
    case DebugView::Velocity:         return "Motion vectors";
    case DebugView::Reflections:      return "Reflections (SSR)";
    default:                          return "?";
    }
}

const char* ToString(Tonemapper t)
{
    switch (t) {
    case Tonemapper::PbrNeutral: return "PBR Neutral";
    case Tonemapper::Aces:       return "ACES";
    case Tonemapper::None:       return "None";
    default:                     return "?";
    }
}

SceneRenderer::SceneRenderer(Renderer& renderer, const VulkanContext& ctx, AssetManager& assets)
    : m_Renderer(renderer), m_Assets(assets), m_Environment(renderer)
{
    CreatePipelines();
    m_ShaderGeneration = renderer.ShaderGeneration();

    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    m_LuminanceHistogram = Buffer(ctx, {.size = 256 * sizeof(std::uint32_t), .usage = kStorage,
                                        .debugName = "LuminanceHistogram"});
    m_ExposureState      = Buffer(ctx, {.size = 16, .usage = kStorage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        .debugName = "ExposureState"});
    m_Clusters           = Buffer(ctx, {.size  = std::uint64_t{kClusterCount} * (1 + kClusterMaxLights) * sizeof(std::uint32_t),
                                        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .debugName = "LightClusters"});
    m_DecalClusters      = Buffer(ctx, {.size  = std::uint64_t{kClusterCount} * (1 + kClusterMaxDecals) * sizeof(std::uint32_t),
                                        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .debugName = "DecalClusters"});
    for (Buffer& b : m_PickReadback)
        b = Buffer(ctx, {.size = 16, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .memory = MemoryUsage::Readback,
                         .debugName = "PickReadback"});
    for (Buffer& b : m_ExposureReadback)
        b = Buffer(ctx, {.size = 16, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .memory = MemoryUsage::Readback,
                         .debugName = "ExposureReadback"});
    m_GpuScene   = std::make_unique<GpuScene>(renderer);
    m_GpuCulling = std::make_unique<GpuCulling>(renderer);
    m_Particles  = std::make_unique<ParticleRenderer>(renderer, assets, kHdrFormat, kVelocityFormat, kDepthFormat);
    m_Terrain    = std::make_unique<TerrainRenderer>(renderer, assets,
                                                     TerrainRenderer::Formats{.hdr      = kHdrFormat,
                                                                              .velocity = kVelocityFormat,
                                                                              .normal   = kNormalFormat,
                                                                              .entityId = kEntityIdFormat,
                                                                              .depth    = kDepthFormat,
                                                                              .shadow   = kShadowFormat});
}

void SceneRenderer::CreatePipelines()
{
    // Built completely before anything is replaced: a failure keeps the old pipelines.
    struct {
        Pipeline Prepass, PrepassPicking, Mesh, Sky, Shadow, ShadowMasked, LocalShadow, LocalShadowMasked, BloomDown,
            BloomUp, Gtao, GtaoDenoise, Histogram, ExposureAverage, LightCull, MeshBlend, Taa, SsrHiZ, SsrTrace, SsrResolve,
            SsrBlur, FogInject, FogIntegrate;
    } built;
    const VkDevice         device = m_Renderer.GetContext().Device();
    const VkPipelineLayout layout = m_Renderer.GetBindless().PipelineLayout();

    // Prepass writes depth + view normals; the lighting pass then only shades the visible
    // surface (depth EQUAL, no writes). Both use mesh.vert with an invariant gl_Position.
    built.Prepass = GraphicsPipelineBuilder{}
                        .SetShaders(ShaderPath("mesh.vert.spv"), ShaderPath("depth_normal.frag.spv"))
                        .AddColorAttachment(kNormalFormat)
                        .AddColorAttachment(kVelocityFormat)
                        .SetDepthFormat(kDepthFormat)
                        .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL) // reverse-Z
                        .SetDynamicCulling(true)
                        .SetDebugName("DepthNormalPrepass")
                        .Build(device, layout);
    built.PrepassPicking = GraphicsPipelineBuilder{}
                               .SetShaders(ShaderPath("mesh.vert.spv"), ShaderPath("depth_normal_id.frag.spv"))
                               .AddColorAttachment(kNormalFormat)
                               .AddColorAttachment(kVelocityFormat)
                               .AddColorAttachment(kEntityIdFormat)
                               .SetDepthFormat(kDepthFormat)
                               .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL)
                               .SetDynamicCulling(true)
                               .SetDebugName("DepthNormalIdPrepass")
                               .Build(device, layout);
    built.Mesh = GraphicsPipelineBuilder{}
                     .SetShaders(ShaderPath("mesh.vert.spv"), ShaderPath("mesh.frag.spv"))
                     .AddColorAttachment(kHdrFormat)
                     .AddColorAttachment(kVelocityFormat) // the prepass wrote it
                     .SetAttachmentBlend(1, BlendMode::NoWrite)
                     .SetDepthFormat(kDepthFormat)
                     .SetDepth(true, false, VK_COMPARE_OP_EQUAL)
                     .SetDynamicCulling(true)
                     .SetDebugName("MeshPbr")
                     .Build(device, layout);

    built.MeshBlend = GraphicsPipelineBuilder{}
                          .SetShaders(ShaderPath("mesh.vert.spv"), ShaderPath("mesh_blend.frag.spv"))
                          .AddColorAttachment(kHdrFormat)
                          .AddColorAttachment(kVelocityFormat)
                          .SetDepthFormat(kDepthFormat)
                          .SetDepth(true, false, VK_COMPARE_OP_GREATER_OR_EQUAL)
                          .SetDynamicCulling(true)
                          .SetBlend(BlendMode::Alpha)
                          .SetAttachmentBlend(1, BlendMode::Coverage)
                          .SetDebugName("MeshBlend")
                          .Build(device, layout);

    // Depth 0 = infinity: passes only where no geometry was drawn. No depth writes.
    built.Sky = GraphicsPipelineBuilder{}
                    .SetShaders(ShaderPath("fullscreen.vert.spv"), ShaderPath("sky.frag.spv"))
                    .AddColorAttachment(kHdrFormat)
                    .AddColorAttachment(kVelocityFormat)
                    .SetAttachmentBlend(1, BlendMode::NoWrite)
                    .SetDepthFormat(kDepthFormat)
                    .SetDepth(true, false, VK_COMPARE_OP_GREATER_OR_EQUAL)
                    .SetDebugName("Sky")
                    .Build(device, layout);


    // Shadow casters: no culling (thin/open meshes cast from both sides), reverse-Z, depth clamp
    // (pancaking of casters in front of a cascade), slope-scaled bias set per frame.
    GraphicsPipelineBuilder shadow;
    shadow.SetShaders(ShaderPath("shadow.vert.spv"))
        .SetDepthFormat(kShadowFormat)
        .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL)
        .SetDepthClamp(true)
        .SetDynamicDepthBias(true)
        .SetDebugName("ShadowDepth");
    built.Shadow       = shadow.Build(device, layout);
    built.ShadowMasked = shadow.SetShaders(ShaderPath("shadow.vert.spv"), ShaderPath("shadow_mask.frag.spv"))
                             .SetDebugName("ShadowDepthMasked")
                             .Build(device, layout);

    // Local lights: perspective views, no depth clamp (casters behind the light must not pancake
    // onto its near plane).
    GraphicsPipelineBuilder localShadow;
    localShadow.SetShaders(ShaderPath("shadow.vert.spv"))
        .SetDepthFormat(kShadowFormat)
        .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL)
        .SetDynamicDepthBias(true)
        .SetDebugName("LocalShadowDepth");
    built.LocalShadow       = localShadow.Build(device, layout);
    built.LocalShadowMasked = localShadow.SetShaders(ShaderPath("shadow.vert.spv"), ShaderPath("shadow_mask.frag.spv"))
                                  .SetDebugName("LocalShadowDepthMasked")
                                  .Build(device, layout);

    const auto compute = [&](const char* spv, const char* name) {
        return CreateComputePipeline(device, layout, ShaderPath(spv), name);
    };
    built.BloomDown       = compute("bloom_downsample.comp.spv", "BloomDownsample");
    built.BloomUp         = compute("bloom_upsample.comp.spv", "BloomUpsample");
    built.Gtao            = compute("gtao.comp.spv", "Gtao");
    built.GtaoDenoise     = compute("gtao_denoise.comp.spv", "GtaoDenoise");
    built.Histogram       = compute("luminance_histogram.comp.spv", "LuminanceHistogram");
    built.ExposureAverage = compute("exposure_average.comp.spv", "ExposureAverage");
    built.LightCull       = compute("light_cull.comp.spv", "LightCull");
    built.Taa             = compute("taa.comp.spv", "TemporalAA");
    built.SsrHiZ          = compute("ssr_hiz.comp.spv", "SsrDepthPyramid");
    built.SsrTrace        = compute("ssr_trace.comp.spv", "SsrTrace");
    built.SsrResolve      = compute("ssr_resolve.comp.spv", "SsrResolve");
    built.SsrBlur         = compute("ssr_blur.comp.spv", "SsrBlur");
    built.FogInject       = compute("fog_inject.comp.spv", "FogInject");
    built.FogIntegrate    = compute("fog_integrate.comp.spv", "FogIntegrate");


    const auto replace = [&](Pipeline& current, Pipeline& fresh) {
        if (current)
            m_Renderer.DeferRelease(std::move(current)); // frames in flight may still use it
        current = std::move(fresh);
    };
    replace(m_Prepass, built.Prepass);
    replace(m_PrepassPicking, built.PrepassPicking);
    replace(m_Mesh, built.Mesh);
    replace(m_Sky, built.Sky);
    replace(m_Shadow, built.Shadow);
    replace(m_ShadowMasked, built.ShadowMasked);
    replace(m_LocalShadow, built.LocalShadow);
    replace(m_LocalShadowMasked, built.LocalShadowMasked);
    replace(m_BloomDown, built.BloomDown);
    replace(m_BloomUp, built.BloomUp);
    replace(m_Gtao, built.Gtao);
    replace(m_GtaoDenoise, built.GtaoDenoise);
    replace(m_Histogram, built.Histogram);
    replace(m_ExposureAverage, built.ExposureAverage);
    replace(m_LightCull, built.LightCull);
    replace(m_MeshBlend, built.MeshBlend);
    replace(m_Taa, built.Taa);
    replace(m_SsrHiZ, built.SsrHiZ);
    replace(m_SsrTrace, built.SsrTrace);
    replace(m_SsrResolve, built.SsrResolve);
    replace(m_SsrBlur, built.SsrBlur);
    replace(m_FogInject, built.FogInject);
    replace(m_FogIntegrate, built.FogIntegrate);
    for (auto& [format, pipeline] : m_Tonemap) // rebuilt on demand
        m_Renderer.DeferRelease(std::move(pipeline));
    m_Tonemap.clear();
}

void SceneRenderer::RebuildPipelines()
{
    try {
        CreatePipelines();
        m_Environment.RebuildPipelines();
        m_GpuScene->RebuildPipelines();
        m_GpuCulling->RebuildPipelines();
        m_Particles->CreatePipelines();
        m_Terrain->CreatePipelines();
        ENGINE_INFO("Scene renderer: pipelines rebuilt (shader generation {})", m_Renderer.ShaderGeneration());
    } catch (const std::exception& e) {
        ENGINE_ERROR("Scene renderer: pipeline rebuild failed, keeping the old ones: {}", e.what());
    }
}

SceneRenderer::~SceneRenderer()
{
    m_GpuCulling.reset();
    m_GpuScene.reset();
    m_Particles.reset();
    m_Terrain.reset();
    ReleaseTargets();
    ReleaseShadowMap();
    ReleaseShadowAtlas();
    ReleasePickingTarget();
    ReleaseFogVolumes();
    for (auto& [format, pipeline] : m_Tonemap)
        m_Renderer.DeferRelease(std::move(pipeline));
    for (Pipeline* p : {&m_Mesh, &m_Sky, &m_Shadow, &m_ShadowMasked, &m_BloomDown, &m_BloomUp,
                        &m_Prepass, &m_Gtao, &m_GtaoDenoise, &m_Histogram, &m_ExposureAverage, &m_LightCull,
                        &m_LocalShadow, &m_LocalShadowMasked, &m_PrepassPicking, &m_MeshBlend, &m_Taa, &m_SsrHiZ,
                        &m_SsrTrace, &m_SsrResolve, &m_SsrBlur, &m_FogInject, &m_FogIntegrate})
        m_Renderer.DeferRelease(std::move(*p));
    m_Renderer.DeferRelease(std::move(m_Clusters));
    m_Renderer.DeferRelease(std::move(m_DecalClusters));
    if (m_SsrPyramid)
        m_Renderer.DeferRelease(std::move(m_SsrPyramid));
    for (auto& [key, entry] : m_DecalTextures)
        m_Assets.Release(entry.handle);
    m_DecalTextures.clear();
    m_Renderer.DeferRelease(std::move(m_LuminanceHistogram));
    m_Renderer.DeferRelease(std::move(m_ExposureState));
    m_Renderer.DeferRelease(std::move(m_ExposureReadback));
    m_Renderer.DeferRelease(std::move(m_PickReadback));
}

void SceneRenderer::ReleaseTargets()
{
    if (!m_Hdr)
        return;
    // Frames in flight may still read them: slots and images go through the deferred queue.
    Renderer* r = &m_Renderer;
    std::vector<std::uint32_t> sampled = m_BloomSampled;
    std::vector<std::uint32_t> storage = m_BloomStorage;
    sampled.insert(sampled.end(), {m_DepthSlot, m_HdrSlot, m_NormalSlot, m_AoRawSampled, m_AoSampled, m_VelocitySlot,
                                   m_HistorySampled[0], m_HistorySampled[1], m_SsrRawSampled, m_SsrTempSampled,
                                   m_SsrHistorySampled[0], m_SsrHistorySampled[1]});
    storage.insert(storage.end(), {m_AoRawStorage, m_AoStorage, m_HistoryStorage[0], m_HistoryStorage[1], m_SsrRawStorage,
                                   m_SsrTempStorage, m_SsrHistoryStorage[0], m_SsrHistoryStorage[1]});
    r->DeferCall([r, sampled = std::move(sampled), storage = std::move(storage)] {
        BindlessRegistry& b = r->GetBindless();
        for (std::uint32_t s : sampled)
            b.RemoveSampledImage(s);
        for (std::uint32_t s : storage)
            b.RemoveStorageImage(s);
    });
    r->DeferRelease(std::move(m_BloomViews));
    for (Image* image : {&m_Depth, &m_Bloom, &m_Hdr, &m_Normals, &m_AoRaw, &m_Ao, &m_Velocity, &m_History[0], &m_History[1],
                         &m_SsrRaw, &m_SsrTemp, &m_SsrHistory[0], &m_SsrHistory[1]})
        r->DeferRelease(std::move(*image));
    m_HistoryValid       = false;
    m_HistoryLayoutReady = false;
    m_PrevHdrValid       = false;
    m_SsrHistoryValid    = false;
    m_SsrLayoutReady     = false;
    m_BloomViews.clear();
    m_BloomSampled.clear();
    m_BloomStorage.clear();
}

void SceneRenderer::ReleaseShadowMap()
{
    if (!m_ShadowMap)
        return;
    Renderer* r = &m_Renderer;
    r->DeferCall([r, slots = m_ShadowSlots] {
        for (std::uint32_t s : slots)
            r->GetBindless().RemoveSampledImage(s);
    });
    r->DeferRelease(std::move(m_ShadowViews));
    r->DeferRelease(std::move(m_ShadowMap));
    m_ShadowViews.clear();
}

void SceneRenderer::EnsureTargets(VkExtent2D extent)
{
    if (m_Hdr && m_Hdr.Extent().width == extent.width && m_Hdr.Extent().height == extent.height)
        return;
    ReleaseTargets();

    const VulkanContext& ctx      = m_Renderer.GetContext();
    BindlessRegistry&    bindless = m_Renderer.GetBindless();

    m_Hdr     = Image(ctx, {.extent    = {extent.width, extent.height, 1},
                            .format    = kHdrFormat,
                            .usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .debugName = "HdrColor"});
    m_HdrSlot = bindless.AddSampledImage(m_Hdr.View());

    m_Depth     = Image(ctx, {.extent    = {extent.width, extent.height, 1},
                              .format    = kDepthFormat,
                              .usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              .debugName = "SceneDepth"});
    m_DepthSlot = bindless.AddSampledImage(m_Depth.View(), VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);

    m_Normals    = Image(ctx, {.extent    = {extent.width, extent.height, 1},
                               .format    = kNormalFormat,
                               .usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               .debugName = "ViewNormals"});
    m_NormalSlot = bindless.AddSampledImage(m_Normals.View());

    m_Velocity     = Image(ctx, {.extent    = {extent.width, extent.height, 1},
                                 .format    = kVelocityFormat,
                                 .usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 .debugName = "MotionVectors"});
    m_VelocitySlot = bindless.AddSampledImage(m_Velocity.View());
    // Reflections at half resolution (rounded up), always GENERAL.
    const VkExtent2D halfSize = MipExtent({extent.width + 1, extent.height + 1}, 1);
    const auto makeSsr = [&](const char* name, std::uint32_t& sampledSlot, std::uint32_t& storageSlot) {
        Image image = Image(ctx, {.extent    = {halfSize.width, halfSize.height, 1},
                                  .format    = kHdrFormat,
                                  .usage     = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  .debugName = name});
        sampledSlot = bindless.AddSampledImage(image.View(), VK_IMAGE_LAYOUT_GENERAL);
        storageSlot = bindless.AddStorageImage(image.View());
        return image;
    };
    m_SsrRaw        = makeSsr("SsrRaw", m_SsrRawSampled, m_SsrRawStorage);
    m_SsrTemp       = makeSsr("SsrTemp", m_SsrTempSampled, m_SsrTempStorage);
    m_SsrHistory[0] = makeSsr("SsrHistory", m_SsrHistorySampled[0], m_SsrHistoryStorage[0]);
    m_SsrHistory[1] = makeSsr("SsrHistory", m_SsrHistorySampled[1], m_SsrHistoryStorage[1]);

    for (std::uint32_t i = 0; i < 2; ++i) { // TAA history: storage writes, sampled reads, always GENERAL
        m_History[i]        = Image(ctx, {.extent    = {extent.width, extent.height, 1},
                                          .format    = kHdrFormat,
                                          .usage     = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                          .debugName = "TaaHistory"});
        m_HistorySampled[i] = bindless.AddSampledImage(m_History[i].View(), VK_IMAGE_LAYOUT_GENERAL);
        m_HistoryStorage[i] = bindless.AddStorageImage(m_History[i].View());
    }

    // AO targets stay in GENERAL (storage writes, then sampled by the next pass).
    const auto makeAo = [&](const char* name) {
        return Image(ctx, {.extent    = {extent.width, extent.height, 1},
                           .format    = kAoFormat,
                           .usage     = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           .debugName = name});
    };
    m_AoRaw        = makeAo("AmbientOcclusionRaw");
    m_Ao           = makeAo("AmbientOcclusion");
    m_AoRawSampled = bindless.AddSampledImage(m_AoRaw.View(), VK_IMAGE_LAYOUT_GENERAL);
    m_AoRawStorage = bindless.AddStorageImage(m_AoRaw.View());
    m_AoSampled    = bindless.AddSampledImage(m_Ao.View(), VK_IMAGE_LAYOUT_GENERAL);
    m_AoStorage    = bindless.AddStorageImage(m_Ao.View());

    // Bloom chain at half resolution, down to at least 1x1 per level.
    const VkExtent2D    half = MipExtent(extent, 1);
    const std::uint32_t mips =
        std::min(kMaxBloomMips, static_cast<std::uint32_t>(std::bit_width(std::min(half.width, half.height))));
    m_Bloom = Image(ctx, {.extent    = {half.width, half.height, 1},
                          .format    = kHdrFormat,
                          .usage     = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          .mipLevels = mips,
                          .debugName = "Bloom"});
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        m_BloomViews.push_back(m_Bloom.CreateView(VK_IMAGE_VIEW_TYPE_2D, mip, 1, 0, 1, "BloomMip"));
        // The chain stays in GENERAL: sampled and storage access alternate between dispatches.
        m_BloomSampled.push_back(bindless.AddSampledImage(m_BloomViews.back().Handle(), VK_IMAGE_LAYOUT_GENERAL));
        m_BloomStorage.push_back(bindless.AddStorageImage(m_BloomViews.back().Handle()));
    }
}

void SceneRenderer::EnsureShadowMap()
{
    const std::uint32_t resolution = std::max(shadows.resolution, 16u);
    if (m_ShadowMap && m_ShadowMap.Extent().width == resolution)
        return;
    ReleaseShadowMap();

    m_ShadowMap = Image(m_Renderer.GetContext(), {.extent      = {resolution, resolution, 1},
                                                  .format      = kShadowFormat,
                                                  .usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                                                  .arrayLayers = kMaxCascades,
                                                  .viewType    = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                                  .debugName   = "ShadowCascades"});
    for (std::uint32_t c = 0; c < kMaxCascades; ++c) {
        m_ShadowViews.push_back(m_ShadowMap.CreateView(VK_IMAGE_VIEW_TYPE_2D, 0, 1, c, 1, "ShadowCascade"));
        m_ShadowSlots[c] = m_Renderer.GetBindless().AddSampledImage(m_ShadowViews.back().Handle());
    }
}

bool SceneRenderer::EnsureShadowAtlas()
{
    const std::uint32_t maxSize = m_Renderer.GetContext().Properties().limits.maxImageDimension2D;
    const std::uint32_t size    = std::bit_floor(std::clamp(localShadows.atlasSize, 256u, maxSize));
    if (m_ShadowAtlas && m_ShadowAtlas.Extent().width == size)
        return false;
    ReleaseShadowAtlas();
    m_ShadowAtlas        = Image(m_Renderer.GetContext(), {.extent    = {size, size, 1},
                                                           .format    = kShadowFormat,
                                                           .usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                                        VK_IMAGE_USAGE_SAMPLED_BIT,
                                                           .debugName = "LocalShadowAtlas"});
    m_ShadowAtlasSlot    = m_Renderer.GetBindless().AddSampledImage(m_ShadowAtlas.View());
    m_ShadowAtlasWritten = false;
    return true;
}

void SceneRenderer::ReleaseShadowAtlas()
{
    if (!m_ShadowAtlas)
        return;
    Renderer* r = &m_Renderer;
    r->DeferCall([r, slot = m_ShadowAtlasSlot] { r->GetBindless().RemoveSampledImage(slot); });
    r->DeferRelease(std::move(m_ShadowAtlas));
    m_ShadowAtlas        = {};
    m_ShadowAtlasWritten = false;
    m_ShadowCache.clear();
    m_TileAllocator = {};
}

bool SceneRenderer::GpuPath() const
{
    return culling.gpuDriven;
}

void SceneRenderer::GatherDraws(const Frustum& frustum, const glm::vec4& sphere, DrawList& out, Gather mode)
{
    out.draws.clear();
    out.address       = 0;
    const bool  range = sphere.w > 0.0f;
    const auto  near  = [&](const Aabb& box) {
        const glm::vec3 closest = glm::clamp(glm::vec3(sphere), box.min, box.max);
        return glm::dot(closest - glm::vec3(sphere), closest - glm::vec3(sphere)) <= sphere.w * sphere.w;
    };
    std::vector<std::pair<float, std::uint32_t>> sorted; // transparent: (distance, entry)
    const auto gather = [&](const SpatialIndex::MeshProxy& proxy, bool animated) {
        if (!animated && range && !near(proxy.bounds))
            return;
        const GpuInstance* inst = m_GpuScene->FindInstance(proxy.entity);
        if (!inst)
            return; // model released meanwhile
        if (((inst->flags & kInstanceSkinned) != 0) != animated)
            return;
        // Whole meshes were culled by the BVH; multi-part meshes (and light ranges) also per submesh.
        const bool perSubmesh = inst->drawCount > 1 || range;
        for (std::uint32_t d = inst->firstDraw; d < inst->firstDraw + inst->drawCount; ++d) {
            const GpuSubmesh& sm    = m_GpuScene->DrawSubmesh(d);
            const bool        blend = (sm.flags & kMaterialAlphaBlend) != 0;
            if ((mode == Gather::Camera && blend) || (mode == Gather::Transparent && !blend))
                continue;
            const bool skinBoundsValid = (inst->flags & kInstanceSkinBoundsValid) != 0;
            const Aabb localBounds = animated && skinBoundsValid
                                         ? Aabb{glm::vec3(inst->skinnedBoundsMin), glm::vec3(inst->skinnedBoundsMax)}
                                         : Aabb{sm.boundsMin, sm.boundsMax};
            const Aabb box = TransformAabb(localBounds, inst->model);
            if (((!animated && perSubmesh) || (animated && skinBoundsValid)) &&
                (!frustum.Intersects(box) || (range && !near(box))))
                continue;
            const LodChoice lod   = animated ? LodChoice{} : SelectLod(sm, inst->model, m_LodCamera, m_LodForced);
            const std::uint32_t e = d | (lod.lod << kVisibleLodShift);
            if (mode == Gather::Transparent) {
                sorted.emplace_back(glm::distance(glm::vec3(m_LodCamera), (box.min + box.max) * 0.5f), e); // xyz: camera
            } else if (mode == Gather::Camera && lod.fade > 0) { // cross-fade: both levels, complementary dither
                out.draws.push_back(e | (lod.fade << kVisibleFadeShift));
                out.draws.push_back(d | ((lod.lod + 1) << kVisibleLodShift) | (lod.fade << kVisibleFadeShift) | kVisibleFadeIn);
            } else {
                out.draws.push_back(e);
            }
        }
    };
    m_Spatial.QueryMeshes(frustum, [&](const SpatialIndex::MeshProxy& proxy) { gather(proxy, false); });
    // Animated bounds can exceed the imported bind-pose bounds, so keep skinned instances out of
    // the static BVH/frustum/range tests until conservative pose bounds are available.
    for (const SpatialIndex::MeshProxy& proxy : m_Spatial.Meshes())
        if (const GpuInstance* instance = m_GpuScene->FindInstance(proxy.entity);
            instance && (instance->flags & kInstanceSkinned) != 0)
            gather(proxy, true);
    if (mode == Gather::Transparent) { // far to near: blending needs back to front
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [distance, entry] : sorted)
            out.draws.push_back(entry);
    }
    if (!out.draws.empty()) { // visible list of the pass: firstInstance = position in it
        const TransientAllocation a = m_Renderer.AllocateTransient(out.draws.size() * sizeof(std::uint32_t), 16);
        std::memcpy(a.cpu, out.draws.data(), out.draws.size() * sizeof(std::uint32_t));
        out.address = a.gpu;
    }
    m_Stats.drawItems += static_cast<std::uint32_t>(out.draws.size());
}

void SceneRenderer::DrawTransparent(VkCommandBuffer cmd, const DrawList& list, VkDeviceAddress frameAddress,
                                    std::uint32_t flags)
{
    if (list.draws.empty())
        return;
    const VkPipelineLayout layout = m_Renderer.GetBindless().PipelineLayout();
    const MeshPush push{.frame = frameAddress, .visible = list.address, .cascade = 0, .flags = flags};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_MeshBlend.Handle());
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdBindIndexBuffer(cmd, m_Renderer.Geometry().IndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
    for (std::uint32_t i = 0; i < list.draws.size(); ++i) {
        const std::uint32_t d        = list.draws[i] & kVisibleRecordMask;
        const auto          lod      = static_cast<glm::length_t>((list.draws[i] >> kVisibleLodShift) & 3u);
        const GpuSubmesh&   sm       = m_GpuScene->DrawSubmesh(d);
        const bool          mirrored = (m_GpuScene->InstanceData(m_GpuScene->Draw(d).instance).flags & kInstanceMirrored) != 0;
        vkCmdSetCullMode(cmd, (sm.flags & kMaterialDoubleSided) != 0 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
        vkCmdSetFrontFace(cmd, mirrored ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkCmdDrawIndexed(cmd, sm.lodIndexCount[lod], 1, sm.lodFirstIndex[lod], sm.vertexOffset, i);
        ++m_Stats.drawCalls;
        ++m_Stats.transparentDraws;
        m_Stats.triangles += sm.lodIndexCount[lod] / 3;
    }
}

void SceneRenderer::DrawCpuCamera(VkCommandBuffer cmd, const DrawList& list, VkDeviceAddress frameAddress, bool countStats,
                                  std::uint32_t flags)
{
    if (list.draws.empty())
        return;
    const VkPipelineLayout layout = m_Renderer.GetBindless().PipelineLayout();
    const MeshPush push{.frame = frameAddress, .visible = list.address, .cascade = 0, .flags = flags};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdBindIndexBuffer(cmd, m_Renderer.Geometry().IndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
    std::uint32_t bucket = ~0u;
    for (std::uint32_t i = 0; i < list.draws.size(); ++i) {
        const std::uint32_t d   = list.draws[i] & kVisibleRecordMask;
        const auto          lod = static_cast<glm::length_t>((list.draws[i] >> kVisibleLodShift) & 3u);
        const GpuSubmesh&   sm  = m_GpuScene->DrawSubmesh(d);
        const std::uint32_t b   = m_GpuScene->Batch(m_GpuScene->Draw(d).batch).cameraBucket;
        if (b != bucket) {
            vkCmdSetCullMode(cmd, (b & 1u) != 0 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            vkCmdSetFrontFace(cmd, (b & 2u) != 0 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
            bucket = b;
        }
        vkCmdDrawIndexed(cmd, sm.lodIndexCount[lod], 1, sm.lodFirstIndex[lod], sm.vertexOffset, i);
        if (countStats) {
            ++m_Stats.drawCalls;
            m_Stats.triangles += sm.lodIndexCount[lod] / 3;
            m_Stats.lodDraws += list.draws[i] >> kVisibleLodShift != 0 ? 1u : 0u; // LOD > 0 or fading
        }
    }
}

void SceneRenderer::DrawCpuShadow(VkCommandBuffer cmd, const DrawList& list, VkDeviceAddress frameAddress,
                                  std::uint32_t cascade, const Pipeline& plain, const Pipeline& masked,
                                  std::uint32_t& drawCounter)
{
    if (list.draws.empty())
        return;
    const MeshPush push{.frame = frameAddress, .visible = list.address, .cascade = cascade, .flags = 0};
    vkCmdPushConstants(cmd, m_Renderer.GetBindless().PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdBindIndexBuffer(cmd, m_Renderer.Geometry().IndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
    // Plain casters first, then alpha-tested ones (like the GPU path's buckets): two binds at most.
    for (const bool alphaTested : {false, true}) {
        bool bound = false;
        for (std::uint32_t i = 0; i < list.draws.size(); ++i) {
            const std::uint32_t d   = list.draws[i] & kVisibleRecordMask;
            const auto          lod = static_cast<glm::length_t>((list.draws[i] >> kVisibleLodShift) & 3u);
            const GpuSubmesh&   sm  = m_GpuScene->DrawSubmesh(d);
            if (((sm.flags & (kMaterialAlphaMask | kMaterialAlphaBlend)) != 0) != alphaTested)
                continue;
            if (!std::exchange(bound, true)) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, alphaTested ? masked.Handle() : plain.Handle());
                // Again after the bind: lavapipe drops fragment-stage push constants set while a
                // pipeline without a fragment shader was bound (valid per spec, crashes there).
                vkCmdPushConstants(cmd, m_Renderer.GetBindless().PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            }
            vkCmdDrawIndexed(cmd, sm.lodIndexCount[lod], 1, sm.lodFirstIndex[lod], sm.vertexOffset, i);
            ++drawCounter;
        }
    }
}

void SceneRenderer::DrawGpuCamera(VkCommandBuffer cmd, std::uint32_t view, VkDeviceAddress frameAddress,
                                  std::uint32_t flags)
{
    const MeshPush push{.frame = frameAddress, .visible = m_GpuCulling->VisibleAddress(), .cascade = 0, .flags = flags};
    vkCmdPushConstants(cmd, m_Renderer.GetBindless().PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdBindIndexBuffer(cmd, m_Renderer.Geometry().IndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
    m_GpuCulling->Draw(cmd, view, kCameraBuckets, [&](std::uint32_t bucket) {
        vkCmdSetCullMode(cmd, (bucket & 1u) != 0 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
        vkCmdSetFrontFace(cmd, (bucket & 2u) != 0 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    });
}

void SceneRenderer::DrawGpuShadow(VkCommandBuffer cmd, std::uint32_t view, VkDeviceAddress frameAddress,
                                  std::uint32_t cascade, const Pipeline& plain, const Pipeline& masked)
{
    const MeshPush push{.frame = frameAddress, .visible = m_GpuCulling->VisibleAddress(), .cascade = cascade, .flags = 0};
    vkCmdPushConstants(cmd, m_Renderer.GetBindless().PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdBindIndexBuffer(cmd, m_Renderer.Geometry().IndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
    m_GpuCulling->Draw(cmd, view, kShadowBuckets, [&](std::uint32_t bucket) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bucket != 0 ? masked.Handle() : plain.Handle());
        // See DrawCpuShadow: re-push after switching to / from the depth-only pipeline.
        vkCmdPushConstants(cmd, m_Renderer.GetBindless().PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    });
}

void SceneRenderer::CullGpu(VkCommandBuffer cmd, const CameraData& camera,
                            const std::array<Cascade, kMaxCascades>& cascades, std::uint32_t cascadeCount,
                            VkExtent2D extent)
{
    const auto makeView = [](const glm::mat4& viewProj, bool clipNear, std::uint32_t flags) {
        GpuCullView view;
        view.viewProj = viewProj;
        view.planes   = Frustum::FromViewProjection(viewProj, clipNear).Planes();
        view.flags    = flags;
        return view;
    };
    // Frozen culling keeps the camera of the moment it was switched on (the Hi-Z is kept too).
    const glm::mat4 current = camera.projection * camera.view;
    if (culling.freeze && !m_FrozenViewProj)
        m_FrozenViewProj = current;
    else if (!culling.freeze)
        m_FrozenViewProj.reset();
    const glm::mat4 cameraViewProj = m_FrozenViewProj.value_or(current);

    std::vector<GpuCullView> views;
    const std::uint32_t occlusion = culling.occlusion ? kCullViewOcclusion : 0u;
    views.push_back(makeView(cameraViewProj, true, kCullViewCameraEarly | occlusion));
    views.push_back(makeView(cameraViewProj, true, kCullViewCameraLate | kCullViewOcclusion));
    // Casters towards the light stay (depth clamp). With occlusion culling, cascades are culled after
    // the Hi-Z against it (m_ShadowOcclusion: rendered after the prepass).
    for (std::uint32_t c = 0; c < cascadeCount; ++c) {
        GpuCullView view = makeView(cascades[c].viewProj, false, kCullViewShadow | (m_ShadowOcclusion ? kCullViewShadowOcclusion : 0u));
        view.sphere      = glm::vec4(glm::normalize(lighting.sky.sunDirection), 2.0f * shadows.maxDistance);
        views.push_back(view);
    }
    for (ShadowTile& tile : m_ShadowTiles) {
        if (!tile.render)
            continue;
        tile.gpuView     = static_cast<std::uint32_t>(views.size());
        GpuCullView view = makeView(tile.viewProj, true, kCullViewShadow | kCullViewSphere);
        view.sphere      = glm::vec4(tile.lightPosition, tile.lightRange);
        views.push_back(view);
    }
    // World box of the camera frustum between the near plane and the shadow distance.
    const glm::mat4 toWorld = glm::inverse(camera.view);
    const float     tanY    = 1.0f / std::abs(camera.projection[1][1]);
    const float     tanX    = 1.0f / std::abs(camera.projection[0][0]);
    glm::vec3       regionMin(std::numeric_limits<float>::max()), regionMax(std::numeric_limits<float>::lowest());
    for (const float d : {camera.nearPlane, std::max(shadows.maxDistance, camera.nearPlane)})
        for (int corner = 0; corner < 4; ++corner) {
            const glm::vec3 p = glm::vec3(toWorld * glm::vec4((corner & 1 ? 1.0f : -1.0f) * d * tanX,
                                                              (corner & 2 ? 1.0f : -1.0f) * d * tanY, -d, 1.0f));
            regionMin = glm::min(regionMin, p);
            regionMax = glm::max(regionMax, p);
        }
    m_GpuCulling->CullEarly(cmd, *m_GpuScene, views, extent,
                            {.camera          = m_LodCamera,
                             .forced          = m_LodForced,
                             .shadowRegionMin = regionMin,
                             .shadowRegionMax = regionMax});
}

const Pipeline& SceneRenderer::TonemapPipeline(VkFormat outputFormat)
{
    for (const auto& [format, pipeline] : m_Tonemap)
        if (format == outputFormat)
            return pipeline;
    m_Tonemap.emplace_back(outputFormat,
                           GraphicsPipelineBuilder{}
                               .SetShaders(ShaderPath("fullscreen.vert.spv"), ShaderPath("tonemap.frag.spv"))
                               .AddColorAttachment(outputFormat)
                               .SetDebugName("Tonemap")
                               .Build(m_Renderer.GetContext().Device(), m_Renderer.GetBindless().PipelineLayout()));
    return m_Tonemap.back().second;
}

void SceneRenderer::Render(const FrameContext& frame, Scene& scene, const CameraData& camera)
{
    const RenderOutput swapchain{.image = frame.image, .view = frame.view, .format = frame.format, .extent = frame.extent};
    Render(frame, scene, camera, swapchain);
}

void SceneRenderer::Render(const FrameContext& frame, Scene& scene, const CameraData& camera,
                           const RenderOutput& output)
{
    m_Stats = {};
    const VkCommandBuffer cmd = frame.cmd;
    if (m_Renderer.ShaderGeneration() != m_ShaderGeneration) { // shader hot reload
        m_ShaderGeneration = m_Renderer.ShaderGeneration();
        RebuildPipelines();
    }
    const SkySettings&    sky = lighting.sky;

    const auto  now       = std::chrono::steady_clock::now();
    const float deltaTime = m_LastFrameTime ? std::chrono::duration<float>(now - *m_LastFrameTime).count() : 0.0f;
    m_LastFrameTime       = now;
    ReadExposure(frame.frameIndex); // this slot's fence was waited on in BeginFrame
    ReadPick(scene, frame.frameIndex);
    m_GpuCulling->ReadStats(frame.frameIndex);

    const VkExtent2D extent = output.extent;
    EnsureTargets(extent);
    EnsurePickingTarget(extent);
    EnsureShadowMap();
    GpuProfiler* profiler = &m_Renderer.Profiler();
    {
        GpuScope scope(profiler, cmd, "Environment");
        m_Environment.Update(cmd, sky); // compute, only when the sky changed
    }
    // Scene changes -> BVH -> GPU scene (instances, draw records, batches: uploaded incrementally).
    m_Spatial.Sync(scene, m_Assets);
    m_Stats.cpuSpatialMs = m_Spatial.LastSync().milliseconds;
    const auto sceneStart = std::chrono::steady_clock::now();
    m_GpuScene->Update(scene, m_Spatial, m_Assets);
    m_GpuScene->Upload(cmd);
    m_Stats.cpuGpuSceneMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sceneStart).count();
    m_GpuFrame = GpuPath();

    const std::uint32_t cascadeCount = shadows.enabled ? std::clamp(shadows.cascadeCount, 1u, kMaxCascades) : 0u;
    const auto          cascades     = ComputeCascades(camera, sky.sunDirection, shadows);

    // TAA: sub-pixel jitter of the projection (clip-space translation, works for any projection).
    const glm::mat4 unjitteredViewProj = camera.projection * camera.view;
    glm::vec2       jitter{0.0f};
    if (taa.enabled) {
        jitter = HaltonJitter(m_FrameCounter) * 2.0f / glm::vec2(static_cast<float>(extent.width),
                                                                  static_cast<float>(extent.height));
    } else {
        m_HistoryValid = false; // re-enabling starts from the current frame
    }
    glm::mat4 jitterMatrix(1.0f);
    jitterMatrix[3][0] = jitter.x;
    jitterMatrix[3][1] = jitter.y;
    const glm::mat4 viewProj     = jitterMatrix * unjitteredViewProj;
    const glm::mat4 prevViewProj = m_PrevViewProj.value_or(unjitteredViewProj);
    m_PrevViewProj               = unjitteredViewProj;
    const Frustum   frustum  = Frustum::FromViewProjection(unjitteredViewProj);
    const auto      cullStart = std::chrono::steady_clock::now();
    // LOD: projected error in pixels = error / distance * pixelsPerUnit (vertical, at distance 1).
    const float pixelsPerUnit = 0.5f * static_cast<float>(extent.height) * std::abs(camera.projection[1][1]);
    m_LodCamera = glm::vec4(camera.position,
                            culling.lod && culling.lodPixelError > 0.0f ? pixelsPerUnit / culling.lodPixelError : 0.0f);
    m_LodForced = culling.forceLod >= 0 ? static_cast<std::uint32_t>(culling.forceLod) + 1u : 0u;
    if (!m_GpuFrame) {
        GatherDraws(frustum, glm::vec4(0.0f), m_CameraDraws, Gather::Camera);
        m_Stats.culled = static_cast<std::uint32_t>(m_Spatial.SubmeshCount() -
                                                    std::min<std::uint64_t>(m_CameraDraws.draws.size(), m_Spatial.SubmeshCount()));
    }
    m_TransparentDraws.draws.clear();
    if (m_GpuScene->BlendDraws() > 0) // both paths: blending needs a CPU sort
        GatherDraws(frustum, glm::vec4(0.0f), m_TransparentDraws, Gather::Transparent);
    CollectLights(scene, frustum);
    CollectDecals(scene, frustum);
    m_FogThisFrame = fog.enabled && fog.volumetric;
    if (m_FogThisFrame) {
        if (fog != m_FogHistorySettings) { // no lag after changes (editor sliders, scripts)
            m_FogHistorySettings = fog;
            m_FogHistoryValid    = false;
        }
        EnsureFogVolumes(extent);
        CollectFogVolumes(scene, frustum, camera.position);
    } else {
        ReleaseFogVolumes();
        m_FogVolumes.clear();
    }
    {
        static const ParticleRenderData kNoParticles;
        m_Particles->Prepare(particles ? particles->RenderData() : kNoParticles, frustum, camera.position, frame.frameIndex);
    }
    {
        std::vector<Frustum> cascadeFrusta;
        for (std::uint32_t c = 0; c < cascadeCount; ++c)
            cascadeFrusta.push_back(Frustum::FromViewProjection(cascades[c].viewProj, false));
        m_Terrain->Prepare(cmd, scene, camera.position, frustum, cascadeFrusta, cascadeCount > 0);
        const TerrainRenderStats& terrainStats = m_Terrain->Stats();
        m_Stats.terrains         = terrainStats.terrains;
        m_Stats.terrainNodes     = terrainStats.nodes;
        m_Stats.terrainTriangles = terrainStats.triangles;
    }
    m_Stats.cpuCullingMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cullStart).count();
    AssignLocalShadows(camera); // sets GpuLight::shadow
    const auto pushArray = [&]<class T>(const std::vector<T>& values) -> VkDeviceAddress {
        if (values.empty())
            return 0;
        const TransientAllocation a = m_Renderer.AllocateTransient(values.size() * sizeof(T), 16);
        std::memcpy(a.cpu, values.data(), values.size() * sizeof(T));
        return a.gpu;
    };
    const VkDeviceAddress lightAddress      = pushArray(m_Lights);
    const VkDeviceAddress decalAddress      = pushArray(m_Decals);
    // Never null: llvmpipe speculatively loads from the list even when the loop runs zero times.
    const VkDeviceAddress fogVolumeAddress  = m_FogVolumes.empty() ? pushArray(std::vector<GpuFogVolume>(1))
                                                                   : pushArray(m_FogVolumes);
    const float           fogDistance       = std::max(fog.volumetricDistance, 1.0f);
    const VkExtent3D      fogGrid           = m_FogThisFrame ? m_FogIntegrated.Extent() : VkExtent3D{1, 1, 1};
    const VkDeviceAddress shadowViewAddress = pushArray(m_ShadowViewData);
    // Logarithmic slices between the near plane and clusterFar: slice = log(z) * scale + bias.
    const float clusterNear  = camera.nearPlane;
    const float clusterFar   = std::max(lights.clusterFar, clusterNear * 2.0f);
    const float sliceScale   = static_cast<float>(kClusterGridZ) / std::log(clusterFar / clusterNear);
    const float sliceBias    = -std::log(clusterNear) * sliceScale;
    // Reflections need last frame's image: the TAA history, else the HDR target (still unchanged).
    m_SsrThisFrame = ssr.enabled && ssr.intensity > 0.0f && (taa.enabled ? m_HistoryValid : m_PrevHdrValid);

    FrameUniforms   uniforms{
          .viewProj       = viewProj,
          .view           = camera.view,
          .proj           = jitterMatrix * camera.projection,
          .invViewProj    = glm::inverse(viewProj),
          .prevViewProj   = prevViewProj,
          .unjitteredViewProj = unjitteredViewProj,
          .cameraPosition = glm::vec4(camera.position, 1.0f),
          .sunDirection   = glm::vec4(glm::normalize(sky.sunDirection), 0.0f),
          .sunRadiance    = glm::vec4(sky.sunColor * sky.sunIntensity, 0.0f),
          .sky            = glm::vec4(sky.skyIntensity, lighting.iblIntensity, 0.0f, 0.0f),
          .ibl            = glm::uvec4(m_Environment.IrradianceCube(), m_Environment.PrefilteredCube(),
                                       m_Environment.BrdfLut(), m_Environment.PrefilteredMipCount()),
          .cascadeViewProj = {},
          .cascadeSplits   = glm::vec4(0.0f),
          .cascadeTexel    = glm::vec4(0.0f),
          .shadowMaps      = glm::uvec4(m_ShadowSlots[0], m_ShadowSlots[1], m_ShadowSlots[2], m_ShadowSlots[3]),
          .shadowParams    = glm::vec4(static_cast<float>(cascadeCount), shadows.normalBias, shadows.filterRadius,
                                       shadows.cascadeBlend),
          .shadowInfo      = glm::uvec4(m_ShadowMap.Extent().width, shadows.debugCascades ? 1u : 0u, 0u, 0u),
          .aoInfo          = glm::uvec4(m_AoSampled, ao.enabled ? 1u : 0u, 0u, 0u),
          .clusterParams   = glm::vec4(sliceScale, sliceBias, static_cast<float>(kClusterGridX) / static_cast<float>(extent.width),
                                       static_cast<float>(kClusterGridY) / static_cast<float>(extent.height)),
          .clusterDepth    = glm::vec4(clusterNear, clusterFar, 0.0f, 0.0f),
          .lightInfo       = glm::uvec4(static_cast<std::uint32_t>(m_Lights.size()), 0u, 0u, 0u),
          .localShadowInfo = glm::uvec4(m_ShadowAtlasSlot, m_ShadowAtlas ? m_ShadowAtlas.Extent().width : 0u, 0u, 0u),
          .localShadowParams = glm::vec4(localShadows.normalBias, localShadows.filterRadius, 0.0f, 0.0f),
          .hizInfo         = glm::uvec4(m_GpuCulling->HiZSize(), m_GpuCulling->HiZLevels(), culling.hizDebugLevel),
          .taaInfo         = glm::vec4(jitter, deltaTime, static_cast<float>(m_FrameCounter % 1024)),
          .decalInfo       = glm::uvec4(static_cast<std::uint32_t>(m_Decals.size()), 0u, 0u, 0u),
          .ssrInfo         = glm::uvec4(m_SsrRawSampled, m_SsrThisFrame ? 1u : 0u, 0u, 0u),
          .ssrParams       = glm::vec4(std::max(ssr.intensity, 0.0f), 0.0f, 1.0f / static_cast<float>(extent.width),
                                       1.0f / static_cast<float>(extent.height)),
          .fogColor        = glm::vec4(glm::clamp(fog.albedo, glm::vec3(0.0f), glm::vec3(1.0f)), std::max(fog.density, 0.0f)),
          .fogHeight       = glm::vec4(fog.baseHeight, std::max(fog.heightFalloff, 0.0f), std::max(fog.startDistance, 0.0f),
                                       std::clamp(fog.maxOpacity, 0.0f, 1.0f)),
          .fogLight        = glm::vec4(std::clamp(fog.anisotropy, -0.95f, 0.95f), std::max(fog.sunScale, 0.0f),
                                       std::max(fog.ambientScale, 0.0f), std::max(fog.lightScale, 0.0f)),
          .fogInfo         = glm::uvec4(fog.enabled ? 1u : 0u, m_FogIntegratedSampled,
                                        static_cast<std::uint32_t>(m_FogVolumes.size()), m_FogThisFrame ? 1u : 0u),
          .fogGrid         = glm::vec4(fogDistance, static_cast<float>(fogGrid.depth),
                                       static_cast<float>(extent.width) / static_cast<float>(fogGrid.width * kFogTileSize),
                                       static_cast<float>(extent.height) / static_cast<float>(fogGrid.height * kFogTileSize)),
          .lights          = lightAddress,
          .clusters        = m_Clusters.Address(),
          .shadowViews     = shadowViewAddress,
          .vertices        = m_Renderer.Geometry().Address(GeometryKind::Vertices),
          .materials       = m_Renderer.Geometry().Address(GeometryKind::Materials),
          .submeshes       = m_Renderer.Geometry().Address(GeometryKind::Submeshes),
          .instances       = m_GpuScene->InstanceAddress(),
          .jointMatrices   = m_GpuScene->JointMatrixAddress(),
          .draws           = m_GpuScene->DrawAddress(),
          .hiz             = m_GpuCulling->HiZAddress(),
          .textureTable    = m_Renderer.TextureTableAddress(),
          .prevJointMatrices = m_GpuScene->PrevJointMatrixAddress(),
          .decals          = decalAddress,
          .decalClusters   = m_DecalClusters.Address(),
          .fogVolumes      = fogVolumeAddress};
    ++m_FrameCounter;
    for (std::uint32_t c = 0; c < kMaxCascades; ++c) {
        uniforms.cascadeViewProj[c] = cascades[c].viewProj;
        uniforms.cascadeSplits[c]   = cascades[c].splitFar;
        uniforms.cascadeTexel[c]    = cascades[c].texelWorldSize;
    }
    const VkDeviceAddress frameAddress = m_Renderer.PushTransient(uniforms);

    // Cascade casters can be culled against the camera Hi-Z (GPU path with occlusion): then the
    // cascades are rendered after the prepass, which builds it.
    m_ShadowOcclusion = m_GpuFrame && culling.occlusion && culling.shadowOcclusion && !culling.freeze;
    if (m_GpuFrame) {
        GpuScope scope(profiler, cmd, "GPU culling");
        CullGpu(cmd, camera, cascades, cascadeCount, extent);
    }
    if (cascadeCount > 0 && !m_ShadowOcclusion) {
        GpuScope scope(profiler, cmd, "Shadows");
        RenderShadows(cmd, frameAddress, cascades, cascadeCount);
    }
    if (std::ranges::any_of(m_ShadowTiles, &ShadowTile::render)) {
        GpuScope scope(profiler, cmd, "Local shadows");
        RenderLocalShadows(cmd, frameAddress);
    }

    {
        GpuScope scope(profiler, cmd, "Depth + normals");
        RenderPrepass(cmd, extent, frameAddress, frame.frameIndex);
    }
    if (cascadeCount > 0 && m_ShadowOcclusion) {
        GpuScope scope(profiler, cmd, "Shadows");
        RenderShadows(cmd, frameAddress, cascades, cascadeCount);
    }
    if (ao.enabled) {
        GpuScope scope(profiler, cmd, "GTAO");
        RenderAmbientOcclusion(cmd, extent, camera);
    }
    if (!m_Lights.empty() || !m_Decals.empty()) {
        GpuScope scope(profiler, cmd, "Light + decal culling");
        CullLights(cmd, frameAddress);
    }
    if (m_SsrThisFrame) {
        GpuScope scope(profiler, cmd, "Reflections");
        RenderSsr(cmd, extent, frameAddress);
    } else {
        m_SsrHistoryValid = false;
    }
    if (m_FogThisFrame) {
        GpuScope scope(profiler, cmd, "Volumetric fog");
        RenderFog(cmd, frameAddress);
    }
    {
        GpuScope scope(profiler, cmd, "Lighting + sky");
        RenderMain(cmd, extent, frameAddress, frame.frameIndex);
    }
    m_PrevHdrValid = true;

    // HDR -> sampled by bloom (compute) and tone mapping (fragment).
    CmdImageBarrier(cmd, {.image     = m_Hdr.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
    m_ResolvedSlot = m_HdrSlot;
    if (taa.enabled) {
        GpuScope scope(profiler, cmd, "TAA");
        RenderTaa(cmd, extent, frameAddress);
    }
    const bool bloom = post.bloom && post.bloomStrength > 0.0f;
    if (bloom) {
        GpuScope scope(profiler, cmd, "Bloom");
        RenderBloom(cmd);
    }
    if (post.autoExposure) {
        GpuScope scope(profiler, cmd, "Auto exposure");
        RenderExposure(cmd, frame.frameIndex, deltaTime);
    } else
        m_Stats.exposure = post.exposure;

    // Stats: GPU counters arrive kFramesInFlight frames late (read at the start of Render).
    m_Stats.gpuDriven = m_GpuFrame;
    m_Stats.instances = m_GpuScene->InstanceCount();
    m_Stats.draws     = m_GpuScene->LiveDraws();
    m_Stats.batches   = m_GpuScene->LiveBatches();
    const PoolUsage vertexUse = m_Renderer.Geometry().Usage(GeometryKind::Vertices);
    const PoolUsage indexUse  = m_Renderer.Geometry().Usage(GeometryKind::Indices);
    m_Stats.geometryVertices  = vertexUse.used;
    m_Stats.geometryVertexCapacity = vertexUse.capacity;
    m_Stats.geometryIndices   = indexUse.used;
    m_Stats.geometryIndexCapacity  = indexUse.capacity;
    if (m_GpuFrame) {
        const GpuCullStats& gpu  = m_GpuCulling->Stats();
        m_GpuCulling->CopyStats(cmd, frame.frameIndex);
        m_Stats.drawCalls        = m_GpuCulling->IndirectCalls();
        m_Stats.culled           = gpu.frustum + gpu.occluded;
        m_Stats.triangles        = gpu.triangles;
        m_Stats.shadowDraws      = gpu.shadow;
        m_Stats.gpuTested        = gpu.tested;
        m_Stats.gpuFrustumCulled = gpu.frustum;
        m_Stats.gpuOccluded      = gpu.occluded;
        m_Stats.gpuEarly         = gpu.early;
        m_Stats.gpuLate          = gpu.late;
        m_Stats.gpuCommands      = gpu.commands;
        m_Stats.lodDraws         = gpu.lodDraws;
        m_Stats.shadowOccluded   = gpu.shadowOccluded;
    }

    // --- Tone mapping into the swapchain image ---
    const auto& bindless  = m_Renderer.GetBindless();
    GpuScope    tonemapScope(profiler, cmd, "Tone mapping");
    const auto  target = Attachment(output.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    BeginRendering(cmd, extent, &target, nullptr);
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    SetViewportScissor(cmd, extent);
    const auto [outlineAddress, outlineWords] = PushOutlineBits();
    const TonemapPush tonemap{.state         = m_ExposureState.Address(),
                              .hdrTexture    = m_ResolvedSlot,
                              .tonemapper    = static_cast<std::uint32_t>(post.tonemapper),
                              .exposure      = post.exposure,
                              .bloomTexture  = m_BloomSampled.front(),
                              .bloomStrength = bloom ? post.bloomStrength : 0.0f,
                              .autoExposure  = post.autoExposure ? 1u : 0u,
                              .debugView     = static_cast<std::uint32_t>(post.debugView),
                              .debugTexture  = DebugTexture(),
                              .frame         = frameAddress,
                              .idTexture     = m_EntityIdSlot,
                              .outlineWords  = outlineWords,
                              .outline       = outlineAddress,
                              .outlineColor  = overlay.outlineColor,
                              .sharpness     = taa.enabled ? std::max(taa.sharpness, 0.0f) : 0.0f};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, TonemapPipeline(output.format).Handle());
    vkCmdPushConstants(cmd, bindless.PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(tonemap), &tonemap);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

std::uint32_t SceneRenderer::DebugTexture() const
{
    switch (post.debugView) {
    case DebugView::Normals:       return m_NormalSlot;
    case DebugView::LightClusters: return m_DepthSlot;
    case DebugView::Velocity:      return m_VelocitySlot;
    case DebugView::Reflections:   return m_SsrRawSampled;
    case DebugView::ShadowAtlas:
        return m_ShadowAtlasWritten ? m_ShadowAtlasSlot : m_Renderer.DefaultTextureIndex(DefaultTexture::Black);
    default:                       return m_AoSampled;
    }
}

void SceneRenderer::AssignLocalShadows(const CameraData& camera)
{
    m_ShadowTiles.clear();
    m_ShadowViewData.clear();
    if (!localShadows.enabled) {
        ReleaseShadowAtlas(); // 64 MB at 4096^2
        return;
    }
    if (localShadows.maxLights == 0 || m_Lights.empty()) {
        // Nothing to shadow: cached maps would miss this frame's scene changes, drop them.
        for (const ShadowCacheEntry& e : m_ShadowCache)
            for (std::uint32_t v = 0; v < e.views; ++v)
                m_TileAllocator.Free(e.offsets[v], e.size);
        m_ShadowCache.clear();
        return;
    }
    const bool          recreated = EnsureShadowAtlas();
    const std::uint32_t atlasSize = m_ShadowAtlas.Extent().width;
    const std::uint32_t minTile   = std::clamp(std::bit_floor(std::max(localShadows.minTileSize, 16u)), 16u, atlasSize);
    const std::uint32_t maxTile   = std::clamp(std::bit_floor(std::max(localShadows.maxTileSize, minTile)), minTile, atlasSize);
    if (recreated || m_TileAllocator.AtlasSize() != atlasSize || m_TileAllocator.MinTile() != minTile) {
        m_ShadowCache.clear();
        m_TileAllocator.Reset(atlasSize, minTile);
    }

    // Invalidation: rasterizer bias is baked into the maps; casters that moved, appeared or
    // vanished near a light (the spatial index reports their old and new bounds).
    const glm::vec2 bias{localShadows.depthBias, localShadows.slopeBias};
    if (bias != m_CachedBias) {
        for (ShadowCacheEntry& entry : m_ShadowCache)
            entry.valid.fill(false);
        m_CachedBias = bias;
    }
    for (ShadowCacheEntry& entry : m_ShadowCache)
        for (const Aabb& region : m_Spatial.ChangedRegions()) {
            const glm::vec3 closest = glm::clamp(entry.position, region.min, region.max);
            if (glm::dot(closest - entry.position, closest - entry.position) <= entry.range * entry.range) {
                entry.valid.fill(false);
                break;
            }
        }

    // Importance: screen size of the light's range sphere (fraction of the viewport height).
    struct Want {
        std::uint32_t light;
        float         importance;
        std::uint32_t size  = 0;
        std::uint32_t views = 1;
    };
    std::vector<Want> wants;
    for (std::uint32_t i = 0; i < m_Lights.size(); ++i) {
        if (!m_LightCastsShadows[i])
            continue;
        const GpuLight& l = m_Lights[i];
        const float     d = glm::distance(l.position, camera.position);
        const float     importance =
            d <= l.range ? 1.0f
                         : std::min(1.0f, l.range / std::sqrt(d * d - l.range * l.range) * camera.projection[1][1] * 0.5f);
        wants.push_back({.light = i, .importance = importance});
    }
    std::ranges::stable_sort(wants, std::greater{}, &Want::importance);
    if (wants.size() > localShadows.maxLights)
        wants.resize(localShadows.maxLights);
    for (Want& w : wants) {
        const bool point = m_Lights[w.light].type == 0;
        w.views          = point ? kCubeFaces : 1u;
        w.size = std::clamp(std::bit_floor(std::max(static_cast<std::uint32_t>(w.importance * static_cast<float>(maxTile)), 1u)),
                            minTile, maxTile);
        if (point)
            w.size = std::max(w.size / 2, minTile);
    }

    // Keep the tiles of lights that still want the same size; free everything else.
    const auto freeEntry = [&](const ShadowCacheEntry& e) {
        for (std::uint32_t v = 0; v < e.views; ++v)
            m_TileAllocator.Free(e.offsets[v], e.size);
    };
    const auto wantOf = [&](Entity light) {
        return std::ranges::find_if(wants, [&](const Want& w) { return m_LightEntities[w.light] == light; });
    };
    std::erase_if(m_ShadowCache, [&](const ShadowCacheEntry& e) {
        const auto w    = wantOf(e.light);
        const bool keep = w != wants.end() && w->size == e.size && w->views == e.views;
        if (!keep)
            freeEntry(e);
        return !keep;
    });
    const auto entryOf = [&](Entity light) {
        return std::ranges::find_if(m_ShadowCache, [&](const ShadowCacheEntry& e) { return e.light == light; });
    };

    // New tiles in priority order: halve on failure; at the minimum size, evict the least important
    // light that already has tiles.
    for (std::size_t wi = 0; wi < wants.size(); ++wi) {
        Want&        w     = wants[wi];
        const Entity light = m_LightEntities[w.light];
        if (entryOf(light) != m_ShadowCache.end())
            continue;
        ShadowCacheEntry entry{.light = light, .views = w.views};
        for (std::uint32_t size = w.size; entry.size == 0;) {
            std::uint32_t allocated = 0;
            for (; allocated < w.views; ++allocated) {
                const auto offset = m_TileAllocator.Allocate(size);
                if (!offset)
                    break;
                entry.offsets[allocated] = *offset;
            }
            if (allocated == w.views) {
                entry.size = size;
                break;
            }
            for (std::uint32_t v = 0; v < allocated; ++v)
                m_TileAllocator.Free(entry.offsets[v], size);
            if (size > minTile) {
                size /= 2;
                continue;
            }
            // Evict the least important other light with tiles, then retry from the wanted size.
            bool evicted = false;
            for (std::size_t vi = wants.size(); vi-- > wi + 1;) {
                if (const auto victim = entryOf(m_LightEntities[wants[vi].light]); victim != m_ShadowCache.end()) {
                    freeEntry(*victim);
                    m_ShadowCache.erase(victim);
                    evicted = true;
                    break;
                }
            }
            if (!evicted)
                break;
            size = w.size;
        }
        if (entry.size != 0) {
            w.size = entry.size;
            m_ShadowCache.push_back(entry);
        }
    }

    // Views of every light with tiles; unchanged matrices keep the cached map.
    const float border = std::ceil(localShadows.filterRadius) + 2.0f; // PCF taps stay in the tile
    for (const Want& w : wants) {
        const auto it = entryOf(m_LightEntities[w.light]);
        if (it == m_ShadowCache.end())
            continue;
        ShadowCacheEntry& entry = *it;
        GpuLight&         l     = m_Lights[w.light];
        entry.position          = l.position;
        entry.range             = l.range;
        l.shadow                = static_cast<std::uint32_t>(m_ShadowTiles.size());

        // Reverse-Z perspective with an infinite far plane; the range bounds the casters instead.
        const float nearPlane = std::max(l.range * 0.005f, 1e-4f);
        const auto  addView   = [&](std::uint32_t face, const glm::mat4& view, float tanHalf) {
            const glm::mat4 viewProj = PerspectiveReverseZ(2.0f * std::atan(tanHalf), 1.0f, nearPlane) * view;
            if (entry.viewProj[face] != viewProj) {
                entry.viewProj[face] = viewProj;
                entry.valid[face]    = false;
            }
            m_ShadowTiles.push_back({.viewProj      = viewProj,
                                     .offset        = entry.offsets[face],
                                     .size          = entry.size,
                                     .lightPosition = l.position,
                                     .lightRange    = l.range,
                                     .texelScale    = 2.0f * tanHalf / static_cast<float>(entry.size),
                                     .render        = !entry.valid[face],
                                     .cacheEntry    = static_cast<std::uint32_t>(it - m_ShadowCache.begin()),
                                     .face          = face});
        };
        if (entry.views == kCubeFaces) {
            const float tanHalf = ShadowTanHalfWithBorder(1.0f, entry.size, border);
            for (std::uint32_t face = 0; face < kCubeFaces; ++face)
                addView(face, CubeFaceView(l.position, face), tanHalf);
        } else {
            // Wide cones (> 85 degrees) are only shadowed up to 85 degrees.
            const float     outer   = std::acos(std::clamp(l.cosOuter, 0.0f, 1.0f));
            const float     tanHalf = ShadowTanHalfWithBorder(std::tan(std::min(outer, glm::radians(85.0f))), entry.size, border);
            const glm::vec3 up      = std::abs(l.direction.y) > 0.99f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            addView(0, glm::lookAt(l.position, l.position + l.direction, up), tanHalf);
        }
        ++m_Stats.shadowedLights;
    }

    const float invAtlas = 1.0f / static_cast<float>(atlasSize);
    for (const ShadowTile& tile : m_ShadowTiles) {
        m_ShadowViewData.push_back({.viewProj = tile.viewProj,
                                    .rect     = glm::vec4(glm::vec2(tile.offset) * invAtlas,
                                                          glm::vec2(static_cast<float>(tile.size) * invAtlas)),
                                    .params   = glm::vec4(tile.texelScale, 0.0f, 0.0f, 0.0f)});
        m_Stats.shadowTilesRendered += tile.render ? 1u : 0u;
    }
    m_Stats.shadowTiles = static_cast<std::uint32_t>(m_ShadowTiles.size());
}

void SceneRenderer::RenderLocalShadows(VkCommandBuffer cmd, VkDeviceAddress frameAddress)
{
    const auto&            bindless  = m_Renderer.GetBindless();
    const std::uint32_t    atlasSize = m_ShadowAtlas.Extent().width;
    constexpr VkPipelineStageFlags2 kDepthStages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

    // Earlier frames may still sample the atlas (lighting, debug view): WAR on their fragment shaders.
    // Cached tiles must survive: keep the contents unless the atlas was never written.
    CmdImageBarrier(cmd, {.image     = m_ShadowAtlas.Handle(),
                          .oldLayout = m_ShadowAtlasWritten ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                            : VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .dstStage  = kDepthStages,
                          .dstAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .aspect    = VK_IMAGE_ASPECT_DEPTH_BIT});

    // Only re-rendered tiles are cleared (to 0 = far); cached ones are loaded as they are.
    const auto depth = Attachment(m_ShadowAtlas.View(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                  m_ShadowAtlasWritten ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    BeginRendering(cmd, {atlasSize, atlasSize}, nullptr, &depth);
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    vkCmdSetDepthBias(cmd, -localShadows.depthBias, 0.0f, -localShadows.slopeBias); // reverse-Z

    for (std::uint32_t t = 0; t < m_ShadowTiles.size(); ++t) {
        const ShadowTile& tile = m_ShadowTiles[t];
        if (!tile.render)
            continue;
        m_ShadowCache[tile.cacheEntry].valid[tile.face] = true;
        const VkRect2D    rect{{static_cast<std::int32_t>(tile.offset.x), static_cast<std::int32_t>(tile.offset.y)},
                               {tile.size, tile.size}};
        const VkViewport viewport{static_cast<float>(tile.offset.x), static_cast<float>(tile.offset.y),
                                  static_cast<float>(tile.size), static_cast<float>(tile.size), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &rect);
        VkClearAttachment clear{};
        clear.aspectMask                      = VK_IMAGE_ASPECT_DEPTH_BIT;
        clear.clearValue.depthStencil         = {0.0f, 0};
        const VkClearRect clearRect{rect, 0, 1};
        vkCmdClearAttachments(cmd, 1, &clear, 1, &clearRect);

        const std::uint32_t cascade = kMaxCascades + t; // shadow.vert: local view t
        if (m_GpuFrame) {
            DrawGpuShadow(cmd, tile.gpuView, frameAddress, cascade, m_LocalShadow, m_LocalShadowMasked);
            m_Stats.localShadowDraws += kShadowBuckets; // indirect multi-draws
        } else {
            DrawList list;
            GatherDraws(Frustum::FromViewProjection(tile.viewProj), glm::vec4(tile.lightPosition, tile.lightRange), list,
                        Gather::Shadow);
            DrawCpuShadow(cmd, list, frameAddress, cascade, m_LocalShadow, m_LocalShadowMasked, m_Stats.localShadowDraws);
        }
    }
    vkCmdEndRendering(cmd);

    CmdImageBarrier(cmd, {.image     = m_ShadowAtlas.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = kDepthStages,
                          .srcAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                          .aspect    = VK_IMAGE_ASPECT_DEPTH_BIT});
    m_ShadowAtlasWritten = true;
}

void SceneRenderer::CollectLights(Scene& scene, const Frustum& frustum)
{
    m_Lights.clear();
    m_LightCastsShadows.clear();
    m_LightEntities.clear();
    m_Stats.lightsTotal      = static_cast<std::uint32_t>(m_Spatial.LightCount());
    const Registry& registry = scene.GetRegistry();
    m_Spatial.QueryLights(frustum, [&](Entity entity) { // light BVH: only candidates near the view
        const Light&          light = registry.Get<Light>(entity);
        const WorldTransform& world = registry.Get<WorldTransform>(entity);
        const float range = EffectiveRange(light);
        const glm::vec3 radiance = light.color * light.intensity;
        if (!lights.enabled || range <= 0.0f || std::max({radiance.r, radiance.g, radiance.b}) <= 0.0f)
            return;
        const glm::vec3 position = world.matrix[3];
        if (!frustum.Intersects({position - glm::vec3(range), position + glm::vec3(range)}))
            return;
        if (m_Lights.size() >= kMaxVisibleLights) {
            static bool warned = false;
            if (!std::exchange(warned, true))
                ENGINE_WARN("More than {} visible lights: the rest is dropped", kMaxVisibleLights);
            return;
        }

        m_LightCastsShadows.push_back(light.castShadows);
        m_LightEntities.push_back(entity);
        GpuLight& gpu = m_Lights.emplace_back();
        gpu.position  = position;
        gpu.range     = range;
        gpu.color     = radiance;
        gpu.direction = glm::normalize(-glm::vec3(world.matrix[2])); // local -Z (glTF)
        if (light.type == LightType::Spot) {
            const float outer = std::clamp(light.outerConeAngle, 1e-3f, glm::half_pi<float>());
            const float inner = std::clamp(light.innerConeAngle, 0.0f, outer);
            const float cosOuter = std::cos(outer);
            gpu.spotScale  = 1.0f / std::max(std::cos(inner) - cosOuter, 1e-4f);
            gpu.spotOffset = -cosOuter * gpu.spotScale;
            gpu.cosOuter   = cosOuter;
            gpu.sinOuter   = std::sin(outer);
            gpu.type       = 1;
        }
    });
    m_Stats.lights = static_cast<std::uint32_t>(m_Lights.size());
}

std::uint32_t SceneRenderer::DecalTexture(const std::string& path, TextureKind kind)
{
    if (path.empty())
        return ~0u;
    const std::string key = std::to_string(static_cast<int>(kind)) + '|' + path;
    auto it = m_DecalTextures.find(key);
    if (it == m_DecalTextures.end())
        it = m_DecalTextures.emplace(key, DecalTextureEntry{.handle = m_Assets.LoadTexture(PathFromUtf8(path), kind)}).first;
    it->second.lastUsed = m_FrameCounter;
    return m_Assets.TableEntry(it->second.handle); // neutral default while loading, placeholder on failure
}

void SceneRenderer::ReleaseUnusedDecalTextures()
{
    constexpr std::uint64_t kKeepFrames = 300; // keep briefly unused ones (decals culled for a moment)
    for (auto it = m_DecalTextures.begin(); it != m_DecalTextures.end();) {
        if (m_FrameCounter - it->second.lastUsed > kKeepFrames) {
            m_Assets.Release(it->second.handle); // the table entry is freed deferred
            it = m_DecalTextures.erase(it);
        } else {
            ++it;
        }
    }
}

void SceneRenderer::CollectDecals(Scene& scene, const Frustum& frustum)
{
    m_Decals.clear();
    if (!decals.enabled)
        return;
    struct Candidate {
        std::int32_t  order;
        std::uint64_t uuid;
        GpuDecal      gpu;
    };
    std::vector<Candidate> candidates;
    const Registry& registry = scene.GetRegistry();
    registry.ViewOf<Decal, WorldTransform>().Each([&](Entity entity, const Decal& decal, const WorldTransform& world) {
        const glm::mat4 box = world.matrix * glm::scale(glm::mat4(1.0f), glm::max(glm::abs(decal.halfExtents), glm::vec3(1e-3f)));
        const float     det = glm::determinant(glm::mat3(box));
        if (!std::isfinite(det) || std::abs(det) < 1e-12f)
            return;
        const glm::vec3 center = box[3];
        const float     radius = glm::length(glm::vec3(box[0])) + glm::length(glm::vec3(box[1])) + glm::length(glm::vec3(box[2]));
        if (!frustum.Intersects({center - glm::vec3(radius), center + glm::vec3(radius)}))
            return;
        float opacity = decal.color.a;
        if (decal.lifetime > 0.0f && decal.fadeOut > 0.0f) // last seconds of the lifetime
            opacity *= std::clamp((decal.lifetime - decal.age) / decal.fadeOut, 0.0f, 1.0f);
        const float start = glm::radians(std::clamp(decal.angleFade, 0.0f, 89.0f));
        const float end   = std::max(start + glm::radians(5.0f), glm::radians(89.0f));
        GpuDecal gpu{.worldToDecal = glm::inverse(box),
                     .color        = glm::vec4(glm::vec3(decal.color), opacity),
                     .emissive     = glm::vec4(decal.emissiveColor, decal.normalStrength),
                     .textures     = glm::uvec4(DecalTexture(decal.albedo, TextureKind::Color),
                                                DecalTexture(decal.normal, TextureKind::Normal),
                                                DecalTexture(decal.mask, TextureKind::Linear),
                                                DecalTexture(decal.emissive, TextureKind::Color)),
                     .params       = glm::vec4(decal.roughness, decal.metallic, decal.materialOpacity, decal.edgeFade),
                     .fade         = glm::vec4(std::cos(start), std::cos(end), 0.0f, 0.0f),
                     .tangent      = glm::vec4(glm::normalize(glm::vec3(world.matrix[0])), 0.0f),
                     .axis         = glm::vec4(-glm::normalize(glm::vec3(world.matrix[2])), 0.0f),
                     .bounds       = glm::vec4(center, radius)};
        candidates.push_back({decal.sortOrder, registry.Get<Uuid>(entity).value, gpu});
    });
    // Sort order first, then a stable tie-break: the cluster lists keep this order.
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        return a.order != b.order ? a.order < b.order : a.uuid < b.uuid;
    });
    if (candidates.size() > kMaxVisibleDecals) {
        static bool warned = false;
        if (!std::exchange(warned, true))
            ENGINE_WARN("More than {} visible decals: the rest is dropped", kMaxVisibleDecals);
        candidates.resize(kMaxVisibleDecals);
    }
    for (const Candidate& c : candidates)
        m_Decals.push_back(c.gpu);
    m_Stats.decals = static_cast<std::uint32_t>(m_Decals.size());
    ReleaseUnusedDecalTextures();
}

void SceneRenderer::CullLights(VkCommandBuffer cmd, VkDeviceAddress frameAddress)
{
    const auto& bindless = m_Renderer.GetBindless();
    // Shared by all frames in flight: the previous frame's shading / debug view reads (WAR) and its
    // culling writes (WAW) come first.
    MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_LightCull.Handle());
    const LightCullPush push{.frame = frameAddress, .clusters = m_Clusters.Address(), .decalClusters = m_DecalClusters.Address()};
    vkCmdPushConstants(cmd, bindless.PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, kClusterCount / kLightCullGroupSize, 1, 1);
    // Read by shading (fragment) and the volumetric fog (compute).
    MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void SceneRenderer::RenderShadows(VkCommandBuffer cmd, VkDeviceAddress frameAddress,
                                  const std::array<Cascade, kMaxCascades>& cascades, std::uint32_t cascadeCount)
{
    const auto&            bindless   = m_Renderer.GetBindless();
    const std::uint32_t    resolution = m_ShadowMap.Extent().width;
    constexpr VkPipelineStageFlags2 kDepthStages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

    // Previous frames may still sample the maps (WAR): wait for their fragment shaders.
    CmdImageBarrier(cmd, {.image     = m_ShadowMap.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .dstStage  = kDepthStages,
                          .dstAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .aspect    = VK_IMAGE_ASPECT_DEPTH_BIT});

    // Not flipped (unlike the main pass): shadow UV = NDC * 0.5 + 0.5 in the shader.
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(resolution), static_cast<float>(resolution), 0.0f, 1.0f};
    const VkRect2D   scissor{{0, 0}, {resolution, resolution}};

    for (std::uint32_t c = 0; c < cascadeCount; ++c) {
        auto depth = Attachment(m_ShadowViews[c].Handle(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                VK_ATTACHMENT_LOAD_OP_CLEAR);
        depth.clearValue.depthStencil = {0.0f, 0}; // reverse-Z: far = 0
        BeginRendering(cmd, {resolution, resolution}, nullptr, &depth);
        bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdSetDepthBias(cmd, -shadows.depthBias, 0.0f, -shadows.slopeBias); // reverse-Z: away from the light

        // Casters between the light and the cascade stay (clamped onto its near plane).
        if (m_GpuFrame) {
            DrawGpuShadow(cmd, kCameraLateView + 1 + c, frameAddress, c, m_Shadow, m_ShadowMasked);
        } else {
            DrawList list;
            GatherDraws(Frustum::FromViewProjection(cascades[c].viewProj, false), glm::vec4(0.0f), list, Gather::Shadow);
            DrawCpuShadow(cmd, list, frameAddress, c, m_Shadow, m_ShadowMasked, m_Stats.shadowDraws);
        }
        m_Terrain->DrawShadow(cmd, frameAddress, c);
        vkCmdEndRendering(cmd);
    }

    CmdImageBarrier(cmd, {.image     = m_ShadowMap.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = kDepthStages,
                          .srcAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                          .aspect    = VK_IMAGE_ASPECT_DEPTH_BIT});
}

void SceneRenderer::EnsurePickingTarget(VkExtent2D extent)
{
    if (!overlay.picking) {
        ReleasePickingTarget();
        m_PickRequest.reset();
        return;
    }
    if (m_EntityIds && m_EntityIds.Extent().width == extent.width && m_EntityIds.Extent().height == extent.height)
        return;
    ReleasePickingTarget();
    m_EntityIds    = Image(m_Renderer.GetContext(), {.extent    = {extent.width, extent.height, 1},
                                                     .format    = kEntityIdFormat,
                                                     .usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                                     .debugName = "EntityIds"});
    m_EntityIdSlot = m_Renderer.GetBindless().AddSampledImage(m_EntityIds.View());
}

void SceneRenderer::ReleasePickingTarget()
{
    if (!m_EntityIds)
        return;
    Renderer* r = &m_Renderer;
    r->DeferCall([r, slot = m_EntityIdSlot] { r->GetBindless().RemoveSampledImage(slot); });
    r->DeferRelease(std::move(m_EntityIds));
    m_EntityIds    = {};
    m_EntityIdSlot = 0;
}

void SceneRenderer::ReadPick(const Scene& scene, std::uint32_t frameIndex)
{
    if (!std::exchange(m_PickPending[frameIndex], false))
        return;
    Buffer& readback = m_PickReadback[frameIndex];
    readback.Invalidate(0, VK_WHOLE_SIZE);
    std::uint32_t id = 0;
    std::memcpy(&id, readback.Mapped(), sizeof(id));

    // Slot index -> current entity; the slot may have been freed or reused since.
    const Registry& registry = scene.GetRegistry();
    const Entity    entity   = id == 0 ? NullEntity : registry.EntityAtIndex(id - 1);
    m_PickResult             = entity != NullEntity && registry.Has<Hierarchy>(entity) ? entity : NullEntity;
}

std::pair<VkDeviceAddress, std::uint32_t> SceneRenderer::PushOutlineBits()
{
    if (!m_EntityIds || overlay.outlined.empty())
        return {0, 0};
    std::uint32_t maxIndex = 0;
    for (Entity e : overlay.outlined)
        maxIndex = std::max(maxIndex, EntityIndex(e));
    std::vector<std::uint32_t> words(maxIndex / 32 + 1, 0u);
    for (Entity e : overlay.outlined)
        words[EntityIndex(e) / 32] |= 1u << (EntityIndex(e) % 32);
    const TransientAllocation a = m_Renderer.AllocateTransient(words.size() * sizeof(std::uint32_t), 16);
    std::memcpy(a.cpu, words.data(), words.size() * sizeof(std::uint32_t));
    return {a.gpu, static_cast<std::uint32_t>(words.size())};
}

void SceneRenderer::RenderPrepass(VkCommandBuffer cmd, VkExtent2D extent, VkDeviceAddress frameAddress,
                                  std::uint32_t frameIndex)
{
    // Depth is shared by all frames in flight: earlier frames' tests, GTAO and denoise may still use it.
    constexpr VkPipelineStageFlags2 kDepthStages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    CmdImageBarrier(cmd, {.image     = m_Depth.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .srcStage  = kDepthStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, // + tone mapping debug views
                          .srcAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .dstStage  = kDepthStages,
                          .dstAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .aspect    = VK_IMAGE_ASPECT_DEPTH_BIT});

    // Normals are shared by all frames in flight: the previous frame's GTAO (and the normals debug
    // view) may still read them.
    CmdImageBarrier(cmd, {.image     = m_Normals.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});

    // Motion vectors: the previous frame's TAA / reflections (compute) and debug view read them.
    CmdImageBarrier(cmd, {.image     = m_Velocity.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});

    // Entity IDs: the previous frame's outline (fragment) and pick copy (transfer) read them.
    const bool picking = static_cast<bool>(m_EntityIds);
    if (picking)
        CmdImageBarrier(cmd, {.image     = m_EntityIds.Handle(),
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              .srcStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                              .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                              .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});

    std::array<VkRenderingAttachmentInfo, 3> colors{
        Attachment(m_Normals.View(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR),
        Attachment(m_Velocity.View(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR),
        Attachment(picking ? m_EntityIds.View() : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_ATTACHMENT_LOAD_OP_CLEAR)};
    colors[0].clearValue.color = {{0.0f, 0.0f, 1.0f, 0.0f}}; // sky: facing the camera
    colors[1].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // sky: camera motion only (TAA reprojects it)
    colors[2].clearValue.color.uint32[0] = 0;                // no entity
    auto depth = Attachment(m_Depth.View(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR);
    depth.clearValue.depthStencil = {0.0f, 0}; // reverse-Z: far = 0

    const auto drawPass = [&](auto&& draw) {
        BeginRendering(cmd, extent, colors.data(), &depth, picking ? 3u : 2u);
        m_Renderer.GetBindless().Bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
        SetViewportScissor(cmd, extent);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, picking ? m_PrepassPicking.Handle() : m_Prepass.Handle());
        draw();
        vkCmdEndRendering(cmd);
    };
    if (!m_GpuFrame) {
        drawPass([&] {
            DrawCpuCamera(cmd, m_CameraDraws, frameAddress, false, 0);
            m_Terrain->DrawPrepass(cmd, frameAddress, picking);
        });
    } else {
        // Early: what was visible last frame (or everything in the frustum without occlusion culling).
        // The terrain goes in here too: it occludes most of the scene in the Hi-Z.
        drawPass([&] {
            DrawGpuCamera(cmd, kCameraEarlyView, frameAddress, 0);
            m_Terrain->DrawPrepass(cmd, frameAddress, picking);
        });
        if (culling.occlusion) {
            // Hi-Z of the early depth -> late culling -> draws that became visible, on top.
            CmdImageBarrier(cmd, {.image     = m_Depth.Handle(),
                                  .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                                  .srcStage  = kDepthStages,
                                  .srcAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                  .dstStage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                  .aspect    = VK_IMAGE_ASPECT_DEPTH_BIT});
            m_GpuCulling->BuildHiZ(cmd, m_DepthSlot, extent, culling.freeze);
            m_GpuCulling->CullLate(cmd);
            const std::array<VkImageMemoryBarrier2, 4> resume{
                MakeImageBarrier({.image     = m_Depth.Handle(),
                                  .oldLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                  .srcStage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  .dstStage  = kDepthStages,
                                  .dstAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                  .aspect    = VK_IMAGE_ASPECT_DEPTH_BIT}),
                MakeImageBarrier({.image     = m_Normals.Handle(),
                                  .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                  .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT}),
                MakeImageBarrier({.image     = m_Velocity.Handle(),
                                  .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                  .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT}),
                MakeImageBarrier({.image     = picking ? m_EntityIds.Handle() : m_Normals.Handle(),
                                  .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                  .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT})};
            VkDependencyInfo resumeDep{};
            resumeDep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            resumeDep.imageMemoryBarrierCount = picking ? 4u : 3u;
            resumeDep.pImageMemoryBarriers    = resume.data();
            vkCmdPipelineBarrier2(cmd, &resumeDep);
            for (VkRenderingAttachmentInfo& color : colors)
                color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            drawPass([&] { DrawGpuCamera(cmd, kCameraLateView, frameAddress, 0); });
        }
    }

    if (picking) {
        // Requested pixel -> this slot's readback buffer (read when the slot comes around again).
        const bool copy = m_PickRequest && m_PickRequest->x < extent.width && m_PickRequest->y < extent.height;
        CmdImageBarrier(cmd, {.image     = m_EntityIds.Handle(),
                              .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              .newLayout = copy ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                              .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                              .dstStage  = copy ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              .dstAccess = copy ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
        if (copy) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset      = {static_cast<std::int32_t>(m_PickRequest->x),
                                       static_cast<std::int32_t>(m_PickRequest->y), 0};
            region.imageExtent      = {1, 1, 1};
            vkCmdCopyImageToBuffer(cmd, m_EntityIds.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   m_PickReadback[frameIndex].Handle(), 1, &region);
            CmdImageBarrier(cmd, {.image     = m_EntityIds.Handle(),
                                  .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  .srcStage  = VK_PIPELINE_STAGE_2_COPY_BIT,
                                  .dstStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
            m_PickPending[frameIndex] = true;
        }
        m_PickRequest.reset();
    }

    // Depth: read-only attachment for the lighting pass and sampled by GTAO from here on.
    const std::array<VkImageMemoryBarrier2, 3> barriers{
        MakeImageBarrier({.image     = m_Depth.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                          .srcStage  = kDepthStages,
                          .srcAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .dstStage  = kDepthStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                          .aspect    = VK_IMAGE_ASPECT_DEPTH_BIT}),
        MakeImageBarrier({.image     = m_Normals.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT}),
        MakeImageBarrier({.image     = m_Velocity.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT})};
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    dep.pImageMemoryBarriers    = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

void SceneRenderer::RenderAmbientOcclusion(VkCommandBuffer cmd, VkExtent2D extent, const CameraData& camera)
{
    const auto&            bindless = m_Renderer.GetBindless();
    const VkPipelineLayout layout   = bindless.PipelineLayout();
    constexpr auto         kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const glm::vec2        invSize{1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height)};
    const std::uint32_t    groupsX = (extent.width + 7) / 8;
    const std::uint32_t    groupsY = (extent.height + 7) / 8;

    // Shared targets: previous frames' denoise (compute) and lighting (fragment) reads first (WAR).
    const std::array<VkImageMemoryBarrier2, 2> toGeneral{
        MakeImageBarrier({.image     = m_AoRaw.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                          .srcStage  = kCompute,
                          .dstStage  = kCompute,
                          .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT}),
        MakeImageBarrier({.image     = m_Ao.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstStage  = kCompute,
                          .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT})};
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(toGeneral.size());
    dep.pImageMemoryBarriers    = toGeneral.data();
    vkCmdPipelineBarrier2(cmd, &dep);

    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    const GtaoPush gtao{.depth        = m_DepthSlot,
                        .normals      = m_NormalSlot,
                        .dst          = m_AoRawStorage,
                        .sliceCount   = std::max(ao.sliceCount, 1u),
                        .invSize      = invSize,
                        .tanHalfFov   = {1.0f / camera.projection[0][0], 1.0f / camera.projection[1][1]},
                        .nearPlane    = camera.nearPlane,
                        .radius       = ao.radius,
                        .falloff      = std::clamp(ao.falloff, 0.05f, 1.0f),
                        .power        = ao.power,
                        .stepsPerSide = std::max(ao.stepsPerSide, 1u)};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Gtao.Handle());
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(gtao), &gtao);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);
    MemoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    const DenoisePush denoise{.src       = m_AoRawSampled,
                              .depth     = m_DepthSlot,
                              .dst       = m_AoStorage,
                              .pad       = 0,
                              .invSize   = invSize,
                              .nearPlane = camera.nearPlane,
                              .sharpness = ao.sharpness};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_GtaoDenoise.Handle());
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(denoise), &denoise);
    vkCmdDispatch(cmd, groupsX, groupsY, 1);
    MemoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void SceneRenderer::RenderMain(VkCommandBuffer cmd, VkExtent2D extent, VkDeviceAddress frameAddress,
                               std::uint32_t frameIndex)
{
    const auto&            bindless = m_Renderer.GetBindless();
    const VkPipelineLayout layout   = bindless.PipelineLayout();

    // The HDR target is shared by all frames in flight: wait for the previous frame's bloom,
    // exposure and tone mapping reads before overwriting (WAR, execution dependency only).
    CmdImageBarrier(cmd, {.image     = m_Hdr.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});

    // Motion vectors of the prepass: transparent surfaces and particles blend theirs on top.
    CmdImageBarrier(cmd, {.image     = m_Velocity.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});

    // No color clear: the sky covers every pixel without geometry. Depth comes from the prepass.
    const std::array<VkRenderingAttachmentInfo, 2> colors{
        Attachment(m_Hdr.View(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE),
        Attachment(m_Velocity.View(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD)};
    auto depth     = Attachment(m_Depth.View(), VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD);
    depth.storeOp  = VK_ATTACHMENT_STORE_OP_NONE; // read-only: no store access at all

    BeginRendering(cmd, extent, colors.data(), &depth, 2);
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    SetViewportScissor(cmd, extent);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Mesh.Handle());
    const std::uint32_t lodTint = post.debugView == DebugView::Lod ? kMeshTintLod : 0u;
    if (m_GpuFrame) {
        DrawGpuCamera(cmd, kCameraEarlyView, frameAddress, lodTint);
        if (culling.occlusion)
            DrawGpuCamera(cmd, kCameraLateView, frameAddress,
                          (post.debugView == DebugView::Culling ? kMeshTintLate : 0u) | lodTint);
    } else {
        DrawCpuCamera(cmd, m_CameraDraws, frameAddress, true, lodTint);
    }
    m_Terrain->DrawLighting(cmd, frameAddress);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Sky.Handle());
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(frameAddress), &frameAddress);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    // Transparent surfaces last, over opaque geometry and sky (depth tested, not written).
    DrawTransparent(cmd, m_TransparentDraws, frameAddress, lodTint);
    // Particles after everything else (unlit, sorted per emitter and between emitters).
    m_Particles->Draw(cmd, frameAddress, m_DepthSlot, frameIndex);
    const ParticleRenderStats& particleStats = m_Particles->Stats();
    m_Stats.particleBatches   = particleStats.batches;
    m_Stats.particlesDrawn    = particleStats.particles + particleStats.ribbonPoints;
    m_Stats.particleDrawCalls = particleStats.drawCalls;
    vkCmdEndRendering(cmd);
    CmdImageBarrier(cmd, {.image     = m_Velocity.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
}

void SceneRenderer::RenderTaa(VkCommandBuffer cmd, VkExtent2D extent, VkDeviceAddress frameAddress)
{
    const auto&    bindless = m_Renderer.GetBindless();
    constexpr auto kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (!m_HistoryLayoutReady) { // both stay in GENERAL from here on
        for (const Image& history : m_History)
            CmdImageBarrier(cmd, {.image     = history.Handle(),
                                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                  .srcStage  = kCompute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  .dstStage  = kCompute,
                                  .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
        m_HistoryLayoutReady = true;
        m_HistoryValid       = false;
    }
    // Last frame's resolve is read (RAW); the target was read by earlier frames' bloom, exposure,
    // tone mapping and reflections (WAR).
    MemoryBarrier(cmd, kCompute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const std::uint32_t target = m_HistoryIndex;
    const std::uint32_t source = target ^ 1u;
    const TaaPush push{.frame    = frameAddress,
                       .current  = m_HdrSlot,
                       .history  = m_HistorySampled[source],
                       .target   = m_HistoryStorage[target],
                       .depth    = m_DepthSlot,
                       .velocity = m_VelocitySlot,
                       .feedback = std::clamp(taa.feedback, 0.0f, 0.98f),
                       .valid    = m_HistoryValid ? 1u : 0u,
                       .pad      = 0,
                       .size     = {extent.width, extent.height}};
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Taa.Handle());
    vkCmdPushConstants(cmd, bindless.PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    MemoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_ResolvedSlot = m_HistorySampled[target];
    m_HistoryIndex = source;
    m_HistoryValid = true;
}

void SceneRenderer::RenderSsr(VkCommandBuffer cmd, VkExtent2D extent, VkDeviceAddress frameAddress)
{
    const auto&            bindless = m_Renderer.GetBindless();
    const VkPipelineLayout layout   = bindless.PipelineLayout();
    constexpr auto         kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr auto         kRW      = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    // Closest-depth pyramid: level 0 = full resolution, then halved down to 1 x 1.
    std::vector<glm::uvec2> sizes{glm::uvec2(extent.width, extent.height)};
    while (sizes.back().x > 1 || sizes.back().y > 1)
        sizes.push_back(glm::max((sizes.back() + 1u) / 2u, glm::uvec2(1)));
    VkDeviceSize total = 0;
    for (const glm::uvec2& s : sizes)
        total += VkDeviceSize{s.x} * s.y;
    if (!m_SsrPyramid || m_SsrPyramid.Size() < total * sizeof(float)) {
        if (m_SsrPyramid)
            m_Renderer.DeferRelease(std::move(m_SsrPyramid));
        m_SsrPyramid = Buffer(m_Renderer.GetContext(), {.size = total * sizeof(float), .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                        .debugName = "SsrDepthPyramid"});
    }
    m_SsrLevels = static_cast<std::uint32_t>(sizes.size());

    // Earlier frames read the pyramid and the targets (compute, lighting, debug view): WAR.
    MemoryBarrier(cmd, kCompute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, kRW);
    if (!m_SsrLayoutReady) {
        for (const Image* image : {&m_SsrRaw, &m_SsrTemp, &m_SsrHistory[0], &m_SsrHistory[1]})
            CmdImageBarrier(cmd, {.image     = image->Handle(),
                                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                  .srcStage  = kCompute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  .dstStage  = kCompute,
                                  .dstAccess = kRW});
        m_SsrLayoutReady  = true;
        m_SsrHistoryValid = false;
    }
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_SsrHiZ.Handle());
    VkDeviceSize offset = 0;
    for (std::size_t level = 0; level < sizes.size(); ++level) {
        const VkDeviceSize previous = level > 0 ? offset - VkDeviceSize{sizes[level - 1].x} * sizes[level - 1].y : 0;
        const SsrHiZPush push{.src       = m_SsrPyramid.Address() + previous * sizeof(float),
                              .dst       = m_SsrPyramid.Address() + offset * sizeof(float),
                              .srcSize   = level == 0 ? sizes[0] : sizes[level - 1],
                              .dstSize   = sizes[level],
                              .depthSlot = m_DepthSlot,
                              .fromDepth = level == 0 ? 1u : 0u};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (sizes[level].x + 7) / 8, (sizes[level].y + 7) / 8, 1);
        MemoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        offset += VkDeviceSize{sizes[level].x} * sizes[level].y;
    }

    const VkExtent2D half = m_SsrRaw.Extent2D();
    const std::uint32_t target = m_SsrIndex, source = target ^ 1u;
    SsrPush push{.frame        = frameAddress,
                 .pyramid      = m_SsrPyramid.Address(),
                 .fullSize     = {extent.width, extent.height},
                 .size         = {half.width, half.height},
                 .levels       = m_SsrLevels,
                 .depth        = m_DepthSlot,
                 .normals      = m_NormalSlot,
                 .velocity     = m_VelocitySlot,
                 .source       = taa.enabled ? m_HistorySampled[m_HistoryIndex ^ 1u] : m_HdrSlot, // last frame
                 .history      = 0,
                 .target       = m_SsrRawStorage,
                 .mode         = std::clamp(ssr.maxSteps, 8u, 256u),
                 .maxRoughness = std::clamp(ssr.maxRoughness, 0.05f, 1.0f),
                 .thickness    = std::max(ssr.thickness, 0.01f),
                 .maxDistance  = std::max(ssr.maxDistance, 0.5f),
                 .feedback     = std::clamp(ssr.feedback, 0.0f, 0.98f)};
    const auto dispatch = [&](const Pipeline& pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Handle());
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (half.width + 7) / 8, (half.height + 7) / 8, 1);
        MemoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    dispatch(m_SsrTrace);
    push.source  = m_SsrRawSampled; // temporal accumulation
    push.history = m_SsrHistorySampled[source];
    push.target  = m_SsrHistoryStorage[target];
    push.mode    = m_SsrHistoryValid ? 1u : 0u;
    dispatch(m_SsrResolve);
    push.source = m_SsrHistorySampled[target]; // blur: horizontal, then vertical into the result
    push.target = m_SsrTempStorage;
    push.mode   = 0;
    dispatch(m_SsrBlur);
    push.source = m_SsrTempSampled;
    push.target = m_SsrRawStorage;
    push.mode   = 1;
    dispatch(m_SsrBlur);
    MemoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_SsrIndex        = source;
    m_SsrHistoryValid = true;
}

void SceneRenderer::EnsureFogVolumes(VkExtent2D extent)
{
    const VkExtent3D grid{(extent.width + kFogTileSize - 1) / kFogTileSize, (extent.height + kFogTileSize - 1) / kFogTileSize,
                          kFogSlices};
    if (m_FogIntegrated && m_FogIntegrated.Extent().width == grid.width && m_FogIntegrated.Extent().height == grid.height)
        return;
    ReleaseFogVolumes();
    const VulkanContext& ctx      = m_Renderer.GetContext();
    BindlessRegistry&    bindless = m_Renderer.GetBindless();
    const auto make = [&](const char* name, std::uint32_t& sampledSlot, std::uint32_t& storageSlot) {
        Image image = Image(ctx, {.extent    = grid,
                                  .format    = kHdrFormat,
                                  .usage     = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  .viewType  = VK_IMAGE_VIEW_TYPE_3D,
                                  .debugName = name});
        sampledSlot = bindless.AddVolumeTexture(image.View(), VK_IMAGE_LAYOUT_GENERAL);
        storageSlot = bindless.AddStorageVolume(image.View());
        return image;
    };
    m_FogInjectVolume[0] = make("FogScattering", m_FogInjectSampled[0], m_FogInjectStorage[0]);
    m_FogInjectVolume[1] = make("FogScattering", m_FogInjectSampled[1], m_FogInjectStorage[1]);
    m_FogIntegrated      = make("FogIntegrated", m_FogIntegratedSampled, m_FogIntegratedStorage);
}

void SceneRenderer::ReleaseFogVolumes()
{
    if (!m_FogIntegrated)
        return;
    Renderer* r = &m_Renderer;
    r->DeferCall([r, sampled = std::array{m_FogInjectSampled[0], m_FogInjectSampled[1], m_FogIntegratedSampled},
                  storage = std::array{m_FogInjectStorage[0], m_FogInjectStorage[1], m_FogIntegratedStorage}] {
        BindlessRegistry& b = r->GetBindless();
        for (std::uint32_t s : sampled)
            b.RemoveVolumeTexture(s);
        for (std::uint32_t s : storage)
            b.RemoveStorageVolume(s);
    });
    for (Image* image : {&m_FogInjectVolume[0], &m_FogInjectVolume[1], &m_FogIntegrated})
        r->DeferRelease(std::move(*image));
    m_FogLayoutReady  = false;
    m_FogHistoryValid = false;
}

void SceneRenderer::CollectFogVolumes(Scene& scene, const Frustum& frustum, const glm::vec3& cameraPosition)
{
    m_FogVolumes.clear();
    struct Candidate {
        float         distance;
        std::uint64_t uuid;
        GpuFogVolume  gpu;
    };
    std::vector<Candidate> candidates;
    const Registry& registry = scene.GetRegistry();
    registry.ViewOf<FogVolume, WorldTransform>().Each([&](Entity entity, const FogVolume& volume, const WorldTransform& world) {
        if (volume.density <= 0.0f && glm::all(glm::lessThanEqual(volume.emissive, glm::vec3(0.0f))))
            return;
        const float det = glm::determinant(glm::mat3(world.matrix));
        if (!std::isfinite(det) || std::abs(det) < 1e-12f)
            return;
        const glm::vec3 half   = glm::max(glm::abs(volume.halfExtents), glm::vec3(1e-3f));
        const glm::vec3 center = world.matrix[3];
        float           radius = 0.0f; // farthest corner
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local((corner & 1) ? half.x : -half.x, (corner & 2) ? half.y : -half.y,
                                  (corner & 4) ? half.z : -half.z);
            radius = std::max(radius, glm::length(glm::mat3(world.matrix) * local));
        }
        if (!frustum.Intersects({center - glm::vec3(radius), center + glm::vec3(radius)}))
            return;
        GpuFogVolume gpu{.worldToLocal  = glm::inverse(world.matrix),
                         .halfExtents   = glm::vec4(half, std::max(volume.edgeFade, 0.0f)),
                         .albedoDensity = glm::vec4(glm::clamp(volume.albedo, glm::vec3(0.0f), glm::vec3(1.0f)),
                                                    std::max(volume.density, 0.0f)),
                         .emissive      = glm::vec4(glm::max(volume.emissive, glm::vec3(0.0f)), 0.0f),
                         .bounds        = glm::vec4(center, radius)};
        candidates.push_back({std::max(glm::distance(center, cameraPosition) - radius, 0.0f), registry.Get<Uuid>(entity).value, gpu});
    });
    // The nearest ones win when there are too many; stable order for identical frames.
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.uuid < b.uuid;
    });
    if (candidates.size() > kMaxFogVolumes) {
        static bool warned = false;
        if (!std::exchange(warned, true))
            ENGINE_WARN("More than {} visible fog volumes: the farthest are dropped", kMaxFogVolumes);
        candidates.resize(kMaxFogVolumes);
    }
    for (const Candidate& c : candidates)
        m_FogVolumes.push_back(c.gpu);
    m_Stats.fogVolumes = static_cast<std::uint32_t>(m_FogVolumes.size());
}

void SceneRenderer::RenderFog(VkCommandBuffer cmd, VkDeviceAddress frameAddress)
{
    const auto&            bindless = m_Renderer.GetBindless();
    const VkPipelineLayout layout   = bindless.PipelineLayout();
    constexpr auto         kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr auto         kRW      = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    // Earlier frames read the volumes (history, lighting pass, particles, sky): WAR.
    MemoryBarrier(cmd, kCompute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, kRW);
    if (!m_FogLayoutReady) {
        for (const Image* image : {&m_FogInjectVolume[0], &m_FogInjectVolume[1], &m_FogIntegrated})
            CmdImageBarrier(cmd, {.image     = image->Handle(),
                                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                  .srcStage  = kCompute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  .dstStage  = kCompute,
                                  .dstAccess = kRW});
        m_FogLayoutReady  = true;
        m_FogHistoryValid = false;
    }
    const VkExtent3D    grid     = m_FogIntegrated.Extent();
    const float         distance = std::max(fog.volumetricDistance, 1.0f);
    const std::uint32_t target = m_FogIndex, source = target ^ 1u;
    FogPush push{.frame        = frameAddress,
                 .source       = m_FogInjectSampled[source],
                 .target       = m_FogInjectStorage[target],
                 .valid        = m_FogHistoryValid && m_FogPrevDistance > 0.0f ? 1u : 0u,
                 .gridX        = grid.width,
                 .gridY        = grid.height,
                 .gridZ        = grid.depth,
                 .feedback     = std::clamp(fog.temporalFeedback, 0.0f, 0.98f),
                 .prevDistance = m_FogPrevDistance};
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FogInject.Handle());
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (grid.width + 3) / 4, (grid.height + 3) / 4, (grid.depth + 3) / 4);
    MemoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    push.source = m_FogInjectSampled[target];
    push.target = m_FogIntegratedStorage;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FogIntegrate.Handle());
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (grid.width + 7) / 8, (grid.height + 7) / 8, 1);
    MemoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_FogIndex        = source;
    m_FogHistoryValid = true;
    m_FogPrevDistance = distance;
    m_Stats.fogGrid   = glm::uvec3(grid.width, grid.height, grid.depth);
}

void SceneRenderer::RenderBloom(VkCommandBuffer cmd)
{
    const auto&            bindless = m_Renderer.GetBindless();
    const VkPipelineLayout layout   = bindless.PipelineLayout();
    const auto             mips     = static_cast<std::uint32_t>(m_BloomViews.size());
    const VkExtent2D       base     = m_Bloom.Extent2D();
    constexpr auto         kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr auto         kRW      = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    // Previous frames' bloom passes and tone mapping may still read the chain (WAR).
    CmdImageBarrier(cmd, {.image     = m_Bloom.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | kCompute,
                          .dstStage  = kCompute,
                          .dstAccess = kRW});
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);

    const auto dispatch = [&](const Pipeline& pipeline, const BloomPush& push) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Handle());
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (push.dstSize.x + 7) / 8, (push.dstSize.y + 7) / 8, 1);
        MemoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, kRW);
    };

    // Downsample: HDR -> mip 0 -> mip 1 -> ...
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const VkExtent2D src = mip == 0 ? m_Hdr.Extent2D() : MipExtent(base, mip - 1);
        const VkExtent2D dst = MipExtent(base, mip);
        dispatch(m_BloomDown, {.src      = mip == 0 ? m_ResolvedSlot : m_BloomSampled[mip - 1],
                               .dst      = m_BloomStorage[mip],
                               .dstSize  = {dst.width, dst.height},
                               .srcTexel = 1.0f / glm::vec2(static_cast<float>(src.width), static_cast<float>(src.height)),
                               .karis    = mip == 0 ? 1u : 0u,
                               .radius   = 0.0f});
    }
    // Upsample: mip[i] += tent(mip[i + 1]), smallest to largest.
    for (std::uint32_t mip = mips - 1; mip-- > 0;) {
        const VkExtent2D dst = MipExtent(base, mip);
        dispatch(m_BloomUp, {.src      = m_BloomSampled[mip + 1],
                             .dst      = m_BloomStorage[mip],
                             .dstSize  = {dst.width, dst.height},
                             .srcTexel = glm::vec2(0.0f),
                             .karis    = 0u,
                             .radius   = post.bloomRadius});
    }
    MemoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void SceneRenderer::RenderExposure(VkCommandBuffer cmd, std::uint32_t frameIndex, float deltaTime)
{
    const auto&            bindless = m_Renderer.GetBindless();
    const VkPipelineLayout layout   = bindless.PipelineLayout();
    constexpr auto         kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr auto         kRW      = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

    if (!m_ExposureInitialized) { // zero histogram, "not adapted yet" state
        // New buffers may reuse memory that earlier (fenced, finished) frames read. That is safe, but
        // sync validation does not always see the host fence wait: make the order explicit.
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd, m_LuminanceHistogram.Handle(), 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cmd, m_ExposureState.Handle(), 0, VK_WHOLE_SIZE, 0);
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, // fill: COPY or CLEAR
                      kCompute | VK_PIPELINE_STAGE_2_COPY_BIT, kRW | VK_ACCESS_2_TRANSFER_READ_BIT);
        m_ExposureInitialized = true;
    }
    // Earlier frames' averaging, tone mapping and readback copies touch the same buffers (WAR, and
    // WAW for the readback copy of this slot: its write must be in the source access scope).
    MemoryBarrier(cmd, kCompute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  kCompute | VK_PIPELINE_STAGE_2_COPY_BIT, kRW | VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const VkExtent2D size     = m_Hdr.Extent2D();
    const float      logRange = std::max(post.maxLogLuminance - post.minLogLuminance, 1e-3f);
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);

    const HistogramPush histogram{.histogram            = m_LuminanceHistogram.Address(),
                                  .hdr                  = m_ResolvedSlot,
                                  .minLogLuminance      = post.minLogLuminance,
                                  .invLogLuminanceRange = 1.0f / logRange,
                                  .pad                  = 0,
                                  .size                 = {size.width, size.height}};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Histogram.Handle());
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(histogram), &histogram);
    vkCmdDispatch(cmd, (size.width + 15) / 16, (size.height + 15) / 16, 1);
    MemoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, kRW);

    // Exponential eye adaptation; the first frame (dt = 0, state 0) snaps to the target.
    const AveragePush average{.histogram         = m_LuminanceHistogram.Address(),
                              .state             = m_ExposureState.Address(),
                              .minLogLuminance   = post.minLogLuminance,
                              .logLuminanceRange = logRange,
                              .pixelCount        = static_cast<float>(size.width) * static_cast<float>(size.height),
                              .adaptation        = 1.0f - std::exp(-deltaTime * post.adaptationSpeed),
                              .key               = post.exposureKey,
                              .compensation      = post.exposure};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ExposureAverage.Handle());
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(average), &average);
    vkCmdDispatch(cmd, 1, 1, 1);
    MemoryBarrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);

    // Copy for the CPU (stats), read once this frame slot's fence has been waited on.
    Buffer&            readback = m_ExposureReadback[frameIndex];
    const VkBufferCopy region{0, 0, 2 * sizeof(float)};
    vkCmdCopyBuffer(cmd, m_ExposureState.Handle(), readback.Handle(), 1, &region);
    MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                  VK_ACCESS_2_HOST_READ_BIT);
    m_ExposureReadbackValid[frameIndex] = true;
}

void SceneRenderer::ReadExposure(std::uint32_t frameIndex)
{
    if (!m_ExposureReadbackValid[frameIndex])
        return;
    Buffer& readback = m_ExposureReadback[frameIndex];
    readback.Invalidate(0, VK_WHOLE_SIZE);
    float values[2];
    std::memcpy(values, readback.Mapped(), sizeof(values));
    m_Stats.averageLuminance = values[0];
    m_Stats.exposure         = values[1];
}

} // namespace Engine
