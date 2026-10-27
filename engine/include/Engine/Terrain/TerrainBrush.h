#pragma once
#include "Engine/Terrain/TerrainData.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Engine {

// Editing brushes for terrains (the editor's sculpt / paint tools). Dabs work in terrain space and
// report what they touched, so callers can mark the store dirty and record undo regions.

enum class TerrainTool : std::uint8_t { Raise, Lower, Smooth, Flatten, Paint, Holes, Foliage, Count };
[[nodiscard]] const char* ToString(TerrainTool tool);

struct TerrainBrush {
    TerrainTool   tool     = TerrainTool::Raise;
    float         radius   = 4.0f; // meters
    float         strength = 0.5f; // 0..1
    float         falloff  = 0.5f; // 0: hard edge, 1: smooth from the center
    std::uint32_t layer    = 0;    // Paint: splat layer, Foliage: foliage layer
    bool          erase    = false; // Holes: fill them, Foliage: remove density
    float         target   = 0.0f; // Flatten: height (meters)
};

// Brush weight at a distance from the center (0 outside the radius).
[[nodiscard]] float TerrainBrushWeight(const TerrainBrush& brush, float distance);

// One dab at `local` (terrain space, meters from the -X/-Z corner) for `dt` seconds of a held brush.
// Returns the change (kinds = 0: nothing touched); rect is normalized over the terrain.
TerrainChanges ApplyTerrainBrush(TerrainData& terrain, const TerrainBrush& brush, glm::vec2 local, float dt);

// Copies of the arrays a change kind covers, inside a normalized rectangle (undo / redo of strokes).
struct TerrainRegion {
    std::uint32_t kinds = 0;
    glm::vec4     rect{0.0f};
    std::uint32_t foliageLayer = 0;
    glm::uvec4    sampleRect{0u}; // heights / holes: x0, z0, x1, z1 (end exclusive)
    glm::uvec4    splatRect{0u};  // splat / density maps
    std::vector<std::uint16_t> heights;
    std::vector<std::uint8_t>  holes, splat, density;
};
[[nodiscard]] TerrainRegion CaptureTerrainRegion(const TerrainData& terrain, std::uint32_t kinds, glm::vec4 rect,
                                                 std::uint32_t foliageLayer = 0);
// Writes the region back (sizes must still match; returns false otherwise).
bool RestoreTerrainRegion(TerrainData& terrain, const TerrainRegion& region);

} // namespace Engine
