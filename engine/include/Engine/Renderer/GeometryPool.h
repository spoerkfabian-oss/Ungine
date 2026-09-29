#pragma once
#include "Engine/Renderer/RangeAllocator.h"
#include "Engine/Renderer/Vulkan/Buffer.h"
#include "Engine/Renderer/Vulkan/Upload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

namespace Engine {

// Capacities in elements. The pool never grows: a model that does not fit fails to load.
struct GeometryPoolDesc {
    std::uint32_t maxVertices  = 4u << 20;  // 48 B each (192 MB)
    std::uint32_t maxIndices   = 16u << 20; // 4 B each (64 MB)
    std::uint32_t maxMaterials = 1u << 16;  // 80 B each
    std::uint32_t maxSubmeshes = 1u << 18;  // 96 B each (GpuSubmesh, 24 MB)
};

enum class GeometryKind : std::uint32_t { Vertices, Indices, Materials, Submeshes, Count };

// Elements [offset, offset + count) of one pool buffer.
struct PoolRange {
    std::uint32_t offset = 0;
    std::uint32_t count  = 0;
};

struct PoolUsage {
    std::uint32_t used        = 0;
    std::uint32_t capacity    = 0;
    std::uint32_t largestFree = 0;
};

// Global geometry for GPU-driven rendering: every model's vertices, indices, materials and
// submesh records live in four shared buffers, so one index buffer binding and one multi-draw
// covers the whole scene. Allocation + upload are thread-safe (asset workers); ranges are
// freed by the owner once the GPU is done with them (Renderer::DeferCall).
class GeometryPool {
public:
    GeometryPool(const VulkanContext& ctx, UploadQueue& uploader, const GeometryPoolDesc& desc);

    GeometryPool(const GeometryPool&)            = delete;
    GeometryPool& operator=(const GeometryPool&) = delete;

    // Allocates `count` elements and records their upload (bytes = count * stride). Throws
    // std::runtime_error when the pool is full. Thread-safe.
    [[nodiscard]] PoolRange Upload(GeometryKind kind, std::span<const std::byte> bytes, UploadTicket& ticket);
    template <class T>
    [[nodiscard]] PoolRange Upload(GeometryKind kind, std::span<const T> elements, UploadTicket& ticket)
    {
        return Upload(kind, std::as_bytes(elements), ticket);
    }
    void Free(GeometryKind kind, PoolRange range); // thread-safe

    [[nodiscard]] VkDeviceAddress Address(GeometryKind kind) const { return Pool(kind).buffer.Address(); }
    [[nodiscard]] VkBuffer        IndexBuffer() const { return Pool(GeometryKind::Indices).buffer.Handle(); }
    [[nodiscard]] PoolUsage       Usage(GeometryKind kind) const;
    [[nodiscard]] static std::uint32_t Stride(GeometryKind kind);

private:
    struct Slot {
        Buffer                 buffer;
        RangeAllocator         ranges;
        std::uint32_t          stride = 0;
        mutable std::mutex     mutex; // guards `ranges`
    };
    [[nodiscard]] const Slot& Pool(GeometryKind kind) const { return m_Pools[static_cast<std::size_t>(kind)]; }
    [[nodiscard]] Slot&       Pool(GeometryKind kind) { return m_Pools[static_cast<std::size_t>(kind)]; }

    UploadQueue& m_Uploader;
    std::array<Slot, static_cast<std::size_t>(GeometryKind::Count)> m_Pools;
};

} // namespace Engine
