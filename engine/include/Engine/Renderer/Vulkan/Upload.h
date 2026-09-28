#pragma once
#include "Engine/Renderer/Vulkan/Buffer.h"
#include "Engine/Renderer/Vulkan/Image.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace Engine {

struct TextureDesc {
    const void*   pixels       = nullptr; // tightly packed, 4 bytes per pixel
    std::uint32_t width        = 0;
    std::uint32_t height       = 0;
    VkFormat      format       = VK_FORMAT_R8G8B8A8_SRGB; // use *_UNORM for normal/ORM maps
    bool          generateMips = true;
    const char*   debugName    = nullptr;
};

// Blocking uploads on the graphics queue. Main thread only.
// (The async resource manager will move this to the transfer queue + timeline semaphores.)
class UploadContext {
public:
    explicit UploadContext(const VulkanContext& ctx);
    ~UploadContext();

    UploadContext(const UploadContext&)            = delete;
    UploadContext& operator=(const UploadContext&) = delete;

    void ImmediateSubmit(const std::function<void(VkCommandBuffer)>& record);

    [[nodiscard]] Buffer CreateBuffer(std::span<const std::byte> data, VkBufferUsageFlags usage,
                                      const char* debugName = nullptr);
    [[nodiscard]] Image  CreateTexture2D(const TextureDesc& desc);

private:
    const VulkanContext& m_Ctx;
    VkCommandPool        m_Pool  = VK_NULL_HANDLE;
    VkCommandBuffer      m_Cmd   = VK_NULL_HANDLE;
    VkFence              m_Fence = VK_NULL_HANDLE;
};

} // namespace Engine
