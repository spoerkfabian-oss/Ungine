#include "GpuCulling.h"
#include "Engine/Renderer/GeometryPool.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace Engine {

namespace {
constexpr std::uint32_t kGroupSize     = 64; // gpu_cull.comp
constexpr std::uint32_t kStatCount     = 10;
constexpr std::uint32_t kCommandStride = sizeof(VkDrawIndexedIndirectCommand);
static_assert(kCommandStride == 20);

struct CullPush { // mirrors CullPush in gpu_cull.comp
    VkDeviceAddress data;
    std::uint32_t   phase;
    std::uint32_t   firstView;
};

struct HiZPush { // mirrors HiZPush in hiz.comp
    VkDeviceAddress src;
    VkDeviceAddress dst;
    glm::uvec2      srcSize;
    glm::uvec2      dstSize;
    std::uint32_t   depthSlot;
    std::uint32_t   fromDepth;
};
static_assert(sizeof(HiZPush) <= kPushConstantSize);

void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
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

constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags2        kRW      = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
// Consumers of the culling output: indirect commands / counts and the visible lists (vertex shaders).
constexpr VkPipelineStageFlags2 kDrawStages = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
constexpr VkAccessFlags2        kDrawReads  = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

// Contents are rebuilt every frame: a larger buffer replaces the old one without copying.
void Ensure(Renderer& renderer, Buffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage, const char* name)
{
    bytes = std::max<VkDeviceSize>(bytes, 16);
    if (buffer && buffer.Size() >= bytes)
        return;
    const VkDeviceSize size = std::max(bytes, buffer ? buffer.Size() + buffer.Size() / 2 : VkDeviceSize{0});
    if (buffer)
        renderer.DeferRelease(std::move(buffer));
    buffer = Buffer(renderer.GetContext(), {.size      = size,
                                            .usage     = usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            .debugName = name});
}
} // namespace

GpuCulling::GpuCulling(Renderer& renderer) : m_Renderer(renderer)
{
    const VkDevice         device = renderer.GetContext().Device();
    const VkPipelineLayout layout = renderer.GetBindless().PipelineLayout();
    m_Cull     = CreateComputePipeline(device, layout, ShaderPath("gpu_cull.comp.spv"), "GpuCull");
    m_HiZBuild = CreateComputePipeline(device, layout, ShaderPath("hiz.comp.spv"), "HiZBuild");
    for (Buffer& b : m_Readback)
        b = Buffer(renderer.GetContext(), {.size      = kStatCount * sizeof(std::uint32_t),
                                           .usage     = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           .memory    = MemoryUsage::Readback,
                                           .debugName = "CullStatsReadback"});
    m_StatsBuffer = Buffer(renderer.GetContext(), {.size      = kStatCount * sizeof(std::uint32_t),
                                                   .usage     = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                   .debugName = "CullStats"});
}

void GpuCulling::RebuildPipelines()
{
    const VkDevice         device = m_Renderer.GetContext().Device();
    const VkPipelineLayout layout = m_Renderer.GetBindless().PipelineLayout();
    Pipeline cull = CreateComputePipeline(device, layout, ShaderPath("gpu_cull.comp.spv"), "GpuCull");
    Pipeline hiz  = CreateComputePipeline(device, layout, ShaderPath("hiz.comp.spv"), "HiZBuild");
    m_Renderer.DeferRelease(std::move(m_Cull));
    m_Renderer.DeferRelease(std::move(m_HiZBuild));
    m_Cull     = std::move(cull);
    m_HiZBuild = std::move(hiz);
}

GpuCulling::~GpuCulling()
{
    m_Renderer.DeferRelease(std::move(m_Cull));
    m_Renderer.DeferRelease(std::move(m_HiZBuild));
    for (Buffer* b : {&m_Visible, &m_Counters, &m_Commands, &m_DrawCounts, &m_StatsBuffer, &m_HiZ})
        if (*b)
            m_Renderer.DeferRelease(std::move(*b));
    m_Renderer.DeferRelease(std::move(m_Readback));
}

void GpuCulling::ReadStats(std::uint32_t frameIndex)
{
    if (!std::exchange(m_ReadbackValid[frameIndex], false))
        return;
    Buffer& readback = m_Readback[frameIndex];
    readback.Invalidate(0, VK_WHOLE_SIZE);
    std::uint32_t values[kStatCount];
    std::memcpy(values, readback.Mapped(), sizeof(values));
    m_Stats = {values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8], values[9]};
}

void GpuCulling::Dispatch(VkCommandBuffer cmd, std::uint32_t phase, std::uint32_t firstView, std::uint32_t viewCount,
                          std::uint32_t threads)
{
    const CullPush push{.data = m_CullData, .phase = phase, .firstView = firstView};
    const std::uint32_t groups = (threads + kGroupSize - 1) / kGroupSize;
    assert(groups <= 65535 && "More draw records than one dispatch covers");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Cull.Handle());
    vkCmdPushConstants(cmd, m_Renderer.GetBindless().PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, std::max(groups, 1u), viewCount, 1);
}

