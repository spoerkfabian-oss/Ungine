#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

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

} // namespace Engine
