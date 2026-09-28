#pragma once
#include "Engine/Renderer/Vulkan/VkCommon.h"

#include <cstdint>
#include <type_traits>

namespace Engine {

// Explicit sync2 image barrier; callers state exact stages/accesses (no ALL_COMMANDS shortcuts).
struct ImageBarrier {
    VkImage               image     = VK_NULL_HANDLE;
    VkImageLayout         oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout         newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 srcStage  = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        srcAccess = VK_ACCESS_2_NONE;
    VkPipelineStageFlags2 dstStage  = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        dstAccess = VK_ACCESS_2_NONE;
    VkImageAspectFlags    aspect    = VK_IMAGE_ASPECT_COLOR_BIT;
    std::uint32_t         baseMip   = 0;
    std::uint32_t         mipCount  = VK_REMAINING_MIP_LEVELS;
};

void CmdImageBarrier(VkCommandBuffer cmd, const ImageBarrier& barrier);

// Flipped viewport (negative height): +Y is up in NDC, like OpenGL/glTF conventions.
void SetViewportScissor(VkCommandBuffer cmd, VkExtent2D extent);

[[nodiscard]] VkSemaphore MakeSemaphore(VkDevice device);
[[nodiscard]] VkFence     MakeFence(VkDevice device, bool signaled);

[[nodiscard]] bool IsDepthFormat(VkFormat format);
[[nodiscard]] bool HasStencilComponent(VkFormat format);

// No-op unless VK_EXT_debug_utils is active (validation builds). Names show up in RenderDoc.
void SetDebugNameRaw(VkDevice device, VkObjectType type, std::uint64_t handle, const char* name);

template <class Handle>
void SetDebugName(VkDevice device, VkObjectType type, Handle handle, const char* name)
{
    if constexpr (std::is_pointer_v<Handle>)
        SetDebugNameRaw(device, type, reinterpret_cast<std::uint64_t>(handle), name);
    else
        SetDebugNameRaw(device, type, static_cast<std::uint64_t>(handle), name);
}

} // namespace Engine
