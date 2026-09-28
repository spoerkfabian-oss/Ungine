#include "Engine/Renderer/Environment.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"

#include <algorithm>
#include <array>
#include <bit>

namespace Engine {

namespace {
constexpr std::uint32_t kEnvSize          = 256;
constexpr std::uint32_t kIrradianceSize   = 32;
constexpr std::uint32_t kPrefilterSize    = 128;
constexpr std::uint32_t kPrefilterMips    = 5; // roughness 0, 0.25, 0.5, 0.75, 1
constexpr std::uint32_t kBrdfLutSize      = 256;
constexpr std::uint32_t kIrradianceSamples = 512;
constexpr std::uint32_t kPrefilterSamples  = 256;
constexpr std::uint32_t kBrdfSamples       = 512;
constexpr VkFormat      kFormat            = VK_FORMAT_R16G16B16A16_SFLOAT; // matches rgba16f storage bindings

// Mirrors IblPush in ibl_common.glsl.
struct IblPush {
    std::uint32_t dst         = 0;
    std::uint32_t size        = 0;
    std::uint32_t src         = 0;
    std::uint32_t srcSize     = 0;
    float         roughness   = 0.0f;
    std::uint32_t sampleCount = 0;
    std::uint32_t pad[2]      = {};
    glm::vec4     toSun{0.0f};
    glm::vec4     sunRadiance{0.0f};
    glm::vec4     skyParams{0.0f};
};
static_assert(sizeof(IblPush) == 80 && sizeof(IblPush) <= kPushConstantSize);

constexpr std::uint32_t GroupCount(std::uint32_t size) { return (size + 7) / 8; } // local_size 8x8

std::uint32_t MipCount(std::uint32_t size) { return static_cast<std::uint32_t>(std::bit_width(size)); }

Image MakeCube(const VulkanContext& ctx, std::uint32_t size, std::uint32_t mips, VkImageUsageFlags extraUsage,
               const char* name)
{
    return Image(ctx, {.extent      = {size, size, 1},
                       .format      = kFormat,
                       .usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | extraUsage,
                       .mipLevels   = mips,
                       .arrayLayers = 6,
                       .viewType    = VK_IMAGE_VIEW_TYPE_CUBE,
                       .debugName   = name});
}

void Barriers(VkCommandBuffer cmd, std::initializer_list<VkImageMemoryBarrier2> barriers)
{
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    dep.pImageMemoryBarriers    = barriers.begin();
    vkCmdPipelineBarrier2(cmd, &dep);
}

void Dispatch(VkCommandBuffer cmd, VkPipelineLayout layout, const Pipeline& pipeline, const IblPush& push,
              std::uint32_t size, std::uint32_t layers)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Handle());
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, GroupCount(size), GroupCount(size), layers);
}
} // namespace

Environment::Environment(Renderer& renderer)
    : m_Renderer(renderer)
{
    const VulkanContext&   ctx      = renderer.GetContext();
    const VkDevice         device   = ctx.Device();
    BindlessRegistry&      bindless = renderer.GetBindless();
    const VkPipelineLayout layout   = bindless.PipelineLayout();

    const auto compute = [&](const char* spv, const char* name) {
        return CreateComputePipeline(device, layout, ShaderPath(spv), name);
    };
    m_SkyPipeline        = compute("ibl_sky.comp.spv", "IblSky");
    m_IrradiancePipeline = compute("ibl_irradiance.comp.spv", "IblIrradiance");
    m_PrefilterPipeline  = compute("ibl_prefilter.comp.spv", "IblPrefilter");
    m_BrdfPipeline       = compute("ibl_brdf.comp.spv", "IblBrdfLut");

    // The environment mip chain is built with blits.
    m_EnvCube = MakeCube(ctx, kEnvSize, MipCount(kEnvSize),
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "IblEnvironment");
    m_Irradiance  = MakeCube(ctx, kIrradianceSize, 1, 0, "IblIrradiance");
    m_Prefiltered = MakeCube(ctx, kPrefilterSize, kPrefilterMips, 0, "IblPrefiltered");
    m_BrdfLut     = Image(ctx, {.extent    = {kBrdfLutSize, kBrdfLutSize, 1},
                                .format    = kFormat,
                                .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                .debugName = "IblBrdfLut"});

    const auto addArrayView = [&](const Image& image, std::uint32_t mip, const char* name) {
        m_StorageViews.push_back(image.CreateView(VK_IMAGE_VIEW_TYPE_2D_ARRAY, mip, 1, 0, 6, name));
        m_StorageArraySlots.push_back(bindless.AddStorageImageArray(m_StorageViews.back().Handle()));
        return m_StorageArraySlots.back();
    };
    m_EnvStorage           = addArrayView(m_EnvCube, 0, "IblEnvironmentMip0");
    m_IrradianceStorage    = addArrayView(m_Irradiance, 0, "IblIrradianceMip0");
    m_PrefilterStorageBase = static_cast<std::uint32_t>(m_StorageArraySlots.size());
    for (std::uint32_t mip = 0; mip < kPrefilterMips; ++mip)
        (void)addArrayView(m_Prefiltered, mip, "IblPrefilteredMip");

    m_BrdfLutStorage  = bindless.AddStorageImage(m_BrdfLut.View());
    m_EnvSlot         = bindless.AddCubeTexture(m_EnvCube.View());
    m_IrradianceSlot  = bindless.AddCubeTexture(m_Irradiance.View());
    m_PrefilteredSlot = bindless.AddCubeTexture(m_Prefiltered.View());
    m_BrdfLutSlot     = bindless.AddSampledImage(m_BrdfLut.View());
}

