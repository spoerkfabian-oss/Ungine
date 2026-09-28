#include "Engine/Renderer/ShadowAtlas.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cassert>

namespace Engine {

namespace {
// Every second bit of `x`, packed (inverse of bit interleaving).
std::uint32_t CompactBits(std::uint32_t x)
{
    x &= 0x55555555u;
    x = (x ^ (x >> 1)) & 0x33333333u;
    x = (x ^ (x >> 2)) & 0x0f0f0f0fu;
    x = (x ^ (x >> 4)) & 0x00ff00ffu;
    x = (x ^ (x >> 8)) & 0x0000ffffu;
    return x;
}
} // namespace

std::optional<std::vector<glm::uvec2>> PackShadowTiles(std::span<const std::uint32_t> sizes, std::uint32_t atlasSize,
                                                       std::uint32_t minTile)
{
    assert(std::has_single_bit(atlasSize) && std::has_single_bit(minTile) && minTile <= atlasSize);
    const std::uint64_t cellsPerSide = atlasSize / minTile;
    const std::uint64_t capacity     = cellsPerSide * cellsPerSide;

    std::vector<glm::uvec2> positions;
    positions.reserve(sizes.size());
    std::uint64_t cursor = 0; // Morton index in minTile cells
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const std::uint32_t size = sizes[i];
        assert(std::has_single_bit(size) && size >= minTile && size <= atlasSize);
        assert(i == 0 || size <= sizes[i - 1]);
        const std::uint64_t side  = size / minTile;
        const std::uint64_t cells = side * side;
        if (cursor + cells > capacity)
            return std::nullopt;
        const auto code = static_cast<std::uint32_t>(cursor);
        positions.emplace_back(CompactBits(code) * minTile, CompactBits(code >> 1) * minTile);
        cursor += cells;
    }
    return positions;
}

void ShadowTileAllocator::Reset(std::uint32_t atlasSize, std::uint32_t minTile)
{
    assert(std::has_single_bit(atlasSize) && std::has_single_bit(minTile) && minTile <= atlasSize);
    m_AtlasSize = atlasSize;
    m_MinTile   = minTile;
    m_Free.assign(static_cast<std::size_t>(std::countr_zero(atlasSize / minTile)) + 1, {});
    m_Free[0].push_back(glm::uvec2(0));
}

std::uint32_t ShadowTileAllocator::Level(std::uint32_t size) const
{
    return static_cast<std::uint32_t>(std::countr_zero(m_AtlasSize / size));
}

std::optional<glm::uvec2> ShadowTileAllocator::Allocate(std::uint32_t size)
{
    assert(std::has_single_bit(size) && size >= m_MinTile && size <= m_AtlasSize);
    const std::uint32_t level = Level(size);
    // Smallest free block that fits (deepest level <= target).
    std::int32_t from = static_cast<std::int32_t>(level);
    while (from >= 0 && m_Free[static_cast<std::size_t>(from)].empty())
        --from;
    if (from < 0)
        return std::nullopt;

    glm::uvec2 block = m_Free[static_cast<std::size_t>(from)].back();
    m_Free[static_cast<std::size_t>(from)].pop_back();
    // Split down to the requested size, keeping the top-left quadrant each time.
    for (auto l = static_cast<std::uint32_t>(from); l < level; ++l) {
        const std::uint32_t half = m_AtlasSize >> (l + 1);
        auto&               list = m_Free[l + 1];
        list.push_back(block + glm::uvec2(half, half));
        list.push_back(block + glm::uvec2(0, half));
        list.push_back(block + glm::uvec2(half, 0));
    }
    return block;
}

void ShadowTileAllocator::Free(glm::uvec2 offset, std::uint32_t size)
{
    std::uint32_t level = Level(size);
    // Merge with the three sibling quadrants while they are all free.
    while (level > 0) {
        const std::uint32_t blockSize  = m_AtlasSize >> level;
        const glm::uvec2    parent     = offset & glm::uvec2(~(2 * blockSize - 1));
        auto&               list       = m_Free[level];
        const glm::uvec2    siblings[] = {parent, parent + glm::uvec2(blockSize, 0), parent + glm::uvec2(0, blockSize),
                                          parent + glm::uvec2(blockSize, blockSize)};
        bool                allFree    = true;
        for (const glm::uvec2& s : siblings)
            allFree &= s == offset || std::ranges::find(list, s) != list.end();
        if (!allFree)
            break;
        for (const glm::uvec2& s : siblings)
            if (s != offset)
                list.erase(std::ranges::find(list, s));
        offset = parent;
        --level;
    }
    m_Free[level].push_back(offset);
}

std::uint64_t ShadowTileAllocator::FreeArea() const
{
    std::uint64_t area = 0;
    for (std::size_t l = 0; l < m_Free.size(); ++l) {
        const std::uint64_t side = m_AtlasSize >> l;
        area += side * side * m_Free[l].size();
    }
    return area;
}

glm::mat4 CubeFaceView(const glm::vec3& position, std::uint32_t face)
{
    static constexpr glm::vec3 kAxis[kCubeFaces] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static constexpr glm::vec3 kUp[kCubeFaces]   = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    assert(face < kCubeFaces);
    return glm::lookAt(position, position + kAxis[face], kUp[face]);
}

float ShadowTanHalfWithBorder(float tanHalf, std::uint32_t tileSize, float borderTexels)
{
    const float inner = 1.0f - 2.0f * borderTexels / static_cast<float>(tileSize);
    return tanHalf / std::max(inner, 0.25f);
}

} // namespace Engine
