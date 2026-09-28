#pragma once
#include "Engine/Renderer/Vulkan/VulkanContext.h"

#include <cstdint>
#include <vector>

namespace Engine {

struct SwapchainDesc {
    bool vsync = true; // FIFO; otherwise MAILBOX -> IMMEDIATE -> FIFO
};

class Swapchain {
public:
    Swapchain(const VulkanContext& ctx, VkExtent2D extent, const SwapchainDesc& desc);
    ~Swapchain();

    Swapchain(const Swapchain&)            = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Caller guarantees the device is idle w.r.t. the old images.
    void Recreate(VkExtent2D extent);

    [[nodiscard]] VkSwapchainKHR Handle()        const { return m_Swapchain.swapchain; }
    [[nodiscard]] VkFormat       Format()        const { return m_Swapchain.image_format; }
    [[nodiscard]] VkExtent2D     Extent()        const { return m_Swapchain.extent; }
    [[nodiscard]] std::uint32_t  ImageCount()    const { return static_cast<std::uint32_t>(m_Images.size()); }
    [[nodiscard]] VkImage        Image(std::uint32_t i) const { return m_Images[i]; }
    [[nodiscard]] VkImageView    View(std::uint32_t i)  const { return m_Views[i]; }

private:
    void Create(VkExtent2D extent, VkSwapchainKHR oldSwapchain);
    void DestroyViews();

    const VulkanContext&     m_Ctx;
    SwapchainDesc            m_Desc;
    vkb::Swapchain           m_Swapchain{};
    std::vector<VkImage>     m_Images;
    std::vector<VkImageView> m_Views;
};

} // namespace Engine
