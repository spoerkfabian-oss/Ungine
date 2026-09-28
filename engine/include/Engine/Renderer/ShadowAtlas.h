#pragma once
#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Engine {

// Point light shadows: one atlas tile per cube face, in this order (mirrored in shadow.glsl).
inline constexpr std::uint32_t kCubeFaces = 6; // +X, -X, +Y, -Y, +Z, -Z

// Packs power-of-two square tiles into a square power-of-two atlas. `sizes` must be sorted in
// descending order, each a power of two in [minTile, atlasSize]. Tiles are laid out back to back
// along a Morton (Z-order) curve in units of minTile: with descending sizes every tile starts
// aligned to its own size, so nothing overlaps and no space is lost. Returns the top-left texel
// of each tile, or nullopt when their total area exceeds the atlas.
[[nodiscard]] std::optional<std::vector<glm::uvec2>> PackShadowTiles(std::span<const std::uint32_t> sizes,
                                                                     std::uint32_t atlasSize, std::uint32_t minTile);

// Buddy allocator for power-of-two square tiles in a square atlas: a free block splits into four
// quadrants, four free quadrants merge back. Tiles keep their place until freed, so cached shadow
// maps survive from frame to frame.
class ShadowTileAllocator {
public:
    void Reset(std::uint32_t atlasSize, std::uint32_t minTile);
    // size: power of two in [minTile, atlasSize]. Top-left texel, or nullopt when no block is free.
    [[nodiscard]] std::optional<glm::uvec2> Allocate(std::uint32_t size);
    void                                    Free(glm::uvec2 offset, std::uint32_t size);
    [[nodiscard]] std::uint64_t             FreeArea() const; // in texels
    [[nodiscard]] std::uint32_t             AtlasSize() const { return m_AtlasSize; }
    [[nodiscard]] std::uint32_t             MinTile() const { return m_MinTile; }

private:
    [[nodiscard]] std::uint32_t Level(std::uint32_t size) const; // 0 = whole atlas

    std::uint32_t                        m_AtlasSize = 0;
    std::uint32_t                        m_MinTile   = 1;
    std::vector<std::vector<glm::uvec2>> m_Free; // free blocks per level
};

// View matrix of a point light's cube face (right-handed, looking along the face axis).
[[nodiscard]] glm::mat4 CubeFaceView(const glm::vec3& position, std::uint32_t face);

// Widens a projection (half-angle tangent `tanHalf`) so the original region maps to the tile
// minus `borderTexels` on every side: PCF taps near its edge stay inside the tile.
[[nodiscard]] float ShadowTanHalfWithBorder(float tanHalf, std::uint32_t tileSize, float borderTexels);

} // namespace Engine
