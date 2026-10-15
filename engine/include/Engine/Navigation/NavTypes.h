#pragma once
#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

// Area name that removes the walkable surface (NavArea components).
inline constexpr std::string_view kNavAreaBlocked = "Blocked";
inline constexpr std::size_t      kNavMaxAreas    = 16; // incl. "Default" (index 0)
inline constexpr std::size_t      kNavMaxProfiles = 8;

// Agent size class: every profile gets its own navmesh (eroded by its radius).
struct NavAgentProfile {
    std::string name     = "Default";
    float       radius   = 0.4f;  // m
    float       height   = 1.8f;  // m
    float       maxClimb = 0.4f;  // step height (m)
    float       maxSlope = 45.0f; // degrees

    bool operator==(const NavAgentProfile&) const = default;
};

// Surface type with a path cost multiplier (>= 1: Detour's heuristic assumes cost >= 1).
struct NavAreaType {
    std::string name = "Default";
    float       cost = 1.0f;
    glm::vec3   color{0.2f, 0.6f, 1.0f}; // editor overlay

    bool operator==(const NavAreaType&) const = default;
};

// Project-wide navigation settings (.ungineproj -> "navigation"). Changing the voxel / tile
// settings or the profiles rebuilds every navmesh; baked files with other settings are ignored.
struct NavSettings {
    float         cellSize          = 0.2f;  // m, horizontal voxel size
    float         cellHeight        = 0.1f;  // m
    std::uint32_t tileSize          = 48;    // cells per tile edge (tile = tileSize * cellSize m)
    float         edgeMaxLength     = 12.0f; // m
    float         edgeMaxError      = 1.3f;  // cells
    float         regionMinSize     = 8.0f;  // cells (sqrt of the area)
    float         regionMergeSize   = 20.0f; // cells
    std::uint32_t maxTiles          = 4096;  // tile layers per navmesh (16 .. 16384)
    std::uint32_t maxObstacles      = 256;
    std::uint32_t maxAgents         = 128;   // per profile
    std::vector<NavAgentProfile> profiles{NavAgentProfile{}};
    std::vector<NavAreaType>     areas{NavAreaType{},
                                       NavAreaType{.name = "Rough", .cost = 3.0f, .color = {0.8f, 0.6f, 0.2f}},
                                       NavAreaType{.name = "Water", .cost = 10.0f, .color = {0.1f, 0.3f, 0.9f}}};

    // Index of a profile / area by name (empty profile name: the first profile).
    [[nodiscard]] std::optional<std::size_t> Profile(std::string_view name) const;
    [[nodiscard]] std::optional<std::size_t> Area(std::string_view name) const;
    // Voxel / tile settings and profiles (what the tiles depend on), for baked files.
    [[nodiscard]] std::uint64_t BuildHash() const;

    bool operator==(const NavSettings&) const = default;
};

// Clamps values, keeps at most kNavMaxProfiles / kNavMaxAreas, makes names unique and non-empty,
// guarantees at least one profile and area 0 "Default".
void SanitizeNavSettings(NavSettings& settings);

enum class NavPathStatus : std::uint8_t { NotFound, Partial, Complete };

struct NavPath {
    NavPathStatus          status = NavPathStatus::NotFound;
    std::vector<glm::vec3> points; // start .. end (corners of the straight path, incl. links)

    [[nodiscard]] bool  Found() const { return status != NavPathStatus::NotFound; }
    [[nodiscard]] float Length() const;
};

struct NavRaycastHit {
    bool      hit = false;    // the ray left the navmesh (wall) before the end
    glm::vec3 point{0.0f};    // where it stopped (end when not hit)
    glm::vec3 normal{0.0f};   // wall normal (horizontal) when hit
};

enum class NavMoveStatus : std::uint8_t { Idle, Moving, Arrived, Failed };

} // namespace Engine
