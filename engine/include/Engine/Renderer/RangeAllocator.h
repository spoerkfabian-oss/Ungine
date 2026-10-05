#pragma once
#include <cstdint>
#include <map>
#include <optional>

namespace Engine {

// Best-fit allocator of [offset, offset + count) ranges in a fixed capacity, free neighbors
// merged on Free. Not thread-safe (GeometryPool locks around it).
class RangeAllocator {
public:
    RangeAllocator() = default;
    explicit RangeAllocator(std::uint32_t capacity) { Reset(capacity); }

    void Reset(std::uint32_t capacity);
    [[nodiscard]] std::optional<std::uint32_t> Allocate(std::uint32_t count); // nullopt: no hole large enough
    void Free(std::uint32_t offset, std::uint32_t count);
    void Grow(std::uint32_t capacity); // adds [old capacity, capacity) as free space

    [[nodiscard]] std::uint32_t Capacity() const { return m_Capacity; }
    [[nodiscard]] std::uint32_t Used() const { return m_Used; }
    [[nodiscard]] std::uint32_t LargestFree() const;
    [[nodiscard]] std::size_t   FreeBlocks() const { return m_Free.size(); }

private:
    std::map<std::uint32_t, std::uint32_t> m_Free; // offset -> count, never adjacent
    std::uint32_t                          m_Capacity = 0;
    std::uint32_t                          m_Used     = 0;
};

} // namespace Engine
