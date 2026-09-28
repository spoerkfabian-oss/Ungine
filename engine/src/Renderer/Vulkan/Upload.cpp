#include "Engine/Renderer/Vulkan/Upload.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"

#include <algorithm>
#include <bit>
#include <cassert>

namespace Engine {

namespace {
constexpr VkPipelineStageFlags2 kShaderStages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
} // namespace

UploadContext::UploadContext(const VulkanContext& ctx)
    : m_Ctx(ctx)
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = ctx.GraphicsQueue().family;
    VK_CHECK(vkCreateCommandPool(ctx.Device(), &poolInfo, nullptr, &m_Pool));

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool        = m_Pool;
    alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.Device(), &alloc, &m_Cmd));

    m_Fence = MakeFence(ctx.Device(), false);
}

UploadContext::~UploadContext()
{
    vkDestroyFence(m_Ctx.Device(), m_Fence, nullptr);
    vkDestroyCommandPool(m_Ctx.Device(), m_Pool, nullptr);
}

void UploadContext::ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record)
{
    const VkDevice dev = m_Ctx.Device();
    VK_CHECK(vkResetCommandPool(dev, m_Pool, 0));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(m_Cmd, &begin));
    record(m_Cmd);
    VK_CHECK(vkEndCommandBuffer(m_Cmd));

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = m_Cmd;

    VkSubmitInfo2 submit{};
    submit.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos    = &cmdInfo;
    VK_CHECK(vkQueueSubmit2(m_Ctx.GraphicsQueue().handle, 1, &submit, m_Fence));

    VK_CHECK(vkWaitForFences(dev, 1, &m_Fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(dev, 1, &m_Fence));
}

Buffer UploadContext::CreateBuffer(std::span<const std::byte> data, VkBufferUsageFlags usage, const char* debugName)
{
    assert(!data.empty());
    Buffer staging(m_Ctx, {.size = data.size(), .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           .memory = MemoryUsage::Upload, .debugName = "staging"});
    staging.Write(data.data(), data.size());

    Buffer buffer(m_Ctx, {.size = data.size(), .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          .memory = MemoryUsage::GpuOnly, .debugName = debugName});

    ImmediateSubmit([&](VkCommandBuffer cmd) {
        const VkBufferCopy region{0, 0, data.size()};
        vkCmdCopyBuffer(cmd, staging.Handle(), buffer.Handle(), 1, &region);

        // Make the copy visible to any later GPU read (separate submissions).
        VkMemoryBarrier2 barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo dep{};
        dep.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    return buffer;
}

Image UploadContext::CreateTexture2D(const TextureDesc& desc)
{
    assert(desc.pixels && desc.width > 0 && desc.height > 0);
    const VkDeviceSize byteSize = VkDeviceSize{desc.width} * desc.height * 4;

    std::uint32_t mipLevels = 1;
    if (desc.generateMips) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(m_Ctx.PhysicalDevice(), desc.format, &props);
        constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((props.optimalTilingFeatures & required) == required)
            mipLevels = std::bit_width(std::max(desc.width, desc.height)); // floor(log2(n)) + 1
        else
            ENGINE_WARN("Format {} can't be linearly blitted; skipping mip generation", static_cast<int>(desc.format));
    }

    Buffer staging(m_Ctx, {.size = byteSize, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           .memory = MemoryUsage::Upload, .debugName = "staging"});
    staging.Write(desc.pixels, byteSize);

    Image image(m_Ctx, {.extent    = {desc.width, desc.height, 1},
                        .format    = desc.format,
                        .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        .mipLevels = mipLevels,
                        .debugName = desc.debugName});

    ImmediateSubmit([&](VkCommandBuffer cmd) {
        CmdImageBarrier(cmd, {.image     = image.Handle(),
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .dstStage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                              .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT});

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent      = {desc.width, desc.height, 1};
        vkCmdCopyBufferToImage(cmd, staging.Handle(), image.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        // Mip chain: mip[i-1] (TRANSFER_SRC) --linear blit--> mip[i] (TRANSFER_DST)
        auto w = static_cast<std::int32_t>(desc.width);
        auto h = static_cast<std::int32_t>(desc.height);
        for (std::uint32_t i = 1; i < mipLevels; ++i) {
            CmdImageBarrier(cmd, {.image     = image.Handle(),
                                  .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  .srcStage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
                                  .baseMip   = i - 1,
                                  .mipCount  = 1});

            const std::int32_t nw = std::max(w / 2, 1);
            const std::int32_t nh = std::max(h / 2, 1);

            VkImageBlit2 blit{};
            blit.sType          = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
            blit.srcOffsets[1]  = {w, h, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            blit.dstOffsets[1]  = {nw, nh, 1};

            VkBlitImageInfo2 blitInfo{};
            blitInfo.sType          = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
            blitInfo.srcImage       = image.Handle();
            blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            blitInfo.dstImage       = image.Handle();
            blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            blitInfo.regionCount    = 1;
            blitInfo.pRegions       = &blit;
            blitInfo.filter         = VK_FILTER_LINEAR;
            vkCmdBlitImage2(cmd, &blitInfo);

            w = nw;
            h = nh;
        }

        // Final layouts: mips [0, n-1) are TRANSFER_SRC, the last one is TRANSFER_DST.
        if (mipLevels > 1) {
            CmdImageBarrier(cmd, {.image     = image.Handle(),
                                  .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  .srcStage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .srcAccess = VK_ACCESS_2_NONE, // only reads happened: execution dep suffices
                                  .dstStage  = kShaderStages,
                                  .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                  .baseMip   = 0,
                                  .mipCount  = mipLevels - 1});
        }
        CmdImageBarrier(cmd, {.image     = image.Handle(),
                              .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              .srcStage  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                              .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              .dstStage  = kShaderStages,
                              .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                              .baseMip   = mipLevels - 1,
                              .mipCount  = 1});
    });
    return image;
}

} // namespace Engine
