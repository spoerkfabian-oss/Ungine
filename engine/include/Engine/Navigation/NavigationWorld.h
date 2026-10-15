#pragma once
#include "Engine/ECS/Entity.h"
#include "Engine/Navigation/NavTypes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Engine {

class AssetManager;
class PhysicsWorld;
class Scene;
class ThreadPool;

struct NavStats {
    std::uint32_t profiles     = 0;
    std::uint32_t tiles        = 0; // navmesh tile layers (all profiles)
    std::uint32_t polygons     = 0;
    std::uint32_t sources      = 0; // geometry colliders
    std::uint32_t areas        = 0; // NavArea volumes
    std::uint32_t links        = 0;
    std::uint32_t obstacles    = 0;
    std::uint32_t agents       = 0;
    std::uint32_t pendingTiles = 0; // dirty or building (all profiles)
    std::uint64_t tilesBuilt   = 0; // since construction / Reset
    double        updateMs     = 0.0; // last Update on the main thread (sources, crowd, applying tiles)
};

// One crowd agent (editor overlay).
struct NavAgentDebug {
    Entity        entity = NullEntity;
    std::uint32_t profile = 0;
    glm::vec3     position{0.0f};
    glm::vec3     velocity{0.0f};
    glm::vec3     target{0.0f};
    NavMoveStatus status = NavMoveStatus::Idle;
    float         radius = 0.0f, height = 0.0f;
    std::vector<glm::vec3> corners; // next corners of its path
};

// Navigation for a Scene: one tiled Recast/Detour navmesh per agent profile (NavSettings), built
// from the colliders of static bodies inside NavMeshBounds volumes and marked by NavArea volumes,
// with NavLink off-mesh links, NavObstacle tile-cache obstacles and a DetourCrowd per profile
// moving the NavAgent entities. Main thread; tiles are rasterized in jobs on the ThreadPool.
//
// Tiles follow the scene: Update compares every navmesh source (collider, area, link, bounds) with
// the last state and rebuilds the tiles it touches (autoRebuild), in the background. Baked files
// (SaveBaked / LoadBaked, `NavMeshFileFor(scene)`) store the compressed tiles plus the source
// state they were built from: after loading, only tiles of changed sources are rebuilt.
class NavigationWorld {
public:
    // jobs: tile builds run there (null: synchronously in Update). assets: mesh colliders (null:
    // skipped). physics: agents with a CharacterController are moved through it (null: directly).
    explicit NavigationWorld(ThreadPool* jobs = nullptr, const AssetManager* assets = nullptr,
                             PhysicsWorld* physics = nullptr);
    ~NavigationWorld();

    NavigationWorld(const NavigationWorld&)            = delete;
    NavigationWorld& operator=(const NavigationWorld&) = delete;

    // Replaces the settings (sanitized); voxel / profile changes rebuild every tile.
    void SetSettings(const NavSettings& settings);
    [[nodiscard]] const NavSettings& Settings() const;

    // Rebuild tiles when sources change (else only Build / obstacles change the navmesh).
    bool autoRebuild = true;

    // Sources -> dirty tiles -> tile jobs; finished tiles go into the navmesh; obstacles; agents
    // (when simulate: the crowd moves over dt and writes the entities / character inputs).
    void Update(Scene& scene, float dt, bool simulate = true);
    // Every tile of every profile from the current scene, now (blocking; tiles still in jobs).
    void Build(Scene& scene);
    // Tiles dirty or building.
    [[nodiscard]] bool IsBuilding() const;
    // Forgets the scene (agents, sources, tiles, obstacles); settings stay.
    void Reset();

    // Writes the tiles + their source state. False with a message when nothing is built or I/O fails.
    bool SaveBaked(const std::filesystem::path& file, std::string* error = nullptr) const;
    // Replaces the navmeshes with the file's (through the VFS) when its settings match; the next
    // Updates rebuild only tiles whose sources changed since. False with a message otherwise.
    bool LoadBaked(const std::filesystem::path& file, std::string* error = nullptr);

    // --- Queries (profile index into NavSettings::profiles; areaMask: bit per area) ---
    // Closest point on the navmesh within `extents` (half box) of p.
    [[nodiscard]] std::optional<glm::vec3> ProjectPoint(const glm::vec3& p, std::uint32_t profile = 0,
                                                        const glm::vec3& extents = {2.0f, 4.0f, 2.0f}) const;
    [[nodiscard]] NavPath FindPath(const glm::vec3& start, const glm::vec3& end, std::uint32_t profile = 0,
                                   std::uint32_t areaMask = 0xFFFFFFFFu) const;
    // Random point on the navmesh (radius > 0: reachable from center within about radius).
    [[nodiscard]] std::optional<glm::vec3> RandomPoint(std::uint32_t profile = 0, std::uint32_t areaMask = 0xFFFFFFFFu) const;
    [[nodiscard]] std::optional<glm::vec3> RandomPointInRadius(const glm::vec3& center, float radius,
                                                               std::uint32_t profile = 0,
                                                               std::uint32_t areaMask = 0xFFFFFFFFu) const;
    // Walks the navmesh surface from start towards end (2D); stops at walls.
    [[nodiscard]] NavRaycastHit Raycast(const glm::vec3& start, const glm::vec3& end, std::uint32_t profile = 0,
                                        std::uint32_t areaMask = 0xFFFFFFFFu) const;
    // Seeds the random queries (tests); otherwise seeded from the clock.
    void SetRandomSeed(std::uint64_t seed);

    // --- Agents (entities with NavAgent) ---
    // Starts moving to target (projected onto the navmesh). Before the agent joined the crowd the
    // request waits for the next Update. Status Moving -> Arrived / Failed.
    void MoveTo(Entity entity, const glm::vec3& target);
    void StopMovement(Entity entity);
    [[nodiscard]] NavMoveStatus MoveStatus(Entity entity) const;
    [[nodiscard]] glm::vec3     AgentVelocity(Entity entity) const;
    [[nodiscard]] bool          HasAgent(Entity entity) const; // in a crowd

    // --- Debug ---
    // Triangles (polygon fans) of a profile's navmesh with the area index of their polygon.
    void ForEachTriangle(std::uint32_t profile,
                         const std::function<void(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                                  std::uint32_t area)>& fn) const;
    // Boundary edges of the polygons (walls, mesh borders).
    void ForEachBoundaryEdge(std::uint32_t profile,
                             const std::function<void(const glm::vec3& a, const glm::vec3& b)>& fn) const;
    void ForEachTile(std::uint32_t profile,
                     const std::function<void(const glm::vec3& min, const glm::vec3& max)>& fn) const;
    void ForEachAgent(const std::function<void(const NavAgentDebug&)>& fn) const;
    [[nodiscard]] const NavStats& Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

// Baked navmesh next to a scene: "Levels/Main.scene.json" -> "Levels/Main.unav".
[[nodiscard]] std::filesystem::path NavMeshFileFor(const std::filesystem::path& scene);

} // namespace Engine
