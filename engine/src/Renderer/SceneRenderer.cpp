#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Model.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Frustum.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

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
};

struct DrawData { // mirrors DrawData in mesh_common.glsl
    glm::mat4 model;
    glm::mat4 normalMatrix;
};

struct MeshPush { // mirrors MeshPush in mesh_common.glsl
    VkDeviceAddress frame;
    VkDeviceAddress vertices;
    VkDeviceAddress materials;
    VkDeviceAddress draw;
    std::uint32_t   materialIndex;
    std::uint32_t   cascade;
};
static_assert(sizeof(MeshPush) <= kPushConstantSize);

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
};

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
                    const VkRenderingAttachmentInfo* depth)
{
    VkRenderingInfo info{};
    info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea           = {{0, 0}, extent};
    info.layerCount           = 1;
    info.colorAttachmentCount = color ? 1u : 0u;
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

SceneRenderer::SceneRenderer(Renderer& renderer, const VulkanContext& ctx, const AssetManager& assets)
    : m_Renderer(renderer), m_Assets(assets), m_Environment(renderer)
{
    const VkDevice         device = ctx.Device();
    const VkPipelineLayout layout = renderer.GetBindless().PipelineLayout();

    // Prepass writes depth + view normals; the lighting pass then only shades the visible
    // surface (depth EQUAL, no writes). Both use mesh.vert with an invariant gl_Position.
    m_Prepass = GraphicsPipelineBuilder{}
                    .SetShaders(ShaderPath("mesh.vert.spv"), ShaderPath("depth_normal.frag.spv"))
                    .AddColorAttachment(kNormalFormat)
                    .SetDepthFormat(kDepthFormat)
                    .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL) // reverse-Z
                    .SetDynamicCulling(true)
                    .SetDebugName("DepthNormalPrepass")
                    .Build(device, layout);
    m_Mesh = GraphicsPipelineBuilder{}
                 .SetShaders(ShaderPath("mesh.vert.spv"), ShaderPath("mesh.frag.spv"))
                 .AddColorAttachment(kHdrFormat)
                 .SetDepthFormat(kDepthFormat)
                 .SetDepth(true, false, VK_COMPARE_OP_EQUAL)
                 .SetDynamicCulling(true)
                 .SetDebugName("MeshPbr")
                 .Build(device, layout);

    // Depth 0 = infinity: passes only where no geometry was drawn. No depth writes.
    m_Sky = GraphicsPipelineBuilder{}
                .SetShaders(ShaderPath("fullscreen.vert.spv"), ShaderPath("sky.frag.spv"))
                .AddColorAttachment(kHdrFormat)
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
    m_Shadow       = shadow.Build(device, layout);
    m_ShadowMasked = shadow.SetShaders(ShaderPath("shadow.vert.spv"), ShaderPath("shadow_mask.frag.spv"))
                         .SetDebugName("ShadowDepthMasked")
                         .Build(device, layout);

    const auto compute = [&](const char* spv, const char* name) {
        return CreateComputePipeline(device, layout, ShaderPath(spv), name);
    };
    m_BloomDown       = compute("bloom_downsample.comp.spv", "BloomDownsample");
    m_BloomUp         = compute("bloom_upsample.comp.spv", "BloomUpsample");
    m_Gtao            = compute("gtao.comp.spv", "Gtao");
    m_GtaoDenoise     = compute("gtao_denoise.comp.spv", "GtaoDenoise");
    m_Histogram       = compute("luminance_histogram.comp.spv", "LuminanceHistogram");
    m_ExposureAverage = compute("exposure_average.comp.spv", "ExposureAverage");

    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    m_LuminanceHistogram = Buffer(ctx, {.size = 256 * sizeof(std::uint32_t), .usage = kStorage,
                                        .debugName = "LuminanceHistogram"});
    m_ExposureState      = Buffer(ctx, {.size = 16, .usage = kStorage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        .debugName = "ExposureState"});
    for (Buffer& b : m_ExposureReadback)
        b = Buffer(ctx, {.size = 16, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .memory = MemoryUsage::Readback,
                         .debugName = "ExposureReadback"});
}

SceneRenderer::~SceneRenderer()
{
    ReleaseTargets();
    ReleaseShadowMap();
    for (auto& [format, pipeline] : m_Tonemap)
        m_Renderer.DeferRelease(std::move(pipeline));
    for (Pipeline* p : {&m_Mesh, &m_Sky, &m_Shadow, &m_ShadowMasked, &m_BloomDown, &m_BloomUp,
                        &m_Prepass, &m_Gtao, &m_GtaoDenoise, &m_Histogram, &m_ExposureAverage})
        m_Renderer.DeferRelease(std::move(*p));
    m_Renderer.DeferRelease(std::move(m_LuminanceHistogram));
    m_Renderer.DeferRelease(std::move(m_ExposureState));
    m_Renderer.DeferRelease(std::move(m_ExposureReadback));
}

