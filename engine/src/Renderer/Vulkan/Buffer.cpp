#include "Engine/Renderer/Vulkan/Buffer.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>

namespace Engine {

Buffer::Buffer(const VulkanContext& ctx, const BufferDesc& desc)
    : m_Allocator(ctx.Allocator()), m_Size(desc.size)
{
    assert(desc.size > 0 && "Zero-sized buffer");

    VkBufferCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size        = desc.size;
    info.usage       = desc.usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    const std::uint32_t families[] = {ctx.GraphicsQueue().family, ctx.TransferQueue().family};
    if (desc.concurrent && families[0] != families[1]) {
        info.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices   = families;
    }

    VmaAllocationCreateInfo alloc{};
    switch (desc.memory) {
    case MemoryUsage::GpuOnly:
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    case MemoryUsage::Upload:
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::Readback:
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }

    VmaAllocationInfo allocInfo{};
    VK_CHECK(vmaCreateBuffer(m_Allocator, &info, &alloc, &m_Buffer, &m_Allocation, &allocInfo));
    m_Mapped = allocInfo.pMappedData;

    VkBufferDeviceAddressInfo addressInfo{};
    addressInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = m_Buffer;
    m_Address          = vkGetBufferDeviceAddress(ctx.Device(), &addressInfo);

    if (desc.debugName) {
        vmaSetAllocationName(m_Allocator, m_Allocation, desc.debugName);
        SetDebugName(ctx.Device(), VK_OBJECT_TYPE_BUFFER, m_Buffer, desc.debugName);
    }
}

void Buffer::Write(const void* data, VkDeviceSize size, VkDeviceSize offset)
{
    assert(m_Mapped && "Buffer is not host-visible");
    assert(offset + size <= m_Size);
    std::memcpy(static_cast<std::byte*>(m_Mapped) + offset, data, static_cast<std::size_t>(size));
    VK_CHECK(vmaFlushAllocation(m_Allocator, m_Allocation, offset, size));
}

void Buffer::Invalidate(VkDeviceSize offset, VkDeviceSize size)
{
    VK_CHECK(vmaInvalidateAllocation(m_Allocator, m_Allocation, offset, size));
}

void Buffer::Flush(VkDeviceSize offset, VkDeviceSize size)
{
    VK_CHECK(vmaFlushAllocation(m_Allocator, m_Allocation, offset, size));
}

void Buffer::Release() noexcept
{
    if (m_Buffer)
        vmaDestroyBuffer(m_Allocator, m_Buffer, m_Allocation);
    m_Buffer     = VK_NULL_HANDLE;
    m_Allocation = VK_NULL_HANDLE;
    m_Mapped     = nullptr;
    m_Address    = 0;
    m_Size       = 0;
}

void Buffer::MoveFrom(Buffer& o) noexcept
{
    m_Allocator  = o.m_Allocator;
    m_Buffer     = std::exchange(o.m_Buffer, VK_NULL_HANDLE);
    m_Allocation = std::exchange(o.m_Allocation, VK_NULL_HANDLE);
    m_Size       = std::exchange(o.m_Size, 0);
    m_Address    = std::exchange(o.m_Address, 0);
    m_Mapped     = std::exchange(o.m_Mapped, nullptr);
}

} // namespace Engine
