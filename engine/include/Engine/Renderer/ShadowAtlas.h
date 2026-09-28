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

// View matrix of a point light's cube face (right-handed, looking along the face axis).
[[nodiscard]] glm::mat4 CubeFaceView(const glm::vec3& position, std::uint32_t face);

// Widens a projection (half-angle tangent `tanHalf`) so the original region maps to the tile
// minus `borderTexels` on every side: PCF taps near its edge stay inside the tile.
[[nodiscard]] float ShadowTanHalfWithBorder(float tanHalf, std::uint32_t tileSize, float borderTexels);

} // namespace Engine