void SceneRenderer::ReleaseTargets()
{
    if (!m_Hdr)
        return;
    // Frames in flight may still read them: slots and images go through the deferred queue.
    Renderer* r = &m_Renderer;
    std::vector<std::uint32_t> sampled = m_BloomSampled;
    std::vector<std::uint32_t> storage = m_BloomStorage;
    sampled.insert(sampled.end(), {m_DepthSlot, m_HdrSlot, m_NormalSlot, m_AoRawSampled, m_AoSampled});
    storage.insert(storage.end(), {m_AoRawStorage, m_AoStorage});
    r->DeferCall([r, sampled = std::move(sampled), storage = std::move(storage)] {
        BindlessRegistry& b = r->GetBindless();
        for (std::uint32_t s : sampled)
            b.RemoveSampledImage(s);
        for (std::uint32_t s : storage)
            b.RemoveStorageImage(s);
    });
    r->DeferRelease(std::move(m_BloomViews));
    for (Image* image : {&m_Depth, &m_Bloom, &m_Hdr, &m_Normals, &m_AoRaw, &m_Ao})
        r->DeferRelease(std::move(*image));
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

void SceneRenderer::CollectDrawItems(Scene& scene)
{
    m_DrawItems.clear();
    ModelHandle  lastHandle;
    const Model* lastModel = nullptr; // entities of one model are usually contiguous

    scene.GetRegistry().ViewOf<WorldTransform, MeshRenderer>().Each(
        [&](Entity, const WorldTransform& world, const MeshRenderer& renderer) {
            if (renderer.model != lastHandle) {
                lastHandle = renderer.model;
                lastModel  = m_Assets.Get(renderer.model); // nullptr while loading or after release
            }
            if (!lastModel || renderer.meshIndex >= lastModel->meshes.size())
                return;

            const glm::mat3 linear = glm::mat3(world.matrix);
            const DrawData  data{.model        = world.matrix,
                                 .normalMatrix = glm::mat4(glm::transpose(glm::inverse(linear)))};
            m_DrawItems.push_back({.model     = lastModel,
                                   .mesh      = &lastModel->meshes[renderer.meshIndex],
                                   .world     = world.matrix,
                                   .drawData  = m_Renderer.PushTransient(data, 16),
                                   // Mirrored transforms flip the winding (glTF: negative determinant).
                                   .frontFace = glm::determinant(linear) < 0.0f ? VK_FRONT_FACE_CLOCKWISE
                                                                                : VK_FRONT_FACE_COUNTER_CLOCKWISE});
        });
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
    const SkySettings&    sky = lighting.sky;

    const auto  now       = std::chrono::steady_clock::now();
    const float deltaTime = m_LastFrameTime ? std::chrono::duration<float>(now - *m_LastFrameTime).count() : 0.0f;
    m_LastFrameTime       = now;
    ReadExposure(frame.frameIndex); // this slot's fence was waited on in BeginFrame

    const VkExtent2D extent = output.extent;
    EnsureTargets(extent);
    EnsureShadowMap();
    GpuProfiler* profiler = &m_Renderer.Profiler();
    {
        GpuScope scope(profiler, cmd, "Environment");
        m_Environment.Update(cmd, sky); // compute, only when the sky changed
    }
    CollectDrawItems(scene);

    const std::uint32_t cascadeCount = shadows.enabled ? std::clamp(shadows.cascadeCount, 1u, kMaxCascades) : 0u;
    const auto          cascades     = ComputeCascades(camera, sky.sunDirection, shadows);

    const glm::mat4 viewProj = camera.projection * camera.view;
    FrameUniforms   uniforms{
          .viewProj       = viewProj,
          .view           = camera.view,
          .proj           = camera.projection,
          .invViewProj    = glm::inverse(viewProj),
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
          .aoInfo          = glm::uvec4(m_AoSampled, ao.enabled ? 1u : 0u, 0u, 0u)};
    for (std::uint32_t c = 0; c < kMaxCascades; ++c) {
        uniforms.cascadeViewProj[c] = cascades[c].viewProj;
        uniforms.cascadeSplits[c]   = cascades[c].splitFar;
        uniforms.cascadeTexel[c]    = cascades[c].texelWorldSize;
    }
    const VkDeviceAddress frameAddress = m_Renderer.PushTransient(uniforms);

    if (cascadeCount > 0) {
        GpuScope scope(profiler, cmd, "Shadows");
        RenderShadows(cmd, frameAddress, cascades, cascadeCount);
    }

    const Frustum frustum = Frustum::FromViewProjection(viewProj);
    {
        GpuScope scope(profiler, cmd, "Depth + normals");
        RenderPrepass(cmd, extent, frustum, frameAddress);
    }
    if (ao.enabled) {
        GpuScope scope(profiler, cmd, "GTAO");
        RenderAmbientOcclusion(cmd, extent, camera);
    }
    {
        GpuScope scope(profiler, cmd, "Lighting + sky");
        RenderMain(cmd, extent, frustum, frameAddress);
    }

    // HDR -> sampled by bloom (compute) and tone mapping (fragment).
    CmdImageBarrier(cmd, {.image     = m_Hdr.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
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

    // --- Tone mapping into the swapchain image ---
    const auto& bindless  = m_Renderer.GetBindless();
    GpuScope    tonemapScope(profiler, cmd, "Tone mapping");
    const auto  target = Attachment(output.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    BeginRendering(cmd, extent, &target, nullptr);
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    SetViewportScissor(cmd, extent);
    const TonemapPush tonemap{.state         = m_ExposureState.Address(),
                              .hdrTexture    = m_HdrSlot,
                              .tonemapper    = static_cast<std::uint32_t>(post.tonemapper),
                              .exposure      = post.exposure,
                              .bloomTexture  = m_BloomSampled.front(),
                              .bloomStrength = bloom ? post.bloomStrength : 0.0f,
                              .autoExposure  = post.autoExposure ? 1u : 0u,
                              .debugView     = static_cast<std::uint32_t>(post.debugView),
                              .debugTexture  = post.debugView == DebugView::Normals ? m_NormalSlot : m_AoSampled};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, TonemapPipeline(output.format).Handle());
    vkCmdPushConstants(cmd, bindless.PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(tonemap), &tonemap);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void SceneRenderer::RenderShadows(VkCommandBuffer cmd, VkDeviceAddress frameAddress,
                                  const std::array<Cascade, kMaxCascades>& cascades, std::uint32_t cascadeCount)
{
    const auto&            bindless   = m_Renderer.GetBindless();
    const VkPipelineLayout layout     = bindless.PipelineLayout();
    const std::uint32_t    resolution = m_ShadowMap.Extent().width;
    constexpr VkPipelineStageFlags2 kDepthStages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

    // Previous frames may still sample the maps (WAR): wait for their fragment shaders.
    CmdImageBarrier(cmd, {.image     = m_ShadowMap.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
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
        const Frustum frustum      = Frustum::FromViewProjection(cascades[c].viewProj, false);
        VkPipeline    bound        = VK_NULL_HANDLE;
        const Model*  boundIndices = nullptr;
        for (const DrawItem& item : m_DrawItems) {
            for (const Submesh& sm : item.mesh->submeshes) {
                if (!frustum.Intersects(TransformAabb({sm.boundsMin, sm.boundsMax}, item.world)))
                    continue;
                const bool       masked   = (item.model->materialFlags[sm.material] & kMaterialAlphaMask) != 0;
                const VkPipeline pipeline = masked ? m_ShadowMasked.Handle() : m_Shadow.Handle();
                if (pipeline != bound) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    bound = pipeline;
                }
                if (item.model != boundIndices) {
                    vkCmdBindIndexBuffer(cmd, item.model->indexBuffer.Handle(), 0, VK_INDEX_TYPE_UINT32);
                    boundIndices = item.model;
                }
                const MeshPush push{.frame         = frameAddress,
                                    .vertices      = item.model->vertexBuffer.Address(),
                                    .materials     = item.model->materialBuffer.Address(),
                                    .draw          = item.drawData,
                                    .materialIndex = sm.material,
                                    .cascade       = c};
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.firstIndex, sm.vertexOffset, 0);
                ++m_Stats.shadowDraws;
            }
        }
        vkCmdEndRendering(cmd);
    }

    CmdImageBarrier(cmd, {.image     = m_ShadowMap.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = kDepthStages,
                          .srcAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                          .aspect    = VK_IMAGE_ASPECT_DEPTH_BIT});
}

void SceneRenderer::DrawVisible(VkCommandBuffer cmd, const Frustum& frustum, VkDeviceAddress frameAddress,
                                bool countStats)
{
    // Same culling and draw order in the prepass and the lighting pass (depth EQUAL relies on it).
    const VkPipelineLayout layout     = m_Renderer.GetBindless().PipelineLayout();
    const Model*           boundModel = nullptr;
    VkCullModeFlags        cullMode   = VK_CULL_MODE_FLAG_BITS_MAX_ENUM;
    VkFrontFace            frontFace  = VK_FRONT_FACE_MAX_ENUM;

    for (const DrawItem& item : m_DrawItems) {
        for (const Submesh& sm : item.mesh->submeshes) {
            if (!frustum.Intersects(TransformAabb({sm.boundsMin, sm.boundsMax}, item.world))) {
                m_Stats.culled += countStats ? 1u : 0u;
                continue;
            }
            if (item.model != boundModel) {
                vkCmdBindIndexBuffer(cmd, item.model->indexBuffer.Handle(), 0, VK_INDEX_TYPE_UINT32);
                boundModel = item.model;
            }

            const bool doubleSided     = (item.model->materialFlags[sm.material] & kMaterialDoubleSided) != 0;
            const VkCullModeFlags cull = doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
            if (cull != cullMode) {
                vkCmdSetCullMode(cmd, cull);
                cullMode = cull;
            }
            if (item.frontFace != frontFace) {
                vkCmdSetFrontFace(cmd, item.frontFace);
                frontFace = item.frontFace;
            }

            const MeshPush push{.frame         = frameAddress,
                                .vertices      = item.model->vertexBuffer.Address(),
                                .materials     = item.model->materialBuffer.Address(),
                                .draw          = item.drawData,
                                .materialIndex = sm.material,
                                .cascade       = 0};
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.firstIndex, sm.vertexOffset, 0);

            if (countStats) {
                ++m_Stats.drawCalls;
                m_Stats.triangles += sm.indexCount / 3;
            }
        }
    }
}

void SceneRenderer::RenderPrepass(VkCommandBuffer cmd, VkExtent2D extent, const Frustum& frustum,
                                  VkDeviceAddress frameAddress)
{
    // Depth is shared by all frames in flight: earlier frames' tests, GTAO and denoise may still use it.
    constexpr VkPipelineStageFlags2 kDepthStages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    CmdImageBarrier(cmd, {.image     = m_Depth.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .srcStage  = kDepthStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .srcAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .dstStage  = kDepthStages,
                          .dstAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .aspect    = VK_IMAGE_ASPECT_DEPTH_BIT});

    // Normals are shared by all frames in flight: the previous frame's GTAO may still read them.
    CmdImageBarrier(cmd, {.image     = m_Normals.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});

    auto normals = Attachment(m_Normals.View(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR);
    normals.clearValue.color = {{0.0f, 0.0f, 1.0f, 0.0f}}; // sky: facing the camera
    auto depth = Attachment(m_Depth.View(), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR);
    depth.clearValue.depthStencil = {0.0f, 0}; // reverse-Z: far = 0

    BeginRendering(cmd, extent, &normals, &depth);
    m_Renderer.GetBindless().Bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    SetViewportScissor(cmd, extent);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Prepass.Handle());
    DrawVisible(cmd, frustum, frameAddress, false);
    vkCmdEndRendering(cmd);

    // Depth: read-only attachment for the lighting pass and sampled by GTAO from here on.
    const std::array<VkImageMemoryBarrier2, 2> barriers{
        MakeImageBarrier({.image     = m_Depth.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                          .srcStage  = kDepthStages,
                          .srcAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .dstStage  = kDepthStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                          .aspect    = VK_IMAGE_ASPECT_DEPTH_BIT}),
        MakeImageBarrier({.image     = m_Normals.Handle(),
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

void SceneRenderer::RenderMain(VkCommandBuffer cmd, VkExtent2D extent, const Frustum& frustum,
                               VkDeviceAddress frameAddress)
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

    // No color clear: the sky covers every pixel without geometry. Depth comes from the prepass.
    const auto hdr = Attachment(m_Hdr.View(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    auto depth     = Attachment(m_Depth.View(), VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD);
    depth.storeOp  = VK_ATTACHMENT_STORE_OP_NONE; // read-only: no store access at all

    BeginRendering(cmd, extent, &hdr, &depth);
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    SetViewportScissor(cmd, extent);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Mesh.Handle());
    DrawVisible(cmd, frustum, frameAddress, true);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Sky.Handle());
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(frameAddress), &frameAddress);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
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
        dispatch(m_BloomDown, {.src      = mip == 0 ? m_HdrSlot : m_BloomSampled[mip - 1],
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
                                  .hdr                  = m_HdrSlot,
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
