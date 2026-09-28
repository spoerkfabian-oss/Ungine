#include "Engine/Scene/AabbTree.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace Engine {

namespace {

Aabb Union(const Aabb& a, const Aabb& b)
{
    return {glm::min(a.min, b.min), glm::max(a.max, b.max)};
}

float Area(const Aabb& b)
{
    const glm::vec3 d = b.max - b.min;
    return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
}

bool Contains(const Aabb& outer, const Aabb& inner)
{
    return glm::all(glm::lessThanEqual(outer.min, inner.min)) && glm::all(glm::lessThanEqual(inner.max, outer.max));
}

// Margin that absorbs small moves: 10 % of the extent per axis (scale independent), at least 1e-4.
Aabb Fatten(const Aabb& box)
{
    const glm::vec3 margin = glm::max((box.max - box.min) * 0.1f, glm::vec3(1e-4f));
    return {box.min - margin, box.max + margin};
}

} // namespace

float AabbTree::RayBox(const glm::vec3& origin, const glm::vec3& invDir, const Aabb& box, float maxT)
{
    const glm::vec3 t0 = (box.min - origin) * invDir;
    const glm::vec3 t1 = (box.max - origin) * invDir;
    // Axis-parallel rays give +-inf per slab; a ray exactly in a slab's boundary plane (0 * inf =
    // NaN) may be reported as a miss (measure zero, acceptable for picking and queries).
    const glm::vec3 lo = glm::min(t0, t1);
    const glm::vec3 hi = glm::max(t0, t1);
    const float     tNear = std::max({lo.x, lo.y, lo.z, 0.0f});
    const float     tFar  = std::min({hi.x, hi.y, hi.z, maxT});
    return tNear <= tFar ? tNear : -1.0f;
}

std::int32_t AabbTree::Allocate()
{
    if (m_FreeList == kNull) {
        m_Nodes.emplace_back();
        m_FreeList = static_cast<std::int32_t>(m_Nodes.size() - 1);
        m_Nodes.back().parent = kNull;
    }
    const std::int32_t id = m_FreeList;
    m_FreeList            = m_Nodes[id].parent;
    m_Nodes[id]           = Node{};
    m_Nodes[id].height    = 0;
    ++m_NodeCount;
    return id;
}

void AabbTree::Free(std::int32_t node)
{
    m_Nodes[node].height = -1;
    m_Nodes[node].parent = m_FreeList;
    m_FreeList           = node;
    --m_NodeCount;
}

std::int32_t AabbTree::Insert(const Aabb& box, std::uint32_t userData)
{
    const std::int32_t leaf = Allocate();
    m_Nodes[leaf].box       = Fatten(box);
    m_Nodes[leaf].userData  = userData;
    InsertLeaf(leaf);
    ++m_LeafCount;
    return leaf;
}

void AabbTree::Remove(std::int32_t proxy)
{
    assert(proxy >= 0 && proxy < static_cast<std::int32_t>(m_Nodes.size()) && m_Nodes[proxy].IsLeaf());
    RemoveLeaf(proxy);
    Free(proxy);
    --m_LeafCount;
}

bool AabbTree::Move(std::int32_t proxy, const Aabb& box)
{
    if (Contains(m_Nodes[proxy].box, box))
        return false;
    RemoveLeaf(proxy);
    m_Nodes[proxy].box = Fatten(box);
    InsertLeaf(proxy);
    return true;
}

void AabbTree::Clear()
{
    m_Nodes.clear();
    m_Root      = kNull;
    m_FreeList  = kNull;
    m_LeafCount = 0;
    m_NodeCount = 0;
}

void AabbTree::InsertLeaf(std::int32_t leaf)
{
    if (m_Root == kNull) {
        m_Root                = leaf;
        m_Nodes[leaf].parent  = kNull;
        return;
    }

    // Descend towards the sibling with the lowest cost: the new parent's area plus the area growth
    // it causes in every ancestor (Box2D's branch-and-bound heuristic, surface area in 3D).
    const Aabb   leafBox = m_Nodes[leaf].box;
    std::int32_t index   = m_Root;
    while (!m_Nodes[index].IsLeaf()) {
        const Node& node        = m_Nodes[index];
        const float area        = Area(node.box);
        const float combined    = Area(Union(node.box, leafBox));
        const float cost        = 2.0f * combined;             // new parent here
        const float inheritance = 2.0f * (combined - area);    // pushing the leaf further down
        const auto  childCost   = [&](std::int32_t child) {
            const Aabb& b = m_Nodes[child].box;
            return m_Nodes[child].IsLeaf() ? Area(Union(leafBox, b)) + inheritance
                                           : Area(Union(leafBox, b)) - Area(b) + inheritance;
        };
        const float cost1 = childCost(node.child1);
        const float cost2 = childCost(node.child2);
        if (cost < cost1 && cost < cost2)
            break;
        index = cost1 < cost2 ? node.child1 : node.child2;
    }

    const std::int32_t sibling   = index;
    const std::int32_t oldParent = m_Nodes[sibling].parent;
    const std::int32_t newParent = Allocate(); // may reallocate m_Nodes: no references across this
    m_Nodes[newParent].parent    = oldParent;
    m_Nodes[newParent].box       = Union(leafBox, m_Nodes[sibling].box);
    m_Nodes[newParent].height    = m_Nodes[sibling].height + 1;
    m_Nodes[newParent].child1    = sibling;
    m_Nodes[newParent].child2    = leaf;
    m_Nodes[sibling].parent      = newParent;
    m_Nodes[leaf].parent         = newParent;
    if (oldParent == kNull) {
        m_Root = newParent;
    } else if (m_Nodes[oldParent].child1 == sibling) {
        m_Nodes[oldParent].child1 = newParent;
    } else {
        m_Nodes[oldParent].child2 = newParent;
    }
    Refit(m_Nodes[leaf].parent);
}

