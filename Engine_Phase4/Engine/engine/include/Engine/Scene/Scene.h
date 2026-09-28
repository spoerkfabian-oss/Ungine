#pragma once
#include "Engine/ECS/Registry.h"
#include "Engine/Scene/Components.h"

#include <string>

namespace Engine {

// Registry + scene-graph rules. Every entity created here has
// Name, Transform, WorldTransform and Hierarchy.
class Scene {
public:
    Entity CreateEntity(std::string name, Entity parent = NullEntity);
    void   DestroyEntity(Entity entity); // destroys the whole subtree
    void   SetParent(Entity child, Entity parent);

    // Propagates local transforms down the hierarchy (iterative DFS from the roots).
    void UpdateTransforms();

    [[nodiscard]] Registry&       GetRegistry()       { return m_Registry; }
    [[nodiscard]] const Registry& GetRegistry() const { return m_Registry; }

private:
    void Detach(Entity child);
    [[nodiscard]] bool IsAncestor(Entity ancestor, Entity entity);

    Registry m_Registry;
};

} // namespace Engine
