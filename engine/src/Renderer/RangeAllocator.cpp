#include "Engine/Renderer/RangeAllocator.h"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace Engine {

void RangeAllocator::Reset(std::uint32_t capacity)
{
    m_Free.clear();
    m_Capacity = capacity;
    m_Used     = 0;
    if (capacity > 0)
        m_Free.emplace(0u, capacity);
}

std::optional<std::uint32_t> RangeAllocator::Allocate(std::uint32_t count)
{
    if (count == 0)
        return std::nullopt;
    for (auto it = m_Free.begin(); it != m_Free.end(); ++it) {
        if (it->second < count)
            continue;
        const std::uint32_t offset = it->first;
        const std::uint32_t rest   = it->second - count;
        m_Free.erase(it);
        if (rest > 0)
            m_Free.emplace(offset + count, rest);
        m_Used += count;
        return offset;
    }
    return std::nullopt;
}

void RangeAllocator::Free(std::uint32_t offset, std::uint32_t count)
{
    if (count == 0)
        return;
    assert(offset + count <= m_Capacity && m_Used >= count);
    m_Used -= count;
    auto next = m_Free.lower_bound(offset);
    assert(next == m_Free.end() || next->first >= offset + count); // no double free
    // Merge with the following and the preceding hole.
    if (next != m_Free.end() && next->first == offset + count) {
        count += next->second;
        next = m_Free.erase(next);
    }
    if (next != m_Free.begin()) {
        const auto prev = std::prev(next);
        assert(prev->first + prev->second <= offset);
        if (prev->first + prev->second == offset) {
            prev->second += count;
            return;
        }
    }
    m_Free.emplace_hint(next, offset, count);
}

void RangeAllocator::Grow(std::uint32_t capacity)
{
    if (capacity <= m_Capacity)
        return;
    const std::uint32_t added = capacity - m_Capacity;
    const std::uint32_t start = m_Capacity;
    m_Capacity = capacity;
    m_Used += added; // Free() subtracts it again
    Free(start, added);
}

std::uint32_t RangeAllocator::LargestFree() const
{
    std::uint32_t largest = 0;
    for (const auto& [offset, count] : m_Free)
        largest = std::max(largest, count);
    return largest;
}

} // namespace Engine
