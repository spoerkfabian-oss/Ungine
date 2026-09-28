#include "Engine/Renderer/Vulkan/VkUtils.h"

namespace Engine {

void CmdImageBarrier(VkCommandBuffer cmd, const ImageBarrier& b)
{
    VkImageMemoryBarrier2 barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask        = b.srcStage;
    barrier.srcAccessMask       = b.srcAccess;
    barrier.dstStageMask        = b.dstStage;
    barrier.dstAccessMask       = b.dstAccess;
    barrier.oldLayout           = b.oldLayout;
    barrier.newLayout           = b.newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = b.image;
    barrier.subresourceRange    = {b.aspect, b.baseMip, b.mipCount, 0, VK_REMAINING_ARRAY_LAYERS};

    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkSemaphore MakeSemaphore(VkDevice device)
{
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore sem = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSemaphore(device, &info, nullptr, &sem));
    return sem;
}

VkFence MakeFence(VkDevice device, bool signaled)
{
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    info.flags = signaled ? VkFenceCreateFlags{VK_FENCE_CREATE_SIGNALED_BIT} : VkFenceCreateFlags{0};
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &info, nullptr, &fence));
    return fence;
}

void SetViewportScissor(VkCommandBuffer cmd, VkExtent2D extent)
{
    const VkViewport viewport{0.0f, static_cast<float>(extent.height), static_cast<float>(extent.width),
                              -static_cast<float>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

bool IsDepthFormat(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;
    default:                           return false;
    }
}

bool HasStencilComponent(VkFormat format)
{
    return format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_S8_UINT;
}

void SetDebugNameRaw(VkDevice device, VkObjectType type, std::uint64_t handle, const char* name)
{
    if (!name || !vkSetDebugUtilsObjectNameEXT)
        return;
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType   = type;
    info.objectHandle = handle;
    info.pObjectName  = name;
    vkSetDebugUtilsObjectNameEXT(device, &info);
}

} // namespace Engine
