#pragma once
#include <glm/glm.hpp>

#include <array>

namespace Engine {

struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

// Bounds of the transformed box (Arvo 1990). Conservative for rotations.
[[nodiscard]] Aabb TransformAabb(const Aabb& box, const glm::mat4& transform);

// Planes of a Vulkan clip-space frustum (-w <= x,y <= w, 0 <= z <= w). Works with reverse-Z and
// an infinite far plane: that plane degenerates to "always inside".
class Frustum {
public:
    [[nodiscard]] static Frustum FromViewProjection(const glm::mat4& viewProjection);

    // False only if the box is completely outside one plane (may keep some invisible boxes).
    [[nodiscard]] bool Intersects(const Aabb& box) const;

private:
    std::array<glm::vec4, 6> m_Planes{}; // dot(xyz, p) + w >= 0 inside; not normalized (sign test only)
};

} // namespace Engine
