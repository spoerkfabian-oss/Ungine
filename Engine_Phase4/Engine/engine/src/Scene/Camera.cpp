#include "Engine/Scene/Camera.h"
#include "Engine/Core/Input.h"
#include "Engine/Core/Window.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Engine {

glm::mat4 PerspectiveReverseZ(float fovY, float aspect, float zNear)
{
    const float f = 1.0f / std::tan(fovY * 0.5f);
    glm::mat4   m{0.0f};
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][3] = -1.0f;  // w_clip = -z_view (right-handed, looking down -Z)
    m[3][2] = zNear;  // z_clip = near  ->  depth = near / -z_view
    return m;
}

glm::vec3 FlyCamera::Forward() const
{
    return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw)};
}

void FlyCamera::LookAt(const glm::vec3& target)
{
    const glm::vec3 dir = glm::normalize(target - position);
    pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    yaw   = std::atan2(dir.x, -dir.z);
}

void FlyCamera::Update(Input& input, const Window& window, float dt)
{
    const bool wantCapture = input.IsMouseDown(MouseButton::Right);
    if (wantCapture != m_Captured) {
        window.SetCursorCaptured(wantCapture);
        input.SuppressNextMouseDelta(); // GLFW warps the cursor on mode change
        m_Captured = wantCapture;
    }

    if (m_Captured) {
        const glm::vec2 d = input.MouseDelta();
        yaw += d.x * sensitivity;
        pitch = std::clamp(pitch - d.y * sensitivity, glm::radians(-89.0f), glm::radians(89.0f));
    }

    if (input.ScrollDelta() != 0.0f)
        moveSpeed = std::clamp(moveSpeed * std::pow(1.2f, input.ScrollDelta()), 0.05f, 500.0f);

    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    const glm::vec3 forward = Forward();
    const glm::vec3 right   = glm::normalize(glm::cross(forward, up));

    glm::vec3 move{0.0f};
    if (input.IsKeyDown(Key::W)) move += forward;
    if (input.IsKeyDown(Key::S)) move -= forward;
    if (input.IsKeyDown(Key::D)) move += right;
    if (input.IsKeyDown(Key::A)) move -= right;
    if (input.IsKeyDown(Key::E)) move += up;
    if (input.IsKeyDown(Key::Q)) move -= up;

    if (glm::dot(move, move) > 0.0f) {
        const float boost = input.IsKeyDown(Key::LeftShift) ? 4.0f : 1.0f;
        position += glm::normalize(move) * moveSpeed * boost * dt;
    }
}

CameraData FlyCamera::GetData(float aspect) const
{
    return {.view       = glm::lookAt(position, position + Forward(), glm::vec3{0.0f, 1.0f, 0.0f}),
            .projection = PerspectiveReverseZ(fovY, aspect, nearPlane),
            .position   = position,
            .nearPlane  = nearPlane};
}

} // namespace Engine
