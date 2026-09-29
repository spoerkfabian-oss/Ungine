#include "Engine/Scene/SpatialIndex.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Scene/Scene.h"

#include <chrono>

namespace Engine {

namespace {
constexpr float kInfinity = std::numeric_limits<float>::infinity();
} // namespace

void SpatialIndex::Sync(Scene& scene, const AssetManager& assets)
{
    const auto start = std::chrono::steady_clock::now();
    m_LastSync       = {};
    m_ChangedRegions.clear();
    m_MeshUpdates.clear();

    SceneChanges changes = scene.TakeChanges();
    if (!m_Initialized || changes.overflow) {
        Rebuild(scene, assets);
    } else {
        for (Entity e : changes.destroyed) {
            RemoveMesh(e);
            RemoveLight(e);
            m_Pending.erase(Key(e));
        }
        // Models that were reloaded, failed or retried since the last sync: new bounds / geometry.
        if (assets.ContentVersion() != m_ContentVersion) {
            std::vector<Entity> stale;
            for (const MeshProxy& proxy : m_Meshes)
                if (assets.Revision(proxy.model) != proxy.revision)
                    stale.push_back(proxy.entity);
            for (Entity e : stale)
                UpdateMesh(scene, assets, e);
        }
        // Models that finished loading since the last sync.
        if (!m_Pending.empty()) {
            const std::vector<std::uint64_t> pending(m_Pending.begin(), m_Pending.end());
            m_Pending.clear();
            for (std::uint64_t key : pending)
                UpdateMesh(scene, assets, Entity{key});
        }
        for (Entity e : changes.changed) {
            UpdateMesh(scene, assets, e);
            UpdateLight(scene, e);
        }
        m_LastSync.changed = static_cast<std::uint32_t>(changes.changed.size());
    }
    m_ContentVersion        = assets.ContentVersion();
    m_LastSync.pending      = static_cast<std::uint32_t>(m_Pending.size());
    m_LastSync.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void SpatialIndex::Rebuild(Scene& scene, const AssetManager& assets)
{
    m_MeshTree.Clear();
    m_LightTree.Clear();
    m_Meshes.clear();
    m_Lights.clear();
    m_MeshIndex.clear();
    m_LightIndex.clear();
    m_Pending.clear();
    m_Submeshes   = 0;
    m_Initialized = true;

    Registry& registry = scene.GetRegistry();
    registry.ViewOf<MeshRenderer>().Each([&](Entity e, MeshRenderer&) { UpdateMesh(scene, assets, e); });
    registry.ViewOf<Light>().Each([&](Entity e, Light&) { UpdateLight(scene, e); });
    m_ChangedRegions.assign(1, Aabb{glm::vec3(-kInfinity), glm::vec3(kInfinity)}); // everything
    m_LastSync.rebuilt = true;
}

void SpatialIndex::UpdateMesh(Scene& scene, const AssetManager& assets, Entity entity)
{
    m_MeshUpdates.push_back(entity);
    const Registry&     registry = scene.GetRegistry();
    const MeshRenderer* renderer = registry.Valid(entity) ? registry.TryGet<MeshRenderer>(entity) : nullptr;
    const ResolvedMesh  resolved = renderer ? assets.ResolveMesh(renderer->model, renderer->meshIndex) : ResolvedMesh{};
    const Model*        model    = resolved.model; // the placeholder box for failed models
    if (!model || model->meshes[resolved.meshIndex].submeshes.empty()) {
        RemoveMesh(entity);
        if (renderer) {
            const AssetState state = assets.State(renderer->model);
            if (state == AssetState::Loading || state == AssetState::Uploading)
                m_Pending.insert(Key(entity)); // retried every sync until it is Ready (or fails)
        }
        return;
    }

    const Mesh&         mesh     = model->meshes[resolved.meshIndex];
    const std::uint32_t revision = assets.Revision(renderer->model);
    Aabb                local{glm::vec3(kInfinity), glm::vec3(-kInfinity)};
    for (const Submesh& sm : mesh.submeshes) {
        local.min = glm::min(local.min, sm.boundsMin);
        local.max = glm::max(local.max, sm.boundsMax);
    }
    const Aabb world = TransformAabb(local, registry.Get<WorldTransform>(entity).matrix);
    const auto submeshes = static_cast<std::uint32_t>(mesh.submeshes.size());

    if (const auto it = m_MeshIndex.find(Key(entity)); it != m_MeshIndex.end()) {
        MeshProxy& proxy = m_Meshes[it->second];
        const bool same  = proxy.bounds.min == world.min && proxy.bounds.max == world.max &&
                           proxy.model == renderer->model && proxy.meshIndex == renderer->meshIndex &&
                           proxy.revision == revision; // reloaded geometry: shadows must re-render
        if (same)
            return;
        m_ChangedRegions.push_back(proxy.bounds);
        m_ChangedRegions.push_back(world);
        m_Submeshes += submeshes;
        m_Submeshes -= proxy.submeshes;
        proxy.bounds    = world;
        proxy.model     = renderer->model;
        proxy.meshIndex = renderer->meshIndex;
        proxy.submeshes = submeshes;
        proxy.revision  = revision;
        m_LastSync.reinserted += m_MeshTree.Move(proxy.node, world) ? 1u : 0u;
        return;
    }

    const auto index = static_cast<std::uint32_t>(m_Meshes.size());
    m_Meshes.push_back({.entity    = entity,
                        .node      = m_MeshTree.Insert(world, index),
                        .bounds    = world,
                        .model     = renderer->model,
                        .meshIndex = renderer->meshIndex,
                        .submeshes = submeshes,
                        .revision  = revision});
    m_MeshIndex.emplace(Key(entity), index);
    m_Submeshes += submeshes;
    m_ChangedRegions.push_back(world);
    ++m_LastSync.reinserted;
}

void SpatialIndex::RemoveMesh(Entity entity)
{
    const auto it = m_MeshIndex.find(Key(entity));
    if (it == m_MeshIndex.end())
        return;
    m_MeshUpdates.push_back(entity);
    const std::uint32_t index = it->second;
    m_MeshIndex.erase(it);
    MeshProxy& proxy = m_Meshes[index];
    m_ChangedRegions.push_back(proxy.bounds);
    m_Submeshes -= proxy.submeshes;
    m_MeshTree.Remove(proxy.node);

    // Swap-remove: the last proxy takes the freed slot.
    if (index + 1 != m_Meshes.size()) {
        proxy = m_Meshes.back();
        m_MeshIndex[Key(proxy.entity)] = index;
        m_MeshTree.SetUserData(proxy.node, index);
    }
    m_Meshes.pop_back();
}

void SpatialIndex::UpdateLight(Scene& scene, Entity entity)
{
    const Registry& registry = scene.GetRegistry();
    const Light*    light    = registry.Valid(entity) ? registry.TryGet<Light>(entity) : nullptr;
    if (!light) {
        RemoveLight(entity);
        return;
    }
    const glm::vec3 position = registry.Get<WorldTransform>(entity).matrix[3];
    const float     range    = EffectiveRange(*light);
    const Aabb      box{position - glm::vec3(range), position + glm::vec3(range)};

    if (const auto it = m_LightIndex.find(Key(entity)); it != m_LightIndex.end()) {
        LightProxy& proxy = m_Lights[it->second];
        proxy.bounds      = box;
        m_LightTree.Move(proxy.node, box);
        return;
    }
    const auto index = static_cast<std::uint32_t>(m_Lights.size());
    m_Lights.push_back({.entity = entity, .node = m_LightTree.Insert(box, index), .bounds = box});
    m_LightIndex.emplace(Key(entity), index);
}

void SpatialIndex::RemoveLight(Entity entity)
{
    const auto it = m_LightIndex.find(Key(entity));
    if (it == m_LightIndex.end())
        return;
    const std::uint32_t index = it->second;
    m_LightIndex.erase(it);
    LightProxy& proxy = m_Lights[index];
    m_LightTree.Remove(proxy.node);
    if (index + 1 != m_Lights.size()) {
        proxy = m_Lights.back();
        m_LightIndex[Key(proxy.entity)] = index;
        m_LightTree.SetUserData(proxy.node, index);
    }
    m_Lights.pop_back();
}

std::optional<RayHit> SpatialIndex::Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDistance) const
{
    const float length = glm::length(dir);
    if (!(length > 0.0f))
        return std::nullopt;
    const glm::vec3       direction = dir / length;
    const glm::vec3       invDir    = 1.0f / direction;
    std::optional<RayHit> best;
    m_MeshTree.Raycast(origin, direction, maxDistance, [&](std::int32_t node, float maxT) {
        const MeshProxy& proxy = m_Meshes[m_MeshTree.UserData(node)];
        const float      t     = AabbTree::RayBox(origin, invDir, proxy.bounds, maxT);
        if (t < 0.0f)
            return maxT;
        best = RayHit{.entity = proxy.entity, .distance = t};
        return t; // only closer hits from here on
    });
    return best;
}

const SpatialIndex::MeshProxy* SpatialIndex::FindMesh(Entity entity) const
{
    const auto it = m_MeshIndex.find(Key(entity));
    return it != m_MeshIndex.end() ? &m_Meshes[it->second] : nullptr;
}

std::optional<Aabb> SpatialIndex::Bounds(Entity entity) const
{
    if (const auto it = m_MeshIndex.find(Key(entity)); it != m_MeshIndex.end())
        return m_Meshes[it->second].bounds;
    if (const auto it = m_LightIndex.find(Key(entity)); it != m_LightIndex.end())
        return m_Lights[it->second].bounds;
    return std::nullopt;
}

} // namespace Engine
