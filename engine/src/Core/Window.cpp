#include "Engine/Core/Window.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Events/Events.h"

#include <GLFW/glfw3.h> // after volk.h so the Vulkan-specific GLFW API is declared

#include <algorithm>
#include <cmath>
#include <stdexcept>

static_assert(Engine::Key::Escape == GLFW_KEY_ESCAPE && Engine::Key::F1 == GLFW_KEY_F1);
static_assert(Engine::Key::LeftShift == GLFW_KEY_LEFT_SHIFT && Engine::Key::Last == GLFW_KEY_LAST);
static_assert(Engine::MouseButton::Right == GLFW_MOUSE_BUTTON_RIGHT && Engine::MouseButton::Last == GLFW_MOUSE_BUTTON_LAST);
static_assert(static_cast<int>(Engine::InputAction::Repeat) == GLFW_REPEAT);

namespace Engine {

namespace {
void GlfwErrorCallback(int code, const char* description)
{
    ENGINE_ERROR("[GLFW {}] {}", code, description);
}
} // namespace

Window::Window(const WindowDesc& desc, EventBus& events)
    : m_Events(&events)
{
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit())
        throw std::runtime_error("glfwInit failed");
    if (!glfwVulkanSupported()) {
        glfwTerminate();
        throw std::runtime_error("No Vulkan loader/ICD found");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_MAXIMIZED, desc.maximized ? GLFW_TRUE : GLFW_FALSE);

    m_WindowedW = static_cast<int>(desc.width);
    m_WindowedH = static_cast<int>(desc.height);
    GLFWmonitor*       monitor = desc.fullscreen ? glfwGetPrimaryMonitor() : nullptr;
    const GLFWvidmode* mode    = monitor ? glfwGetVideoMode(monitor) : nullptr;
    if (mode) { // "windowed full screen": the current video mode, no mode switch
        glfwWindowHint(GLFW_RED_BITS, mode->redBits);
        glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
        glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
        glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
    }
    m_Handle = glfwCreateWindow(mode ? mode->width : m_WindowedW, mode ? mode->height : m_WindowedH, desc.title.c_str(),
                                mode ? monitor : nullptr, nullptr);
    if (!m_Handle) {
        glfwTerminate();
        throw std::runtime_error("glfwCreateWindow failed");
    }

    glfwSetWindowUserPointer(m_Handle, this);
    InstallCallbacks();
    SetIcons({EngineIcon(16), EngineIcon(32), EngineIcon(48), EngineIcon(128)});
}

void Window::SetIcons(const std::vector<Icon>& icons) const
{
#ifdef __APPLE__
    (void)icons; // the bundle icon is used
#else
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
        return; // not supported there (the desktop file names the icon)
    std::vector<GLFWimage> images;
    for (const Icon& icon : icons)
        if (icon.size > 0 && icon.rgba.size() == std::size_t{icon.size} * icon.size * 4)
            images.push_back({static_cast<int>(icon.size), static_cast<int>(icon.size),
                              const_cast<unsigned char*>(icon.rgba.data())});
    if (!images.empty())
        glfwSetWindowIcon(m_Handle, static_cast<int>(images.size()), images.data());
#endif
}

Window::Icon Window::EngineIcon(std::uint32_t size)
{
    // Signed distances in [-1, 1] (y up): a rounded square with a vertical gradient and a white
    // "U" (two bars joined by a half circle) plus an orange dot.
    Icon icon{size, std::vector<std::uint8_t>(std::size_t{size} * size * 4)};
    const float aa = 2.0f / static_cast<float>(size); // one pixel
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const glm::vec2 p((static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f,
                              1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f);
            // Background: rounded square (radius 0.3) inset by a pixel.
            const glm::vec2 q    = glm::abs(p) - glm::vec2(0.94f - 0.3f);
            const float     box  = glm::length(glm::max(q, 0.0f)) + std::min(std::max(q.x, q.y), 0.0f) - 0.3f;
            const float     bg   = std::clamp(0.5f - box / aa, 0.0f, 1.0f);
            const float     t    = p.y * 0.5f + 0.5f;
            glm::vec3       color = glm::mix(glm::vec3(0.30f, 0.12f, 0.55f), glm::vec3(0.10f, 0.35f, 0.75f), t);
            // The U: stroke of half width 0.13 along the path.
            const float r = 0.38f, top = 0.52f, bottom = -0.08f;
            float       d = 0.0f;
            if (p.y > bottom)
                d = std::min(glm::length(glm::vec2(p.x + r, std::max(p.y - top, 0.0f))),
                             glm::length(glm::vec2(p.x - r, std::max(p.y - top, 0.0f))));
            else
                d = std::abs(glm::length(p - glm::vec2(0.0f, bottom)) - r);
            const float u = std::clamp(0.5f - (d - 0.13f) / aa, 0.0f, 1.0f);
            color         = glm::mix(color, glm::vec3(0.96f, 0.97f, 1.0f), u);
            const float dot = std::clamp(0.5f - (glm::length(p - glm::vec2(0.0f, 0.62f)) - 0.11f) / aa, 0.0f, 1.0f);
            color           = glm::mix(color, glm::vec3(1.0f, 0.62f, 0.15f), dot);
            std::uint8_t* px = &icon.rgba[(std::size_t{y} * size + x) * 4];
            px[0] = static_cast<std::uint8_t>(color.r * 255.0f + 0.5f);
            px[1] = static_cast<std::uint8_t>(color.g * 255.0f + 0.5f);
            px[2] = static_cast<std::uint8_t>(color.b * 255.0f + 0.5f);
            px[3] = static_cast<std::uint8_t>(bg * 255.0f + 0.5f);
        }
    return icon;
}