void GpuCulling::CullEarly(VkCommandBuffer cmd, const GpuScene& scene, std::span<const GpuCullView> views,
                           VkExtent2D depthExtent, const GpuLodParams& lod)
{
    m_IndirectCalls = 0;
    m_ViewCount     = 0;
    m_BatchCapacity = scene.BatchCount();
    m_DrawCapacity  = scene.DrawCapacity();
    if (views.empty() || m_BatchCapacity == 0 || m_DrawCapacity == 0 || scene.LiveDraws() == 0)
        return;
    m_ViewCount = static_cast<std::uint32_t>(views.size());

    // Per view: a visible list (one slice per batch), batch counters, 4 command buckets.
    const std::uint32_t listSize = scene.VisibleCapacity();
    m_Views.assign(views.begin(), views.end());
    m_LateViews = 1;
    while (kCameraLateView + m_LateViews < m_ViewCount && (m_Views[kCameraLateView + m_LateViews].flags & kCullViewShadowOcclusion) != 0)
        ++m_LateViews;
    for (std::uint32_t v = 0; v < m_ViewCount; ++v) {
        m_Views[v].listBase    = v * listSize;
        m_Views[v].counterBase = v * m_BatchCapacity;
        m_Views[v].commandBase = v * kCameraBuckets * m_BatchCapacity;
    }
    constexpr VkBufferUsageFlags kIndirect = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    Ensure(m_Renderer, m_Visible, VkDeviceSize{m_ViewCount} * listSize * 4, 0, "CullVisible");
    Ensure(m_Renderer, m_Counters, VkDeviceSize{m_ViewCount} * m_BatchCapacity * 4, 0, "CullCounters");
    Ensure(m_Renderer, m_Commands, VkDeviceSize{m_ViewCount} * kCameraBuckets * m_BatchCapacity * kCommandStride,
           kIndirect, "CullCommands");
    Ensure(m_Renderer, m_DrawCounts, VkDeviceSize{m_ViewCount} * kCameraBuckets * 4, kIndirect, "CullDrawCounts");

    const VkDeviceAddress viewAddress = [&] {
        const TransientAllocation a = m_Renderer.AllocateTransient(m_Views.size() * sizeof(GpuCullView), 16);
        std::memcpy(a.cpu, m_Views.data(), m_Views.size() * sizeof(GpuCullView));
        return a.gpu;
    }();
    m_Data = {.views         = viewAddress,
                        .draws         = scene.DrawAddress(),
                        .instances     = scene.InstanceAddress(),
                        .submeshes     = m_Renderer.Geometry().Address(GeometryKind::Submeshes),
                        .batches       = scene.BatchAddress(),
                        .counters      = m_Counters.Address(),
                        .visible       = m_Visible.Address(),
                        .visibility    = scene.VisibilityAddress(),
                        .drawCounts    = m_DrawCounts.Address(),
                        .commands      = m_Commands.Address(),
                        .hiz           = m_HiZ ? m_HiZ.Address() : m_StatsBuffer.Address(), // unused without levels
                        .stats         = m_StatsBuffer.Address(),
                        .drawCount     = m_DrawCapacity,
                        .batchCount    = m_BatchCapacity,
                        .batchCapacity = m_BatchCapacity,
                        .pad           = 0,
                        .hizInfo       = glm::uvec4(m_HiZSize, m_HiZLevels, 0u),
                        .depthSize     = glm::uvec4(depthExtent.width, depthExtent.height, 0u, 0u),
                        .lodCamera     = lod.camera,
                        .lodInfo       = glm::uvec4(lod.forced, 0u, 0u, 0u),
                        .shadowRegionMin = glm::vec4(lod.shadowRegionMin, 0.0f),
                        .shadowRegionMax = glm::vec4(lod.shadowRegionMax, 0.0f)};
    m_CullData = m_Renderer.PushTransient(m_Data, 16);

    // Earlier frames: indirect reads, vertex shader reads of the lists, culling (WAR / WAW), stats copy.
    Barrier(cmd, kDrawStages | kCompute | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | kCompute, VK_ACCESS_2_TRANSFER_WRITE_BIT | kRW);
    vkCmdFillBuffer(cmd, m_Counters.Handle(), 0, VkDeviceSize{m_ViewCount} * m_BatchCapacity * 4, 0);
    vkCmdFillBuffer(cmd, m_DrawCounts.Handle(), 0, VkDeviceSize{m_ViewCount} * kCameraBuckets * 4, 0);
    vkCmdFillBuffer(cmd, m_StatsBuffer.Handle(), 0, VK_WHOLE_SIZE, 0);
    Barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kCompute, kRW);

    m_Renderer.GetBindless().Bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    Dispatch(cmd, 0, 0, m_ViewCount, m_DrawCapacity); // every view but the late one (skipped in the shader)
    Barrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, kRW);
    Dispatch(cmd, 2, 0, m_ViewCount, m_BatchCapacity);
    Barrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kDrawStages | kCompute, kDrawReads | kRW);
}