Environment::~Environment()
{
    Renderer* r = &m_Renderer;
    r->DeferCall([r, arrays = std::move(m_StorageArraySlots), storage = m_BrdfLutStorage,
                  cubes = std::array{m_EnvSlot, m_IrradianceSlot, m_PrefilteredSlot}, lut = m_BrdfLutSlot] {
        BindlessRegistry& b = r->GetBindless();
        for (std::uint32_t s : arrays)
            b.RemoveStorageImageArray(s);
        for (std::uint32_t s : cubes)
            b.RemoveCubeTexture(s);
        b.RemoveStorageImage(storage);
        b.RemoveSampledImage(lut);
    });
    r->DeferRelease(std::move(m_StorageViews));
    r->DeferRelease(std::move(m_EnvCube));
    r->DeferRelease(std::move(m_Irradiance));
    r->DeferRelease(std::move(m_Prefiltered));
    r->DeferRelease(std::move(m_BrdfLut));
    r->DeferRelease(std::move(m_SkyPipeline));
    r->DeferRelease(std::move(m_IrradiancePipeline));
    r->DeferRelease(std::move(m_PrefilterPipeline));
    r->DeferRelease(std::move(m_BrdfPipeline));
}

void Environment::Update(VkCommandBuffer cmd, const SkySettings& sky)
{
    const bool needLut   = !m_LutReady;
    const bool needCubes = !m_Generated || *m_Generated != sky;
    if (!needLut && !needCubes)
        return;

    m_Renderer.GetBindless().Bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    if (needLut)
        GenerateBrdfLut(cmd);
    if (needCubes)
        GenerateCubes(cmd, sky);
}

void Environment::GenerateBrdfLut(VkCommandBuffer cmd)
{
    constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    Barriers(cmd, {MakeImageBarrier({.image     = m_BrdfLut.Handle(),
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .dstStage  = kCompute,
                                     .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT})});

    Dispatch(cmd, m_Renderer.GetBindless().PipelineLayout(), m_BrdfPipeline,
             {.dst = m_BrdfLutStorage, .size = kBrdfLutSize, .sampleCount = kBrdfSamples}, kBrdfLutSize, 1);

    Barriers(cmd, {MakeImageBarrier({.image     = m_BrdfLut.Handle(),
                                     .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     .srcStage  = kCompute,
                                     .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                     .dstStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                     .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT})});
    m_LutReady = true;
}

