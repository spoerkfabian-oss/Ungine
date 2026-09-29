#include "Engine/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace Engine {

glm::mat4 Transform::LocalMatrix() const
{
    return glm::translate(glm::mat4{1.0f}, position) * glm::mat4_cast(rotation) * glm::scale(glm::mat4{1.0f}, scale);
}

Scene::Scene() : m_Random(std::random_device{}()) {}

Entity Scene::CreateEntity(std::string name, Entity parent, std::uint64_t uuid)
{
    while (uuid == 0 || m_ByUuid.contains(uuid))
        uuid = m_Random();
    const Entity e = m_Registry.Create();
    m_ByUuid.emplace(uuid, e);
    m_Registry.Emplace<Uuid>(e, uuid);
    m_Registry.Emplace<Name>(e, std::move(name));
    m_Registry.Emplace<Transform>(e);
    m_Registry.Emplace<WorldTransform>(e);
    m_Registry.Emplace<Hierarchy>(e);
    if (parent != NullEntity)
        SetParent(e, parent);
    MarkDirty(e);
    return e;
}

bool Scene::Flag(const std::vector<std::uint8_t>& flags, Entity entity)
{
    const std::uint32_t index = EntityIndex(entity);
    return index < flags.size() && flags[index] != 0;
}

void Scene::SetFlag(std::vector<std::uint8_t>& flags, Entity entity, bool value)
{
    const std::uint32_t index = EntityIndex(entity);
    if (index >= flags.size()) {
        if (!value)
            return;
        flags.resize(std::max<std::size_t>(index + 1, flags.size() * 2), 0);
    }
    flags[index] = value ? 1 : 0;
}

void Scene::MarkDirty(Entity entity)
{
    if (Flag(m_DirtyFlags, entity))
        return;
    SetFlag(m_DirtyFlags, entity, true);
    m_Dirty.push_back(entity);
}

void Scene::RecordChange(Entity entity)
{
    if (m_Changes.overflow || Flag(m_ChangedFlags, entity))
        return;
    if (m_Changes.changed.size() + m_Changes.destroyed.size() >= kMaxTrackedChanges) {
        m_Changes.overflow = true;
        return;
    }
    SetFlag(m_ChangedFlags, entity, true);
    m_Changes.changed.push_back(entity);
}

void Scene::RecordDestroyed(Entity entity)
{
    // The slot may be reused right away: its flags must not stick to the next entity.
    SetFlag(m_DirtyFlags, entity, false);
    SetFlag(m_ChangedFlags, entity, false);
    if (m_Changes.overflow)
        return;
    if (m_Changes.changed.size() + m_Changes.destroyed.size() >= kMaxTrackedChanges) {
        m_Changes.overflow = true;
        return;
    }
    m_Changes.destroyed.push_back(entity);
}

Transform& Scene::EditTransform(Entity entity)
{
    MarkDirty(entity);
    return m_Registry.Get<Transform>(entity);
}

void Scene::SetTransform(Entity entity, const Transform& transform)
{
    MarkDirty(entity);
    m_Registry.Get<Transform>(entity) = transform;
}

void Scene::MarkChanged(Entity entity)
{
    if (m_Registry.Valid(entity))
        RecordChange(entity);
}

SceneChanges Scene::TakeChanges()
{
    m_FrameUpdate    = std::exchange(m_FrameAccumulator, TransformUpdateStats{});
    SceneChanges out = std::exchange(m_Changes, SceneChanges{});
    if (out.overflow)
        std::ranges::fill(m_ChangedFlags, std::uint8_t{0});
    else
        for (Entity e : out.changed)
            SetFlag(m_ChangedFlags, e, false);
    return out;
}

void Scene::DestroyEntity(Entity entity)
{
    if (!m_Registry.Valid(entity))
        return;
    Detach(entity);

    std::vector<Entity> stack{entity};
    while (!stack.empty()) {
        const Entity e = stack.back();
        stack.pop_back();
        if (auto* h = m_Registry.TryGet<Hierarchy>(e))
            stack.insert(stack.end(), h->children.begin(), h->children.end());
        if (const auto* id = m_Registry.TryGet<Uuid>(e))
            m_ByUuid.erase(id->value);
        RecordDestroyed(e);
        m_Registry.Destroy(e);
    }
}

void Scene::Clear()
{
    std::vector<Entity> roots;
    m_Registry.ViewOf<Hierarchy>().Each([&](Entity e, Hierarchy& h) {
        if (h.parent == NullEntity)
            roots.push_back(e);
    });
    for (Entity e : roots)
        DestroyEntity(e);
}

void Scene::SetParent(Entity child, Entity parent, std::size_t siblingIndex)
{
    assert(child != parent);
    assert((parent == NullEntity || !IsAncestor(child, parent)) && "SetParent would create a cycle");
    Detach(child);
    MarkDirty(child);
    m_Registry.Get<Hierarchy>(child).parent = parent;
    if (parent != NullEntity) {
        auto& children = m_Registry.Get<Hierarchy>(parent).children;
        children.insert(children.begin() + static_cast<std::ptrdiff_t>(std::min(siblingIndex, children.size())), child);
    }
}

