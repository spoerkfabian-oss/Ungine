#pragma once
#include "Engine/ECS/Registry.h"
#include "Engine/Scene/Components.h"

#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>

namespace Engine {

// Registry + scene-graph rules. Every entity created here has
// Uuid, Name, Transform, WorldTransform and Hierarchy.
class Scene {
public:
    static constexpr std::size_t kAppend = std::numeric_limits<std::size_t>::max();

    Scene();

    // uuid 0 (or one already in use): a new random one.
    Entity CreateEntity(std::string name, Entity parent = NullEntity, std::uint64_t uuid = 0);
    void   DestroyEntity(Entity entity); // destroys the whole subtree
    void   Clear();                      // destroys every entity
    // siblingIndex: position among the parent's children (clamped; roots have no order).
    void   SetParent(Entity child, Entity parent, std::size_t siblingIndex = kAppend);
    [[nodiscard]] std::size_t SiblingIndex(Entity entity) const; // 0 for roots

    [[nodiscard]] Entity FindByUuid(std::uint64_t uuid) const; // NullEntity if unknown
    [[nodiscard]] bool   IsAncestor(Entity ancestor, Entity entity) const;

    // Propagates local transforms down the hierarchy (iterative DFS from the roots).
    void UpdateTransforms();

    [[nodiscard]] Registry&       GetRegistry()       { return m_Registry; }
    [[nodiscard]] const Registry& GetRegistry() const { return m_Registry; }

private:
    void Detach(Entity child);

    Registry                                  m_Registry;
    std::unordered_map<std::uint64_t, Entity> m_ByUuid;
    std::mt19937_64                           m_Random;
};

} // namespace Engine
