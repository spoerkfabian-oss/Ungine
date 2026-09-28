#pragma once
#include "Engine/ECS/Registry.h"
#include "Engine/Scene/Components.h"

#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

// What changed since the previous Scene::TakeChanges() (consumed once per frame by the
// renderer's spatial index and shadow caches).
struct SceneChanges {
    std::vector<Entity> changed;   // world transform recomputed (incl. new entities) or MarkChanged
    std::vector<Entity> destroyed; // handles are no longer valid
    bool                overflow = false; // too much to track: consumers rebuild from scratch
};

struct TransformUpdateStats {
    std::uint32_t dirtyRoots   = 0; // subtrees recomputed
    std::uint32_t updated      = 0; // world matrices written
    double        milliseconds = 0.0;
};

// Registry + scene-graph rules. Every entity created here has
// Uuid, Name, Transform, WorldTransform and Hierarchy.
//
// Transforms are written through EditTransform / SetTransform only: that marks the subtree dirty
// and UpdateTransforms recomputes just the dirty subtrees. Writing Transform through the registry
// directly leaves stale world matrices and caches (CountStaleTransforms finds them in tests).
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

    [[nodiscard]] const Transform& GetTransform(Entity entity) const { return m_Registry.Get<Transform>(entity); }
    [[nodiscard]] Transform&       EditTransform(Entity entity); // marks the subtree dirty
    void                           SetTransform(Entity entity, const Transform& transform);
    // MeshRenderer or Light added, removed or edited (bounds and shadow caches depend on them).
    void MarkChanged(Entity entity);

    // Recomputes the world matrices of dirty subtrees (parents before children).
    void UpdateTransforms();
    [[nodiscard]] const TransformUpdateStats& LastTransformUpdate() const { return m_LastUpdate; } // last call
    // Sum over all UpdateTransforms calls between the previous two TakeChanges (a whole frame).
    [[nodiscard]] const TransformUpdateStats& FrameTransformUpdate() const { return m_FrameUpdate; }
    [[nodiscard]] SceneChanges                TakeChanges();
    // Debug / tests: world matrices that differ from a full recompute.
    [[nodiscard]] std::size_t CountStaleTransforms() const;

    [[nodiscard]] Registry&       GetRegistry()       { return m_Registry; }
    [[nodiscard]] const Registry& GetRegistry() const { return m_Registry; }

private:
    void Detach(Entity child);
    void MarkDirty(Entity entity);
    void RecordChange(Entity entity);
    void RecordDestroyed(Entity entity);
    [[nodiscard]] static bool Flag(const std::vector<std::uint8_t>& flags, Entity entity);
    static void               SetFlag(std::vector<std::uint8_t>& flags, Entity entity, bool value);

    static constexpr std::size_t kMaxTrackedChanges = std::size_t{1} << 20;

    Registry                                  m_Registry;
    std::vector<Entity>                       m_Dirty;       // subtrees to recompute
    std::vector<std::uint8_t>                 m_DirtyFlags;  // by entity index
    SceneChanges                              m_Changes;
    std::vector<std::uint8_t>                 m_ChangedFlags; // by entity index, deduplicates m_Changes.changed
    TransformUpdateStats                      m_LastUpdate;
    TransformUpdateStats                      m_FrameAccumulator, m_FrameUpdate;
    std::unordered_map<std::uint64_t, Entity> m_ByUuid;
    std::mt19937_64                           m_Random;
};

} // namespace Engine
