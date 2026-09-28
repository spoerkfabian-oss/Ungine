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
