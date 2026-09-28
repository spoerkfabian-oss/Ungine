#pragma once
#include "Engine/Renderer/Vulkan/VkCommon.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

struct GLFWwindow;

namespace Engine {

class EventBus;

struct WindowDesc {
    std::string   title     = "Engine";
    std::uint32_t width     = 1600;
    std::uint32_t height    = 900;
    bool          resizable = true;
};

// Single-window platform layer. Owns GLFW lifetime (one Window per process).
// All GLFW callbacks are translated into events on the EventBus.
class Window {
public:
    Window(const WindowDesc& desc, EventBus& events);
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&)                 = delete; // GLFW user pointer refers to `this`
    Window& operator=(Window&&)      = delete;

    void PollEvents() const;
    void WaitEvents() const;
    void RequestClose() const;
    void SetTitle(const std::string& title) const;
    void SetCursorCaptured(bool captured) const; // hidden + raw motion (FPS-style look)

    [[nodiscard]] bool       ShouldClose() const;
    [[nodiscard]] bool       IsMinimized() const;
    [[nodiscard]] VkExtent2D FramebufferExtent() const;
    [[nodiscard]] glm::vec2  WindowSize() const; // screen coordinates (mouse positions), may differ on HiDPI

    [[nodiscard]] VkSurfaceKHR CreateSurface(VkInstance instance) const;
    [[nodiscard]] GLFWwindow*  Native() const { return m_Handle; }

private:
    void InstallCallbacks();
    static EventBus& BusOf(GLFWwindow* handle);

    GLFWwindow* m_Handle = nullptr;
    EventBus*   m_Events = nullptr;
};

} // namespace Engine
