#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

struct Name {
    std::string value;
};

// Local TRS relative to the parent.
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // w, x, y, z
    glm::vec3 scale{1.0f};

    [[nodiscard]] glm::mat4 LocalMatrix() const;
};

// Written by Scene::UpdateTransforms(); read-only for everyone else.
struct WorldTransform {
    glm::mat4 matrix{1.0f};
};

struct Hierarchy {
    Entity              parent = NullEntity;
    std::vector<Entity> children;
};

// Resolved through the AssetManager each frame; renders nothing until the model is Ready.
struct MeshRenderer {
    ModelHandle   model;
    std::uint32_t meshIndex = 0;
};

enum class LightType : std::uint8_t { Point, Spot };

// Punctual light with KHR_lights_punctual semantics. Position and direction (local -Z) come from
// the WorldTransform. Intensity is in candela on the same scale as the sun's illuminance
// (SkySettings::sunIntensity): at distance d the illuminance is intensity / d^2.
struct Light {
    LightType type           = LightType::Point;
    glm::vec3 color{1.0f};
    float     intensity      = 10.0f;
    float     range          = 0.0f; // world units; 0 = derived from the intensity (see EffectiveRange)
    float     innerConeAngle = 0.0f; // spot, radians from the axis: full intensity inside
    float     outerConeAngle = glm::quarter_pi<float>(); // spot: zero outside
};

// Illuminance below which a light without explicit range is cut off (lights need a finite range
// for clustering).
inline constexpr float kLightCutoffIlluminance = 0.005f;

[[nodiscard]] inline float EffectiveRange(const Light& light)
{
    if (light.range > 0.0f)
        return light.range;
    const float peak = light.intensity * std::max({light.color.r, light.color.g, light.color.b});
    return std::sqrt(std::max(peak, 0.0f) / kLightCutoffIlluminance);
}

} // namespace Engine