void AabbTree::RemoveLeaf(std::int32_t leaf)
{
    if (leaf == m_Root) {
        m_Root = kNull;
        return;
    }
    const std::int32_t parent      = m_Nodes[leaf].parent;
    const std::int32_t grandParent = m_Nodes[parent].parent;
    const std::int32_t sibling     = m_Nodes[parent].child1 == leaf ? m_Nodes[parent].child2 : m_Nodes[parent].child1;

    if (grandParent == kNull) {
        m_Root                  = sibling;
        m_Nodes[sibling].parent = kNull;
        Free(parent);
        return;
    }
    if (m_Nodes[grandParent].child1 == parent)
        m_Nodes[grandParent].child1 = sibling;
    else
        m_Nodes[grandParent].child2 = sibling;
    m_Nodes[sibling].parent = grandParent;
    Free(parent);
    Refit(grandParent);
}

void AabbTree::Refit(std::int32_t index)
{
    while (index != kNull) {
        index             = Balance(index);
        Node&        node = m_Nodes[index];
        const Node&  c1   = m_Nodes[node.child1];
        const Node&  c2   = m_Nodes[node.child2];
        node.height       = 1 + std::max(c1.height, c2.height);
        node.box          = Union(c1.box, c2.box);
        index             = node.parent;
    }
}

// Rotates the taller grandchild up if the subtree at iA is unbalanced; returns the subtree root.
std::int32_t AabbTree::Balance(std::int32_t iA)
{
    Node& A = m_Nodes[iA];
    if (A.IsLeaf() || A.height < 2)
        return iA;

    const std::int32_t iB      = A.child1;
    const std::int32_t iC      = A.child2;
    Node&              B       = m_Nodes[iB];
    Node&              C       = m_Nodes[iC];
    const std::int32_t balance = C.height - B.height;

    const auto rotateUp = [&](std::int32_t iUp, Node& up, std::int32_t iOther, bool upIsChild2) {
        // `up` (child of A) replaces A; its taller child stays with it, the shorter one goes to A.
        const std::int32_t iF = up.child1;
        const std::int32_t iG = up.child2;
        Node&              F  = m_Nodes[iF];
        Node&              G  = m_Nodes[iG];

        up.child1 = iA;
        up.parent = A.parent;
        A.parent  = iUp;
        if (up.parent == kNull)
            m_Root = iUp;
        else if (m_Nodes[up.parent].child1 == iA)
            m_Nodes[up.parent].child1 = iUp;
        else
            m_Nodes[up.parent].child2 = iUp;

        const bool         keepF   = F.height > G.height;
        const std::int32_t iKeep   = keepF ? iF : iG;
        const std::int32_t iGive   = keepF ? iG : iF;
        Node&              keep    = m_Nodes[iKeep];
        Node&              give    = m_Nodes[iGive];
        up.child2                  = iKeep;
        if (upIsChild2)
            A.child2 = iGive;
        else
            A.child1 = iGive;
        give.parent = iA;
        const Node& other = m_Nodes[iOther];
        A.box             = Union(other.box, give.box);
        A.height          = 1 + std::max(other.height, give.height);
        up.box            = Union(A.box, keep.box);
        up.height         = 1 + std::max(A.height, keep.height);
        return iUp;
    };

    if (balance > 1)
        return rotateUp(iC, C, iB, true);
    if (balance < -1)
        return rotateUp(iB, B, iC, false);
    return iA;
}

bool AabbTree::Validate() const
{
    if (m_Root == kNull)
        return m_LeafCount == 0 && m_NodeCount == 0;
    if (m_Nodes[m_Root].parent != kNull)
        return false;
    std::size_t               leaves = 0, nodes = 0;
    std::vector<std::int32_t> stack{m_Root};
    while (!stack.empty()) {
        const std::int32_t id = stack.back();
        stack.pop_back();
        const Node& n = m_Nodes[id];
        ++nodes;
        if (n.IsLeaf()) {
            ++leaves;
            if (n.height != 0 || n.child2 != kNull)
                return false;
            continue;
        }
        const Node& c1 = m_Nodes[n.child1];
        const Node& c2 = m_Nodes[n.child2];
        if (c1.parent != id || c2.parent != id)
            return false;
        if (n.height != 1 + std::max(c1.height, c2.height))
            return false;
        if (!Contains(n.box, c1.box) || !Contains(n.box, c2.box))
            return false;
        stack.push_back(n.child1);
        stack.push_back(n.child2);
    }
    return leaves == m_LeafCount && nodes == m_NodeCount;
}

} // namespace Engine