std::size_t Scene::SiblingIndex(Entity entity) const
{
    const Entity parent = m_Registry.Get<Hierarchy>(entity).parent;
    if (parent == NullEntity)
        return 0;
    const auto& siblings = m_Registry.Get<Hierarchy>(parent).children;
    return static_cast<std::size_t>(std::ranges::find(siblings, entity) - siblings.begin());
}

Entity Scene::FindByUuid(std::uint64_t uuid) const
{
    const auto it = m_ByUuid.find(uuid);
    return it != m_ByUuid.end() && m_Registry.Valid(it->second) ? it->second : NullEntity;
}

Entity Scene::FindPrimaryCamera()
{
    Entity        best = NullEntity;
    bool          bestPrimary = false;
    std::uint64_t bestUuid    = 0;
    m_Registry.ViewOf<CameraComponent>().Each([&](Entity e, CameraComponent& cam) {
        const std::uint64_t uuid = m_Registry.Get<Uuid>(e).value;
        if (best == NullEntity || (cam.primary && !bestPrimary) || (cam.primary == bestPrimary && uuid < bestUuid)) {
            best        = e;
            bestPrimary = cam.primary;
            bestUuid    = uuid;
        }
    });
    return best;
}

void Scene::Detach(Entity child)
{
    auto& h = m_Registry.Get<Hierarchy>(child);
    if (h.parent == NullEntity)
        return;
    auto& siblings = m_Registry.Get<Hierarchy>(h.parent).children;
    std::erase(siblings, child);
    h.parent = NullEntity;
}

bool Scene::IsAncestor(Entity ancestor, Entity entity) const
{
    for (Entity p = m_Registry.Get<Hierarchy>(entity).parent; p != NullEntity; p = m_Registry.Get<Hierarchy>(p).parent)
        if (p == ancestor)
            return true;
    return false;
}

void Scene::UpdateTransforms()
{
    const auto start = std::chrono::steady_clock::now();
    m_LastUpdate     = {};

    struct Item {
        Entity    entity;
        glm::mat4 parentWorld;
    };
    std::vector<Item> stack;
    for (Entity root : m_Dirty) {
        if (!m_Registry.Valid(root))
            continue;
        // A dirty ancestor recomputes this subtree anyway.
        bool covered = false;
        for (Entity p = m_Registry.Get<Hierarchy>(root).parent; p != NullEntity && !covered;
             p = m_Registry.Get<Hierarchy>(p).parent)
            covered = Flag(m_DirtyFlags, p);
        if (covered)
            continue;

        ++m_LastUpdate.dirtyRoots;
        const Entity parent = m_Registry.Get<Hierarchy>(root).parent;
        stack.push_back({root, parent != NullEntity ? m_Registry.Get<WorldTransform>(parent).matrix : glm::mat4{1.0f}});
        while (!stack.empty()) {
            const Item item = stack.back();
            stack.pop_back();
            const glm::mat4 world = item.parentWorld * m_Registry.Get<Transform>(item.entity).LocalMatrix();
            m_Registry.Get<WorldTransform>(item.entity).matrix = world;
            RecordChange(item.entity);
            ++m_LastUpdate.updated;
            for (Entity child : m_Registry.Get<Hierarchy>(item.entity).children)
                stack.push_back({child, world});
        }
    }
    for (Entity e : m_Dirty)
        if (m_Registry.Valid(e))
            SetFlag(m_DirtyFlags, e, false);
    m_Dirty.clear();
    m_LastUpdate.milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    m_FrameAccumulator.dirtyRoots += m_LastUpdate.dirtyRoots;
    m_FrameAccumulator.updated += m_LastUpdate.updated;
    m_FrameAccumulator.milliseconds += m_LastUpdate.milliseconds;
}

std::size_t Scene::CountStaleTransforms() const
{
    std::size_t stale = 0;
    struct Item {
        Entity    entity;
        glm::mat4 parentWorld;
    };
    std::vector<Item> stack;
    for (std::uint32_t i = 0;; ++i) {
        const Entity e = m_Registry.EntityAtIndex(i);
        if (e == NullEntity)
            break;
        if (const auto* h = m_Registry.TryGet<Hierarchy>(e); h && h->parent == NullEntity)
            stack.push_back({e, glm::mat4{1.0f}});
    }
    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        const glm::mat4  world  = item.parentWorld * m_Registry.Get<Transform>(item.entity).LocalMatrix();
        const glm::mat4& cached = m_Registry.Get<WorldTransform>(item.entity).matrix;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (std::abs(world[c][r] - cached[c][r]) > 1e-4f * (1.0f + std::abs(world[c][r]))) {
                    ++stale;
                    c = 4;
                    break;
                }
        for (Entity child : m_Registry.Get<Hierarchy>(item.entity).children)
            stack.push_back({child, world});
    }
    return stale;
}

} // namespace Engine
