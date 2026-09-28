#include "Engine/Renderer/ShadowCascades.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Engine {

std::array<float, kMaxCascades> ComputeCascadeSplits(float nearPlane, float farPlane, std::uint32_t count,
                                                     float lambda)
{
    std::array<float, kMaxCascades> splits{};
    count = std::clamp(count, 1u, kMaxCascades);
    for (std::uint32_t i = 0; i < count; ++i) {
        const float p       = static_cast<float>(i + 1) / static_cast<float>(count);
        const float log     = nearPlane * std::pow(farPlane / nearPlane, p);
        const float uniform = nearPlane + (farPlane - nearPlane) * p;
        splits[i]           = lambda * log + (1.0f - lambda) * uniform;
    }
    splits[count - 1] = farPlane; // exact, no rounding drift
    return splits;
}

glm::mat4 OrthoReverseZ(float left, float right, float bottom, float top, float zNear, float zFar)
{
    glm::mat4 m{1.0f};
    m[0][0] = 2.0f / (right - left);
    m[1][1] = 2.0f / (top - bottom);
    m[2][2] = 1.0f / (zFar - zNear); // z_view = -near -> 1, -far -> 0
    m[3][0] = -(right + left) / (right - left);
    m[3][1] = -(top + bottom) / (top - bottom);
    m[3][2] = zFar / (zFar - zNear);
    return m;
}

std::array<Cascade, kMaxCascades> ComputeCascades(const CameraData& camera, const glm::vec3& lightDirection,
                                                  const ShadowSettings& settings)
{
    const std::uint32_t count = std::clamp(settings.cascadeCount, 1u, kMaxCascades);
    const std::array<float, kMaxCascades> splits =
        ComputeCascadeSplits(camera.nearPlane, settings.maxDistance, count, settings.splitLambda);

    // Frustum slice geometry from the (possibly infinite) perspective matrix.
    const float     tanX        = 1.0f / camera.projection[0][0];
    const float     tanY        = 1.0f / camera.projection[1][1];
    const glm::mat4 cameraWorld = glm::inverse(camera.view);

    // Light rotation only depends on the direction -> texel snapping is stable in its frame.
    const glm::vec3 dir = glm::normalize(lightDirection);
    const glm::vec3 up  = std::abs(dir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 lightView = glm::lookAt(glm::vec3(0.0f), dir, up);

    std::array<Cascade, kMaxCascades> cascades{};
    float sliceNear = camera.nearPlane;
    for (std::uint32_t i = 0; i < count; ++i) {
        const float sliceFar = splits[i];

        // Bounding sphere of the slice: center on the view axis, radius to the far corners. Depends
        // only on the slice distances and FOV, so it does not change when the camera rotates.
        glm::vec3 corners[8];
        int       n = 0;
        for (const float d : {sliceNear, sliceFar})
            for (const float sx : {-1.0f, 1.0f})
                for (const float sy : {-1.0f, 1.0f})
                    corners[n++] = glm::vec3(cameraWorld * glm::vec4(sx * d * tanX, sy * d * tanY, -d, 1.0f));
        glm::vec3 center(0.0f);
        for (const glm::vec3& c : corners)
            center += c * (1.0f / 8.0f);
        float radius = 0.0f;
        for (const glm::vec3& c : corners)
            radius = std::max(radius, glm::length(c - center));
        radius = std::ceil(radius * 16.0f) / 16.0f; // quantize: float noise must not resize the map

        // Snap the center to whole texels in light space.
        const float texel = 2.0f * radius / static_cast<float>(settings.resolution);
        glm::vec3   lc    = glm::vec3(lightView * glm::vec4(center, 1.0f));
        lc.x              = std::floor(lc.x / texel) * texel;
        lc.y              = std::floor(lc.y / texel) * texel;

        // Light view looks down -Z: the sphere spans view depths [-lc.z - radius, -lc.z + radius].
        // Casters in front of the near plane are depth-clamped onto it (pancaking).
        const glm::mat4 proj = OrthoReverseZ(lc.x - radius, lc.x + radius, lc.y - radius, lc.y + radius,
                                             -lc.z - radius, -lc.z + radius);
        cascades[i] = {.viewProj = proj * lightView, .splitFar = sliceFar, .texelWorldSize = texel};
        sliceNear   = sliceFar;
    }
    return cascades;
}

} // namespace Engine