void Environment::GenerateCubes(VkCommandBuffer cmd, const SkySettings& sky)
{
    constexpr VkPipelineStageFlags2 kCompute  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr VkPipelineStageFlags2 kTransfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    constexpr VkPipelineStageFlags2 kFragment = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    const VkPipelineLayout          layout    = m_Renderer.GetBindless().PipelineLayout();
    const std::uint32_t             envMips   = m_EnvCube.MipLevels();

    // 1) Environment: sky into mip 0. Earlier frames may still sample the previous contents
    //    (compute for env; fragment for irradiance/prefiltered) -> WAR dependencies, no access.
    Barriers(cmd, {MakeImageBarrier({.image     = m_EnvCube.Handle(),
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .srcStage  = kCompute,
                                     .dstStage  = kCompute | kTransfer,
                                     .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                                  VK_ACCESS_2_TRANSFER_WRITE_BIT})});

    const glm::vec3 toSun = -glm::normalize(sky.sunDirection);
    Dispatch(cmd, layout, m_SkyPipeline,
             {.dst         = m_EnvStorage,
              .size        = kEnvSize,
              .toSun       = glm::vec4(toSun, 0.0f),
              .sunRadiance = glm::vec4(sky.sunColor * sky.sunIntensity, 0.0f),
              .skyParams   = glm::vec4(sky.skyIntensity, 0.0f, 0.0f, 0.0f)},
             kEnvSize, 6);

    // 2) Mip chain by linear blits (all six faces at once), everything stays in GENERAL.
    auto w = static_cast<std::int32_t>(kEnvSize);
    for (std::uint32_t mip = 1; mip < envMips; ++mip) {
        const bool fromCompute = mip == 1;
        Barriers(cmd, {MakeImageBarrier({.image     = m_EnvCube.Handle(),
                                         .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                         .srcStage  = fromCompute ? kCompute : kTransfer,
                                         .srcAccess = fromCompute ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                                                  : VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                         .dstStage  = kTransfer,
                                         .dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
                                         .baseMip   = mip - 1,
                                         .mipCount  = 1})});
        const std::int32_t nw = std::max(w / 2, 1);

        VkImageBlit2 blit{};
        blit.sType          = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 6};
        blit.srcOffsets[1]  = {w, w, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 6};
        blit.dstOffsets[1]  = {nw, nw, 1};

        VkBlitImageInfo2 info{};
        info.sType          = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
        info.srcImage       = m_EnvCube.Handle();
        info.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        info.dstImage       = m_EnvCube.Handle();
        info.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        info.regionCount    = 1;
        info.pRegions       = &blit;
        info.filter         = VK_FILTER_LINEAR;
        vkCmdBlitImage2(cmd, &info);
        w = nw;
    }

    // 3) Environment -> sampled by the convolutions; outputs -> GENERAL for storage writes.
    Barriers(cmd, {MakeImageBarrier({.image     = m_EnvCube.Handle(),
                                     .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     .srcStage  = kTransfer,
                                     .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                     .dstStage  = kCompute,
                                     .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT}),
                   MakeImageBarrier({.image     = m_Irradiance.Handle(),
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .srcStage  = kFragment,
                                     .dstStage  = kCompute,
                                     .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT}),
                   MakeImageBarrier({.image     = m_Prefiltered.Handle(),
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .srcStage  = kFragment,
                                     .dstStage  = kCompute,
                                     .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT})});

    // 4) Convolutions.
    Dispatch(cmd, layout, m_IrradiancePipeline,
             {.dst = m_IrradianceStorage, .size = kIrradianceSize, .src = m_EnvSlot, .srcSize = kEnvSize,
              .sampleCount = kIrradianceSamples},
             kIrradianceSize, 6);
    for (std::uint32_t mip = 0; mip < kPrefilterMips; ++mip) {
        const std::uint32_t size = std::max(kPrefilterSize >> mip, 1u);
        Dispatch(cmd, layout, m_PrefilterPipeline,
                 {.dst         = m_StorageArraySlots[m_PrefilterStorageBase + mip],
                  .size        = size,
                  .src         = m_EnvSlot,
                  .srcSize     = kEnvSize,
                  .roughness   = static_cast<float>(mip) / static_cast<float>(kPrefilterMips - 1),
                  .sampleCount = kPrefilterSamples},
                 size, 6);
    }

    // 5) Ready for the fragment shaders.
    Barriers(cmd, {MakeImageBarrier({.image     = m_Irradiance.Handle(),
                                     .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     .srcStage  = kCompute,
                                     .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                     .dstStage  = kFragment,
                                     .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT}),
                   MakeImageBarrier({.image     = m_Prefiltered.Handle(),
                                     .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                                     .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     .srcStage  = kCompute,
                                     .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                     .dstStage  = kFragment,
                                     .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT})});
    m_Generated = sky;
}

} // namespace Engine
