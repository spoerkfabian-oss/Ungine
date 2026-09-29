#include "Engine/Renderer/GeometryPool.h"
#include "Engine/Assets/Model.h"

#include <format>
#include <stdexcept>

namespace Engine {

namespace {
constexpr std::array<std::uint32_t, 4> kStrides{sizeof(Vertex), sizeof(std::uint32_t), sizeof(GpuMaterial),
                                                sizeof(GpuSubmesh)};
constexpr std::array<const char*, 4>   kNames{"vertices", "indices", "materials", "submeshes"};
} // namespace

std::uint32_t GeometryPool::Stride(GeometryKind kind)
{
    return kStrides[static_cast<std::size_t>(kind)];
}

GeometryPool::GeometryPool(const VulkanContext& ctx, UploadQueue& uploader, const GeometryPoolDesc& desc)
    : m_Uploader(uploader)
{
    const std::array<std::uint32_t, 4> capacity{desc.maxVertices, desc.maxIndices, desc.maxMaterials, desc.maxSubmeshes};
    constexpr VkBufferUsageFlags       kUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    constexpr std::array<const char*, 4> kDebugNames{"GeometryVertices", "GeometryIndices", "GeometryMaterials",
                                                     "GeometrySubmeshes"};
    for (std::size_t i = 0; i < m_Pools.size(); ++i) {
        Slot& pool  = m_Pools[i];
        pool.stride = kStrides[i];
        pool.ranges.Reset(std::max(capacity[i], 1u));
        pool.buffer = Buffer(ctx, {.size       = VkDeviceSize{pool.ranges.Capacity()} * pool.stride,
                                   .usage      = kUsage | (i == static_cast<std::size_t>(GeometryKind::Indices)
                                                               ? VkBufferUsageFlags{VK_BUFFER_USAGE_INDEX_BUFFER_BIT}
                                                               : VkBufferUsageFlags{0}),
                                   .debugName  = kDebugNames[i],
                                   .concurrent = true});
    }
}

PoolRange GeometryPool::Upload(GeometryKind kind, std::span<const std::byte> bytes, UploadTicket& ticket)
{
    Slot& pool = Pool(kind);
    if (bytes.empty() || bytes.size() % pool.stride != 0)
        throw std::runtime_error(std::format("GeometryPool: {} bytes are not whole {}", bytes.size(),
                                             kNames[static_cast<std::size_t>(kind)]));
    const auto count = static_cast<std::uint32_t>(bytes.size() / pool.stride);

    std::optional<std::uint32_t> offset;
    PoolUsage                    usage;
    {
        std::scoped_lock lock(pool.mutex);
        offset = pool.ranges.Allocate(count);
        usage  = {pool.ranges.Used(), pool.ranges.Capacity(), pool.ranges.LargestFree()};
    }
    if (!offset)
        throw std::runtime_error(std::format("Geometry pool full: {} {} requested, {} of {} in use (largest hole {})",
                                             count, kNames[static_cast<std::size_t>(kind)], usage.used, usage.capacity,
                                             usage.largestFree));
    // Disjoint from every range in use: the copy never races with draws reading other ranges.
    try {
        m_Uploader.WriteBuffer(pool.buffer.Handle(), VkDeviceSize{*offset} * pool.stride, bytes, ticket);
    } catch (...) {
        Free(kind, {*offset, count});
        throw;
    }
    return {*offset, count};
}

void GeometryPool::Free(GeometryKind kind, PoolRange range)
{
    Slot&            pool = Pool(kind);
    std::scoped_lock lock(pool.mutex);
    pool.ranges.Free(range.offset, range.count);
}

PoolUsage GeometryPool::Usage(GeometryKind kind) const
{
    const Slot&      pool = Pool(kind);
    std::scoped_lock lock(pool.mutex);
    return {pool.ranges.Used(), pool.ranges.Capacity(), pool.ranges.LargestFree()};
}

} // namespace Engine