void Window::SetFullscreen(bool fullscreen)
{
    if (fullscreen == IsFullscreen())
        return;
    if (fullscreen) {
        glfwGetWindowPos(m_Handle, &m_WindowedX, &m_WindowedY);
        glfwGetWindowSize(m_Handle, &m_WindowedW, &m_WindowedH);
        GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode    = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if (mode)
            glfwSetWindowMonitor(m_Handle, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(m_Handle, nullptr, m_WindowedX, m_WindowedY, m_WindowedW, m_WindowedH, GLFW_DONT_CARE);
    }
}

bool Window::IsFullscreen() const { return glfwGetWindowMonitor(m_Handle) != nullptr; }

void Window::SetSize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    if (IsFullscreen()) {
        m_WindowedW = width;
        m_WindowedH = height;
    } else {
        glfwSetWindowSize(m_Handle, width, height);
    }
}

Window::~Window()
{
    if (m_Handle)
        glfwDestroyWindow(m_Handle);
    glfwTerminate();
}

EventBus& Window::BusOf(GLFWwindow* handle)
{
    return *static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_Events;
}

void Window::InstallCallbacks()
{
    glfwSetFramebufferSizeCallback(m_Handle, [](GLFWwindow* w, int width, int height) {
        BusOf(w).Publish(FramebufferResizeEvent{static_cast<std::uint32_t>(width),
                                                static_cast<std::uint32_t>(height)});
    });
    glfwSetWindowCloseCallback(m_Handle, [](GLFWwindow* w) { BusOf(w).Publish(WindowCloseEvent{}); });
    glfwSetWindowFocusCallback(m_Handle, [](GLFWwindow* w, int focused) {
        BusOf(w).Publish(WindowFocusEvent{focused == GLFW_TRUE});
    });
    glfwSetKeyCallback(m_Handle, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
        BusOf(w).Publish(KeyEvent{key, scancode, static_cast<InputAction>(action), mods});
    });
    glfwSetCharCallback(m_Handle, [](GLFWwindow* w, unsigned int codepoint) {
        BusOf(w).Publish(CharEvent{codepoint});
    });
    glfwSetMouseButtonCallback(m_Handle, [](GLFWwindow* w, int button, int action, int mods) {
        BusOf(w).Publish(MouseButtonEvent{button, static_cast<InputAction>(action), mods});
    });
    glfwSetCursorPosCallback(m_Handle, [](GLFWwindow* w, double x, double y) {
        BusOf(w).Publish(MouseMoveEvent{x, y});
    });
    glfwSetScrollCallback(m_Handle, [](GLFWwindow* w, double dx, double dy) {
        BusOf(w).Publish(MouseScrollEvent{dx, dy});
    });
}

void Window::PollEvents() const
{
    glfwPollEvents();
    GamepadStateEvent event{};
    for (int joystick = GLFW_JOYSTICK_1; joystick <= GLFW_JOYSTICK_LAST; ++joystick) {
        if (!glfwJoystickPresent(joystick) || !glfwJoystickIsGamepad(joystick))
            continue;
        GLFWgamepadstate state{};
        if (glfwGetGamepadState(joystick, &state) != GLFW_TRUE)
            continue;
        event.connected = true;
        for (std::size_t i = 0; i < event.buttons.size() && i <= GLFW_GAMEPAD_BUTTON_LAST; ++i)
            event.buttons[i] = state.buttons[i];
        for (std::size_t i = 0; i < event.axes.size() && i <= GLFW_GAMEPAD_AXIS_LAST; ++i)
            event.axes[i] = state.axes[i];
        break; // the first mapped controller is the active UI gamepad
    }
    if (event.connected || m_GamepadConnected)
        m_Events->Publish(event);
    m_GamepadConnected = event.connected;
}
void Window::WaitEvents() const { glfwWaitEvents(); }
void Window::WaitEvents(double timeoutSeconds) const { glfwWaitEventsTimeout(timeoutSeconds); }
void Window::RequestClose() const { glfwSetWindowShouldClose(m_Handle, GLFW_TRUE); }
void Window::SetCursorCaptured(bool captured) const
{
    glfwSetInputMode(m_Handle, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(m_Handle, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
}

void Window::SetTitle(const std::string& title) const { glfwSetWindowTitle(m_Handle, title.c_str()); }

void Window::CancelClose() const { glfwSetWindowShouldClose(m_Handle, GLFW_FALSE); }

bool Window::ShouldClose() const { return glfwWindowShouldClose(m_Handle) == GLFW_TRUE; }

bool Window::IsMinimized() const
{
    const VkExtent2D e = FramebufferExtent();
    return e.width == 0 || e.height == 0 || glfwGetWindowAttrib(m_Handle, GLFW_ICONIFIED);
}

VkExtent2D Window::FramebufferExtent() const
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_Handle, &w, &h);
    return {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}

glm::vec2 Window::WindowSize() const
{
    int w = 0, h = 0;
    glfwGetWindowSize(m_Handle, &w, &h);
    return {static_cast<float>(w), static_cast<float>(h)};
}

VkSurfaceKHR Window::CreateSurface(VkInstance instance) const
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VK_CHECK(glfwCreateWindowSurface(instance, m_Handle, nullptr, &surface));
    return surface;
}

} // namespace Engine
