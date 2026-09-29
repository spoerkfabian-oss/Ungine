#include "Engine/Renderer/Vulkan/Swapchain.h"
#include "Renderer/Vulkan/VkbUtil.h"

namespace Engine {

Swapchain::Swapchain(const VulkanContext& ctx, VkExtent2D extent, const SwapchainDesc& desc, VkSwapchainKHR oldSwapchain)
    : m_Ctx(ctx), m_Desc(desc)
{
    Create(extent, oldSwapchain);
}

Swapchain::~Swapchain()
{
    DestroyViews();
    if (m_Swapchain.swapchain)
        vkb::destroy_swapchain(m_Swapchain);
}

void Swapchain::Recreate(VkExtent2D extent)
{
    const vkb::Swapchain old = m_Swapchain;
    DestroyViews();
    Create(extent, old.swapchain); // passing oldSwapchain lets the driver reuse resources
    vkb::destroy_swapchain(old);
}

void Swapchain::Create(VkExtent2D extent, VkSwapchainKHR oldSwapchain)
{
    vkb::SwapchainBuilder builder{m_Ctx.VkbDevice()};
    builder.set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
           .add_fallback_format({VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
           .set_desired_extent(extent.width, extent.height)
           .set_desired_min_image_count(vkb::SwapchainBuilder::TRIPLE_BUFFERING)
           .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
           .set_old_swapchain(oldSwapchain);

    if (m_Desc.vsync) {
        builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
    } else {
        builder.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
               .add_fallback_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR);
    }

    m_Swapchain = Expect(builder.build(), "Swapchain creation failed");
    m_Images    = Expect(m_Swapchain.get_images(), "Swapchain images");
    m_Views     = Expect(m_Swapchain.get_image_views(), "Swapchain image views");

    ENGINE_INFO("Swapchain {}x{}, {} images", m_Swapchain.extent.width, m_Swapchain.extent.height,
                m_Images.size());
}

void Swapchain::DestroyViews()
{
    if (!m_Views.empty())
        m_Swapchain.destroy_image_views(m_Views);
    m_Views.clear();
    m_Images.clear();
}

} // namespace Engine
