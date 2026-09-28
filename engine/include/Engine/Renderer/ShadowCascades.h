#pragma once
#include "Engine/Scene/Camera.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Engine {

inline constexpr std::uint32_t kMaxCascades = 4; // mirrored in frame.glsl

struct ShadowSettings {
    bool          enabled       = true;
    std::uint32_t cascadeCount  = 4;     // 1..kMaxCascades
    std::uint32_t resolution    = 4096;  // per cascade (square); changing it recreates the map
    float         maxDistance   = 60.0f; // view distance covered by the last cascade (world units)
    float         splitLambda   = 0.8f;  // 0 = uniform, 1 = logarithmic split distribution
    float         depthBias     = 1.0f;  // rasterizer constant bias, in depth units
    float         slopeBias     = 2.0f;  // rasterizer slope-scaled bias
    float         normalBias    = 1.5f;  // receiver offset along its normal, in shadow texels
    float         filterRadius  = 1.5f;  // PCF radius, in shadow texels
    float         cascadeBlend  = 0.1f;  // fraction of each cascade cross-faded into the next
    bool          debugCascades = false; // tint surfaces by cascade
};

struct Cascade {
    glm::mat4 viewProj{1.0f};     // world -> light clip space (reverse-Z orthographic)
    float     splitFar       = 0; // view-space distance where the cascade ends
    float     texelWorldSize = 0; // world units per shadow texel
};

// Practical split scheme (GPU Gems 3, ch. 10): per split i, lambda * logarithmic + (1 - lambda) * uniform.
// Returns the far distance of each cascade; entries past `count` are unused.
[[nodiscard]] std::array<float, kMaxCascades> ComputeCascadeSplits(float nearPlane, float farPlane,
                                                                  std::uint32_t count, float lambda);

// Reverse-Z orthographic projection for Vulkan: view-space depth -near -> 1, -far -> 0.
[[nodiscard]] glm::mat4 OrthoReverseZ(float left, float right, float bottom, float top, float zNear, float zFar);

// Stable cascades: each covers the bounding sphere of its view-frustum slice (size independent of
// camera rotation) and is snapped to whole shadow texels in light space (no shimmering on camera
// movement). `lightDirection` is the direction the light travels.
[[nodiscard]] std::array<Cascade, kMaxCascades> ComputeCascades(const CameraData& camera,
                                                                const glm::vec3& lightDirection,
                                                                const ShadowSettings& settings);

} // namespace Engine
