#pragma once
#include "Engine/Renderer/Vulkan/VulkanContext.h"

#include <cstdint>
#include <utility>

namespace Engine {

struct ImageDesc {
    VkExtent3D            extent      = {1, 1, 1};  // depth > 1 => 3D image
    VkFormat              format      = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags     usage       = 0;
    std::uint32_t         mipLevels   = 1;
    std::uint32_t         arrayLayers = 1;          // CSM cascades, cubemap faces
    VkImageViewType       viewType    = VK_IMAGE_VIEW_TYPE_2D;
    VkSampleCountFlagBits samples     = VK_SAMPLE_COUNT_1_BIT;
    const char*           debugName   = nullptr;
};

// Move-only RAII view onto part of an Image (e.g. one mip of a cube map as a 2D array).
class ImageView {
public:
    ImageView() = default;
    ImageView(VkDevice device, VkImageView view) : m_Device(device), m_View(view) {}
    ~ImageView() { Release(); }

    ImageView(ImageView&& o) noexcept : m_Device(o.m_Device), m_View(std::exchange(o.m_View, VK_NULL_HANDLE)) {}
    ImageView& operator=(ImageView&& o) noexcept
    {
        if (this != &o) {
            Release();
            m_Device = o.m_Device;
            m_View   = std::exchange(o.m_View, VK_NULL_HANDLE);
        }
        return *this;
    }
    ImageView(const ImageView&)            = delete;
    ImageView& operator=(const ImageView&) = delete;

    [[nodiscard]] VkImageView Handle() const { return m_View; }

private:
    void Release() noexcept
    {
        if (m_View)
            vkDestroyImageView(m_Device, m_View, nullptr);
        m_View = VK_NULL_HANDLE;
    }

    VkDevice    m_Device = VK_NULL_HANDLE;
    VkImageView m_View   = VK_NULL_HANDLE;
};

// Move-only RAII image + default view covering all mips/layers.
// Render targets (attachment usage) get dedicated allocations.
class Image {
public:
    Image() = default;
    Image(const VulkanContext& ctx, const ImageDesc& desc);
    ~Image() { Release(); }

    Image(Image&& other) noexcept { MoveFrom(other); }
    Image& operator=(Image&& other) noexcept
    {
        if (this != &other) {
            Release();
            MoveFrom(other);
        }
        return *this;
    }
    Image(const Image&)            = delete;
    Image& operator=(const Image&) = delete;

    [[nodiscard]] VkImage            Handle()      const { return m_Image; }
    [[nodiscard]] VkImageView        View()        const { return m_View; }
    [[nodiscard]] VkFormat           Format()      const { return m_Format; }
    [[nodiscard]] VkExtent3D         Extent()      const { return m_Extent; }
    [[nodiscard]] VkExtent2D         Extent2D()    const { return {m_Extent.width, m_Extent.height}; }
    [[nodiscard]] std::uint32_t      MipLevels()   const { return m_MipLevels; }
    [[nodiscard]] std::uint32_t      ArrayLayers() const { return m_ArrayLayers; }
    [[nodiscard]] VkImageAspectFlags Aspect()      const { return m_Aspect; } // full aspect, for barriers
    [[nodiscard]] explicit operator bool()         const { return m_Image != VK_NULL_HANDLE; }

    // Additional view; destroy it (or hand it to DeferRelease) before the image.
    [[nodiscard]] ImageView CreateView(VkImageViewType type, std::uint32_t baseMip, std::uint32_t mipCount,
                                       std::uint32_t baseLayer, std::uint32_t layerCount,
                                       const char* debugName = nullptr) const;

private:
    void Release() noexcept;
    void MoveFrom(Image& other) noexcept;

    VkDevice           m_Device      = VK_NULL_HANDLE;
    VmaAllocator       m_Allocator   = VK_NULL_HANDLE;
    VkImage            m_Image       = VK_NULL_HANDLE;
    VkImageView        m_View        = VK_NULL_HANDLE;
    VmaAllocation      m_Allocation  = VK_NULL_HANDLE;
    VkFormat           m_Format      = VK_FORMAT_UNDEFINED;
    VkExtent3D         m_Extent      = {0, 0, 0};
    std::uint32_t      m_MipLevels   = 0;
    std::uint32_t      m_ArrayLayers = 0;
    VkImageAspectFlags m_Aspect      = 0;
};

} // namespace Engine
