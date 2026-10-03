#pragma once
#include "Engine/Renderer/Vulkan/VkCommon.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

namespace Engine {

class EventBus;

struct WindowDesc {
    std::string   title      = "Engine";
    std::uint32_t width      = 1600;
    std::uint32_t height     = 900;
    bool          resizable  = true;
    bool          fullscreen = false; // on the primary monitor at its current video mode
    bool          maximized  = false;
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
    void WaitEvents(double timeoutSeconds) const; // wakes up after the timeout at the latest
    void RequestClose() const;
    void CancelClose() const; // from a WindowCloseEvent handler: keep running (e.g. unsaved changes)
    void SetTitle(const std::string& title) const;
    void SetCursorCaptured(bool captured) const; // hidden + raw motion (FPS-style look)
    void SetFullscreen(bool fullscreen);          // primary monitor <-> the last windowed placement
    [[nodiscard]] bool IsFullscreen() const;
    // RGBA8 icons of different sizes (the system picks one). The engine icon is set by default.
    struct Icon {
        std::uint32_t             size = 0; // square
        std::vector<std::uint8_t> rgba;
    };
    void SetIcons(const std::vector<Icon>& icons) const;
    // The Ungine logo, drawn procedurally (the files in resources/ are made from the same design).
    [[nodiscard]] static Icon EngineIcon(std::uint32_t size);

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
    int         m_WindowedX = 100, m_WindowedY = 100, m_WindowedW = 1600, m_WindowedH = 900;
    mutable bool m_GamepadConnected = false;
};

} // namespace Engine
