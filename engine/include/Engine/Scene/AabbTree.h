#pragma once
#include "Engine/Scene/Frustum.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace Engine {

// Dynamic AABB tree (as in Box2D / Bullet's DBVT). Leaves store "fat" AABBs, enlarged by a margin,
// so small moves do not touch the tree; a leaf is re-inserted only when its bounds leave the fat
// box. Insertion picks the sibling with the least surface-area growth, AVL-style rotations keep the
// tree balanced. Proxy ids are node indices (stable until Remove). Not thread-safe.
class AabbTree {
public:
    static constexpr std::int32_t kNull = -1;

    [[nodiscard]] std::int32_t Insert(const Aabb& box, std::uint32_t userData);
    void                       Remove(std::int32_t proxy);
    // New tight bounds; true if the leaf had to be re-inserted.
    bool Move(std::int32_t proxy, const Aabb& box);
    void SetUserData(std::int32_t proxy, std::uint32_t userData) { m_Nodes[proxy].userData = userData; }
    void Clear();

    [[nodiscard]] const Aabb&   FatAabb(std::int32_t proxy) const { return m_Nodes[proxy].box; }
    [[nodiscard]] std::uint32_t UserData(std::int32_t proxy) const { return m_Nodes[proxy].userData; }
    [[nodiscard]] std::int32_t  Height() const { return m_Root == kNull ? -1 : m_Nodes[m_Root].height; }
    [[nodiscard]] std::size_t   LeafCount() const { return m_LeafCount; }
    [[nodiscard]] std::size_t   NodeCount() const { return m_NodeCount; }
    // Structure and bounds invariants (tests).
    [[nodiscard]] bool Validate() const;

    // fn(proxy) for every leaf whose fat AABB overlaps the box / frustum.
    template <class F>
    void Query(const Aabb& box, F&& fn) const;
    template <class F>
    void Query(const Frustum& frustum, F&& fn) const;
    // fn(proxy, maxT) -> float for every leaf whose fat AABB the ray origin + t * dir, t in
    // [0, maxT] hits; the callback returns the new maxT (clip to a hit, or keep maxT).
    template <class F>
    void Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxT, F&& fn) const;
    // Debug: fn(fatAabb, depth, isLeaf) for every node, parents first.
    template <class F>
    void ForEachNode(F&& fn) const;

    // Ray vs box (slab test): entry distance in [0, maxT], or a negative value for a miss.
    [[nodiscard]] static float RayBox(const glm::vec3& origin, const glm::vec3& invDir, const Aabb& box, float maxT);

private:
    struct Node {
        Aabb          box;
        std::int32_t  parent = kNull; // next free node while unused
        std::int32_t  child1 = kNull;
        std::int32_t  child2 = kNull;
        std::int32_t  height = -1; // leaf 0, free -1
        std::uint32_t userData = 0;
        [[nodiscard]] bool IsLeaf() const { return child1 == kNull; }
    };

    std::int32_t Allocate();
    void         Free(std::int32_t node);
    void         InsertLeaf(std::int32_t leaf);
    void         RemoveLeaf(std::int32_t leaf);
    std::int32_t Balance(std::int32_t node);
    void         Refit(std::int32_t node); // walk up: balance, heights, boxes

    std::vector<Node> m_Nodes;
    std::int32_t      m_Root      = kNull;
    std::int32_t      m_FreeList  = kNull;
    std::size_t       m_LeafCount = 0;
    std::size_t       m_NodeCount = 0;
};

// --- Templates --------------------------------------------------------------------------------

namespace detail {
[[nodiscard]] inline bool Overlaps(const Aabb& a, const Aabb& b)
{
    return glm::all(glm::lessThanEqual(a.min, b.max)) && glm::all(glm::lessThanEqual(b.min, a.max));
}
} // namespace detail

template <class F>
void AabbTree::Query(const Aabb& box, F&& fn) const
{
    if (m_Root == kNull)
        return;
    std::vector<std::int32_t> stack{m_Root};
    while (!stack.empty()) {
        const Node& node = m_Nodes[stack.back()];
        const auto  id   = stack.back();
        stack.pop_back();
        if (!detail::Overlaps(node.box, box))
            continue;
        if (node.IsLeaf()) {
            fn(id);
        } else {
            stack.push_back(node.child1);
            stack.push_back(node.child2);
        }
    }
}

template <class F>
void AabbTree::Query(const Frustum& frustum, F&& fn) const
{
    if (m_Root == kNull)
        return;
    std::vector<std::int32_t> stack{m_Root};
    while (!stack.empty()) {
        const auto  id   = stack.back();
        const Node& node = m_Nodes[id];
        stack.pop_back();
        if (!frustum.Intersects(node.box))
            continue;
        if (node.IsLeaf()) {
            fn(id);
        } else {
            stack.push_back(node.child1);
            stack.push_back(node.child2);
        }
    }
}

template <class F>
void AabbTree::Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxT, F&& fn) const
{
    if (m_Root == kNull)
        return;
    const glm::vec3           invDir = 1.0f / dir; // +-inf for axis-parallel rays: the slab test handles it
    std::vector<std::int32_t> stack{m_Root};
    while (!stack.empty()) {
        const auto  id   = stack.back();
        const Node& node = m_Nodes[id];
        stack.pop_back();
        if (RayBox(origin, invDir, node.box, maxT) < 0.0f)
            continue;
        if (node.IsLeaf()) {
            maxT = fn(id, maxT);
        } else {
            stack.push_back(node.child1);
            stack.push_back(node.child2);
        }
    }
}

template <class F>
void AabbTree::ForEachNode(F&& fn) const
{
    if (m_Root == kNull)
        return;
    std::vector<std::pair<std::int32_t, int>> stack{{m_Root, 0}};
    while (!stack.empty()) {
        const auto [id, depth] = stack.back();
        stack.pop_back();
        const Node& node = m_Nodes[id];
        fn(node.box, depth, node.IsLeaf());
        if (!node.IsLeaf()) {
            stack.push_back({node.child2, depth + 1});
            stack.push_back({node.child1, depth + 1});
        }
    }
}

} // namespace Engine
