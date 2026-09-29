#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Scene/AabbTree.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine {

class AssetManager;
class Scene;

struct RayHit {
    Entity entity   = NullEntity;
    float  distance = 0.0f; // along the (normalized) ray, to the entity's world AABB
};

// Scene bounds in two dynamic AABB trees: mesh renderers (world AABB of their mesh) and lights
// (box around the range sphere). Sync() applies Scene::TakeChanges() (so it must be the only
// consumer) and retries meshes whose model was still loading. Queries use the fat tree boxes and
// then the tight bounds. Main thread only.
class SpatialIndex {
public:
    struct MeshProxy {
        Entity        entity = NullEntity;
        std::int32_t  node   = AabbTree::kNull;
        Aabb          bounds;              // tight world AABB of the whole mesh
        ModelHandle   model;
        std::uint32_t meshIndex = 0;
        std::uint32_t submeshes = 0;
    };

    struct SyncStats {
        std::uint32_t changed      = 0; // entities processed
        std::uint32_t reinserted   = 0; // tree updates (bounds left the fat box)
        std::uint32_t pending      = 0; // meshes waiting for their model
        bool          rebuilt      = false;
        double        milliseconds = 0.0;
    };

    void Sync(Scene& scene, const AssetManager& assets);
    // Where mesh content changed during the last Sync (old and new bounds of moved, added and
    // removed meshes); a full rebuild reports one infinite box.
    [[nodiscard]] std::span<const Aabb> ChangedRegions() const { return m_ChangedRegions; }
    [[nodiscard]] const SyncStats&      LastSync() const { return m_LastSync; }
    // Entities whose mesh proxy was added, updated (any world transform or MeshRenderer change)
    // or removed during the last Sync; after a rebuild (LastSync().rebuilt) use Meshes() instead.
    [[nodiscard]] std::span<const Entity>    MeshUpdates() const { return m_MeshUpdates; }
    [[nodiscard]] std::span<const MeshProxy> Meshes() const { return m_Meshes; }
    [[nodiscard]] const MeshProxy*           FindMesh(Entity entity) const;

    // fn(const MeshProxy&) for meshes whose bounds intersect the frustum / box.
    template <class F>
    void QueryMeshes(const Frustum& frustum, F&& fn) const;
    template <class F>
    void QueryMeshes(const Aabb& box, F&& fn) const;
    // fn(Entity) for lights whose range box intersects the frustum.
    template <class F>
    void QueryLights(const Frustum& frustum, F&& fn) const;

    // Nearest mesh whose world AABB the ray hits (dir need not be normalized).
    [[nodiscard]] std::optional<RayHit> Raycast(const glm::vec3& origin, const glm::vec3& dir,
                                                float maxDistance = std::numeric_limits<float>::max()) const;
    // Tight world bounds of a mesh or a light's range; nullopt if the entity has neither.
    [[nodiscard]] std::optional<Aabb> Bounds(Entity entity) const;

    [[nodiscard]] std::size_t     MeshCount() const { return m_Meshes.size(); }
    [[nodiscard]] std::size_t     LightCount() const { return m_Lights.size(); }
    [[nodiscard]] std::uint64_t   SubmeshCount() const { return m_Submeshes; }
    [[nodiscard]] const AabbTree& MeshTree() const { return m_MeshTree; }
    [[nodiscard]] const AabbTree& LightTree() const { return m_LightTree; }

private:
    struct LightProxy {
        Entity       entity = NullEntity;
        std::int32_t node   = AabbTree::kNull;
        Aabb         bounds;
    };

    void Rebuild(Scene& scene, const AssetManager& assets);
    void UpdateMesh(Scene& scene, const AssetManager& assets, Entity entity);
    void RemoveMesh(Entity entity);
    void UpdateLight(Scene& scene, Entity entity);
    void RemoveLight(Entity entity);

    static std::uint64_t Key(Entity e) { return static_cast<std::uint64_t>(e); }

    AabbTree                                     m_MeshTree, m_LightTree;
    std::vector<MeshProxy>                       m_Meshes; // dense, tree user data = index
    std::vector<LightProxy>                      m_Lights;
    std::unordered_map<std::uint64_t, std::uint32_t> m_MeshIndex, m_LightIndex;
    std::unordered_set<std::uint64_t>            m_Pending; // entities waiting for their model
    std::vector<Aabb>                            m_ChangedRegions;
    std::vector<Entity>                          m_MeshUpdates;
    std::uint64_t                                m_Submeshes = 0;
    bool                                         m_Initialized = false;
    SyncStats                                    m_LastSync;
};

template <class F>
void SpatialIndex::QueryMeshes(const Frustum& frustum, F&& fn) const
{
    m_MeshTree.Query(frustum, [&](std::int32_t node) {
        const MeshProxy& proxy = m_Meshes[m_MeshTree.UserData(node)];
        if (frustum.Intersects(proxy.bounds))
            fn(proxy);
    });
}

template <class F>
void SpatialIndex::QueryMeshes(const Aabb& box, F&& fn) const
{
    m_MeshTree.Query(box, [&](std::int32_t node) {
        const MeshProxy& proxy = m_Meshes[m_MeshTree.UserData(node)];
        if (detail::Overlaps(proxy.bounds, box))
            fn(proxy);
    });
}

template <class F>
void SpatialIndex::QueryLights(const Frustum& frustum, F&& fn) const
{
    m_LightTree.Query(frustum, [&](std::int32_t node) {
        const LightProxy& proxy = m_Lights[m_LightTree.UserData(node)];
        if (frustum.Intersects(proxy.bounds))
            fn(proxy.entity);
    });
}

} // namespace Engine
