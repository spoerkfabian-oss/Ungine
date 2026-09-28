#include "Engine/Scene/Frustum.h"

namespace Engine {

Aabb TransformAabb(const Aabb& box, const glm::mat4& m)
{
    const glm::vec3 center  = (box.min + box.max) * 0.5f;
    const glm::vec3 extents = (box.max - box.min) * 0.5f;

    const glm::vec3 newCenter = glm::vec3(m * glm::vec4(center, 1.0f));
    const glm::mat3 absM{glm::abs(glm::vec3(m[0])), glm::abs(glm::vec3(m[1])), glm::abs(glm::vec3(m[2]))};
    const glm::vec3 newExtents = absM * extents;
    return {newCenter - newExtents, newCenter + newExtents};
}

Frustum Frustum::FromViewProjection(const glm::mat4& vp, bool clipNear)
{
    // glm is column-major: row i = (vp[0][i], vp[1][i], vp[2][i], vp[3][i]).
    const auto row = [&](int i) { return glm::vec4(vp[0][i], vp[1][i], vp[2][i], vp[3][i]); };
    const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);

    Frustum f;
    f.m_Planes = {r3 + r0,  // left:   -w <= x
                  r3 - r0,  // right:   x <= w
                  r3 + r1,  // bottom: -w <= y
                  r3 - r1,  // top:     y <= w
                  r2,       // z >= 0: far plane (infinite far with reverse-Z -> always true)
                  r3 - r2}; // z <= w: near plane with reverse-Z
    if (!clipNear)
        f.m_Planes[5] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); // always inside
    return f;
}

bool Frustum::Intersects(const Aabb& box) const
{
    const glm::vec3 center  = (box.min + box.max) * 0.5f;
    const glm::vec3 extents = (box.max - box.min) * 0.5f;
    for (const glm::vec4& p : m_Planes) {
        const glm::vec3 n = glm::vec3(p);
        // Signed distance of the center plus the box's projected radius onto the normal.
        if (glm::dot(n, center) + p.w + glm::dot(glm::abs(n), extents) < 0.0f)
            return false;
    }
    return true;
}

} // namespace Engine