void GpuCulling::BuildHiZ(VkCommandBuffer cmd, std::uint32_t depthSlot, VkExtent2D extent, bool keep)
{
    const bool sameSize = m_HiZExtent.width == extent.width && m_HiZExtent.height == extent.height;
    if (keep && m_HiZLevels > 0 && sameSize)
        return;

    // Level 0 = half resolution (rounded up), then down to 1 x 1.
    std::vector<glm::uvec2> sizes{glm::uvec2((extent.width + 1) / 2, (extent.height + 1) / 2)};
    while (sizes.back().x > 1 || sizes.back().y > 1)
        sizes.push_back(glm::max((sizes.back() + 1u) / 2u, glm::uvec2(1)));
    VkDeviceSize total = 0;
    for (const glm::uvec2& s : sizes)
        total += VkDeviceSize{s.x} * s.y;
    Ensure(m_Renderer, m_HiZ, total * sizeof(float), 0, "HiZ");
    m_HiZSize   = sizes.front();
    m_HiZLevels = static_cast<std::uint32_t>(sizes.size());
    m_HiZExtent = extent;

    // Earlier frames' late culling (compute) and the Hi-Z debug view (fragment) read it (WAR).
    Barrier(cmd, kCompute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE, kCompute,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    m_Renderer.GetBindless().Bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_HiZBuild.Handle());
    VkDeviceSize offset = 0;
    for (std::size_t level = 0; level < sizes.size(); ++level) {
        const VkDeviceSize previous = level > 0 ? offset - VkDeviceSize{sizes[level - 1].x} * sizes[level - 1].y : 0;
        const HiZPush push{.src       = m_HiZ.Address() + previous * sizeof(float),
                           .dst       = m_HiZ.Address() + offset * sizeof(float),
                           .srcSize   = level == 0 ? glm::uvec2(extent.width, extent.height) : sizes[level - 1],
                           .dstSize   = sizes[level],
                           .depthSlot = depthSlot,
                           .fromDepth = level == 0 ? 1u : 0u};
        vkCmdPushConstants(cmd, m_Renderer.GetBindless().PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (sizes[level].x + 7) / 8, (sizes[level].y + 7) / 8, 1);
        Barrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        offset += VkDeviceSize{sizes[level].x} * sizes[level].y;
    }
}

void GpuCulling::CullLate(VkCommandBuffer cmd)
{
    if (m_ViewCount <= kCameraLateView)
        return;
    // The Hi-Z may have been (re)created since CullEarly: fresh CullData with its address and size.
    m_Data.hiz     = m_HiZ ? m_HiZ.Address() : m_StatsBuffer.Address();
    m_Data.hizInfo = glm::uvec4(m_HiZSize, m_HiZLevels, 0u);
    m_CullData     = m_Renderer.PushTransient(m_Data, 16);
    m_Renderer.GetBindless().Bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    Dispatch(cmd, 1, kCameraLateView, m_LateViews, m_DrawCapacity);
    Barrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCompute, kRW);
    Dispatch(cmd, 2, kCameraLateView, m_LateViews, m_BatchCapacity);
    Barrier(cmd, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kDrawStages, kDrawReads);
}

void GpuCulling::CopyStats(VkCommandBuffer cmd, std::uint32_t frameIndex)
{
    if (m_ViewCount == 0)
        return;
    // + WAW with the copy into this readback buffer kFramesInFlight frames ago (its host read was
    // fenced, but sync validation only follows device-side dependencies).
    Barrier(cmd, kCompute | VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferCopy region{0, 0, kStatCount * sizeof(std::uint32_t)};
    vkCmdCopyBuffer(cmd, m_StatsBuffer.Handle(), m_Readback[frameIndex].Handle(), 1, &region);
    Barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
    m_ReadbackValid[frameIndex] = true;
}

void GpuCulling::RecordDraw(VkCommandBuffer cmd, std::uint32_t view, std::uint32_t bucket)
{
    const GpuCullView& v = m_Views[view];
    vkCmdDrawIndexedIndirectCount(cmd, m_Commands.Handle(),
                                  VkDeviceSize{v.commandBase + bucket * m_BatchCapacity} * kCommandStride,
                                  m_DrawCounts.Handle(), VkDeviceSize{view * kCameraBuckets + bucket} * 4,
                                  m_BatchCapacity, kCommandStride);
    ++m_IndirectCalls;
}

} // namespace Engine
