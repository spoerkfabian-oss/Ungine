#pragma once
#include <glm/glm.hpp>

namespace Engine {

class Input;
class Window;

struct CameraData {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 position{0.0f};
    float     nearPlane = 0.05f;
};

// Reverse-Z, infinite far plane, Vulkan [0,1] depth. Near maps to 1, infinity to 0.
// Y is not flipped here: the engine uses a negative-height viewport instead.
[[nodiscard]] glm::mat4 PerspectiveReverseZ(float fovY, float aspect, float zNear);

// Free-fly editor camera. Hold RMB to look, WASD/QE to move, Shift = fast, scroll = speed.
class FlyCamera {
public:
    glm::vec3 position{0.0f, 1.0f, 3.0f};
    float     yaw         = 0.0f; // radians, 0 = looking down -Z
    float     pitch       = 0.0f;
    float     fovY        = glm::radians(60.0f);
    float     nearPlane   = 0.05f;
    float     moveSpeed   = 3.0f;  // m/s
    float     sensitivity = 0.0025f;
    bool      moveRequiresLook = false; // editor style: WASD/QE only while RMB is held

    void Update(Input& input, const Window& window, float dt);
    void LookAt(const glm::vec3& target);

    [[nodiscard]] glm::vec3  Forward() const;
    [[nodiscard]] CameraData GetData(float aspect) const;
    [[nodiscard]] bool       IsCaptured() const { return m_Captured; } // mouse look active (cursor hidden)

private:
    bool m_Captured = false;
};

} // namespace Engine
