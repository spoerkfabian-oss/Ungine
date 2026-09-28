#include "Engine/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <utility>

namespace Engine {

glm::mat4 Transform::LocalMatrix() const
{
    return glm::translate(glm::mat4{1.0f}, position) * glm::mat4_cast(rotation) * glm::scale(glm::mat4{1.0f}, scale);
}

Entity Scene::CreateEntity(std::string name, Entity parent)
{
    const Entity e = m_Registry.Create();
    m_Registry.Emplace<Name>(e, std::move(name));
    m_Registry.Emplace<Transform>(e);
    m_Registry.Emplace<WorldTransform>(e);
    m_Registry.Emplace<Hierarchy>(e);
    if (parent != NullEntity)
        SetParent(e, parent);
    return e;
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
        m_Registry.Destroy(e);
    }
}

void Scene::SetParent(Entity child, Entity parent)
{
    assert(child != parent);
    assert((parent == NullEntity || !IsAncestor(child, parent)) && "SetParent would create a cycle");
    Detach(child);
    m_Registry.Get<Hierarchy>(child).parent = parent;
    if (parent != NullEntity)
        m_Registry.Get<Hierarchy>(parent).children.push_back(child);
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

bool Scene::IsAncestor(Entity ancestor, Entity entity)
{
    for (Entity p = m_Registry.Get<Hierarchy>(entity).parent; p != NullEntity; p = m_Registry.Get<Hierarchy>(p).parent)
        if (p == ancestor)
            return true;
    return false;
}

void Scene::UpdateTransforms()
{
    struct Item {
        Entity    entity;
        glm::mat4 parentWorld;
    };
    std::vector<Item> stack;

    m_Registry.ViewOf<Hierarchy>().Each([&](Entity e, Hierarchy& h) {
        if (h.parent == NullEntity)
            stack.push_back({e, glm::mat4{1.0f}});
    });

    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        const glm::mat4 world = item.parentWorld * m_Registry.Get<Transform>(item.entity).LocalMatrix();
        m_Registry.Get<WorldTransform>(item.entity).matrix = world;
        for (Entity child : m_Registry.Get<Hierarchy>(item.entity).children)
            stack.push_back({child, world});
    }
}

} // namespace Engine
