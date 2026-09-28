#include "Engine/Renderer/Vulkan/Image.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"

#include <cassert>
#include <utility>

namespace Engine {

Image::Image(const VulkanContext& ctx, const ImageDesc& desc)
    : m_Device(ctx.Device())
    , m_Allocator(ctx.Allocator())
    , m_Format(desc.format)
    , m_Extent(desc.extent)
    , m_MipLevels(desc.mipLevels)
    , m_ArrayLayers(desc.arrayLayers)
{
    assert(desc.format != VK_FORMAT_UNDEFINED && desc.usage != 0);

    const bool depth = IsDepthFormat(desc.format);
    m_Aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (HasStencilComponent(desc.format))
        m_Aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = desc.extent.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format        = desc.format;
    info.extent        = desc.extent;
    info.mipLevels     = desc.mipLevels;
    info.arrayLayers   = desc.arrayLayers;
    info.samples       = desc.samples;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = desc.usage;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (desc.viewType == VK_IMAGE_VIEW_TYPE_CUBE || desc.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (desc.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        alloc.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    VK_CHECK(vmaCreateImage(m_Allocator, &info, &alloc, &m_Image, &m_Allocation, nullptr));

    VkImageViewCreateInfo view{};
    view.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image    = m_Image;
    view.viewType = desc.viewType;
    view.format   = desc.format;
    // Sampling views of depth/stencil images must select a single aspect.
    view.subresourceRange = {depth ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT} : m_Aspect, 0,
                             desc.mipLevels, 0, desc.arrayLayers};
    VK_CHECK(vkCreateImageView(m_Device, &view, nullptr, &m_View));

    if (desc.debugName) {
        vmaSetAllocationName(m_Allocator, m_Allocation, desc.debugName);
        SetDebugName(m_Device, VK_OBJECT_TYPE_IMAGE, m_Image, desc.debugName);
        SetDebugName(m_Device, VK_OBJECT_TYPE_IMAGE_VIEW, m_View, desc.debugName);
    }
}

void Image::Release() noexcept
{
    if (m_View)
        vkDestroyImageView(m_Device, m_View, nullptr);
    if (m_Image)
        vmaDestroyImage(m_Allocator, m_Image, m_Allocation);
    m_View       = VK_NULL_HANDLE;
    m_Image      = VK_NULL_HANDLE;
    m_Allocation = VK_NULL_HANDLE;
}

void Image::MoveFrom(Image& o) noexcept
{
    m_Device      = o.m_Device;
    m_Allocator   = o.m_Allocator;
    m_Image       = std::exchange(o.m_Image, VK_NULL_HANDLE);
    m_View        = std::exchange(o.m_View, VK_NULL_HANDLE);
    m_Allocation  = std::exchange(o.m_Allocation, VK_NULL_HANDLE);
    m_Format      = o.m_Format;
    m_Extent      = o.m_Extent;
    m_MipLevels   = o.m_MipLevels;
    m_ArrayLayers = o.m_ArrayLayers;
    m_Aspect      = o.m_Aspect;
}

} // namespace Engine
