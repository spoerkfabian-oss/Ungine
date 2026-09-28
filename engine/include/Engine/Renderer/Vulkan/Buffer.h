#pragma once
#include "Engine/Renderer/Vulkan/VulkanContext.h"

namespace Engine {

enum class MemoryUsage {
    GpuOnly,  // device-local; fill via UploadQueue
    Upload,   // host-visible, persistently mapped, sequential writes (staging, per-frame UBO/SSBO)
    Readback, // host-visible, persistently mapped, random reads (GPU -> CPU)
};

struct BufferDesc {
    VkDeviceSize       size      = 0;
    VkBufferUsageFlags usage     = 0;
    MemoryUsage        memory    = MemoryUsage::GpuOnly;
    const char*        debugName = nullptr;
};

// Move-only RAII buffer. Every buffer gets a device address (vertex pulling / BDA).
// Destroying while the GPU may still use it: hand it to Renderer::DeferRelease.
class Buffer {
public:
    Buffer() = default;
    Buffer(const VulkanContext& ctx, const BufferDesc& desc);
    ~Buffer() { Release(); }

    Buffer(Buffer&& other) noexcept { MoveFrom(other); }
    Buffer& operator=(Buffer&& other) noexcept
    {
        if (this != &other) {
            Release();
            MoveFrom(other);
        }
        return *this;
    }
    Buffer(const Buffer&)            = delete;
    Buffer& operator=(const Buffer&) = delete;

    // Host-visible buffers only. Flushes (no-op on HOST_COHERENT memory).
    void Write(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);
    // For callers writing through Mapped() directly.
    void Flush(VkDeviceSize offset, VkDeviceSize size);
    // Before reading GPU writes through Mapped() (no-op on HOST_COHERENT memory).
    void Invalidate(VkDeviceSize offset, VkDeviceSize size);

    [[nodiscard]] VkBuffer        Handle()  const { return m_Buffer; }
    [[nodiscard]] VkDeviceSize    Size()    const { return m_Size; }
    [[nodiscard]] VkDeviceAddress Address() const { return m_Address; }
    [[nodiscard]] void*           Mapped()  const { return m_Mapped; }
    [[nodiscard]] explicit operator bool()  const { return m_Buffer != VK_NULL_HANDLE; }

private:
    void Release() noexcept;
    void MoveFrom(Buffer& other) noexcept;

    VmaAllocator    m_Allocator  = VK_NULL_HANDLE;
    VkBuffer        m_Buffer     = VK_NULL_HANDLE;
    VmaAllocation   m_Allocation = VK_NULL_HANDLE;
    VkDeviceSize    m_Size       = 0;
    VkDeviceAddress m_Address    = 0;
    void*           m_Mapped     = nullptr;
};

} // namespace Engine
