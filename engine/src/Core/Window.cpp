#include "Engine/Core/Window.h"
#include "Engine/Events/EventBus.h"
#include "Engine/Events/Events.h"

#include <GLFW/glfw3.h> // after volk.h so the Vulkan-specific GLFW API is declared

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

    m_Handle = glfwCreateWindow(static_cast<int>(desc.width), static_cast<int>(desc.height),
                                desc.title.c_str(), nullptr, nullptr);
    if (!m_Handle) {
        glfwTerminate();
        throw std::runtime_error("glfwCreateWindow failed");
    }

    glfwSetWindowUserPointer(m_Handle, this);
    InstallCallbacks();
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

void Window::PollEvents() const { glfwPollEvents(); }
void Window::WaitEvents() const { glfwWaitEvents(); }
void Window::RequestClose() const { glfwSetWindowShouldClose(m_Handle, GLFW_TRUE); }
void Window::SetCursorCaptured(bool captured) const
{
    glfwSetInputMode(m_Handle, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(m_Handle, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
}

void Window::SetTitle(const std::string& title) const { glfwSetWindowTitle(m_Handle, title.c_str()); }

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
