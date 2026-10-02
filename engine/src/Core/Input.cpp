#include "Engine/Core/Input.h"

namespace Engine {

namespace {
constexpr bool InRange(int v, int last) { return v >= 0 && v <= last; }
} // namespace

Input::Input(EventBus& events)
{
    m_KeySub = events.Subscribe<KeyEvent>([this](const KeyEvent& e) {
        if (!InRange(e.key, Key::Last)) // GLFW_KEY_UNKNOWN = -1
            return;
        if (e.action == InputAction::Press) {
            m_Keys.set(static_cast<std::size_t>(e.key));
            m_KeysPressed.set(static_cast<std::size_t>(e.key));
        } else if (e.action == InputAction::Release) {
            m_Keys.reset(static_cast<std::size_t>(e.key));
            m_KeysReleased.set(static_cast<std::size_t>(e.key));
        }
    });
    m_ButtonSub = events.Subscribe<MouseButtonEvent>([this](const MouseButtonEvent& e) {
        if (!InRange(e.button, MouseButton::Last))
            return;
        if (e.action == InputAction::Press) {
            m_Buttons.set(static_cast<std::size_t>(e.button));
            m_ButtonsPressed.set(static_cast<std::size_t>(e.button));
        } else if (e.action == InputAction::Release) {
            m_Buttons.reset(static_cast<std::size_t>(e.button));
            m_ButtonsReleased.set(static_cast<std::size_t>(e.button));
        }
    });
    m_MoveSub = events.Subscribe<MouseMoveEvent>([this](const MouseMoveEvent& e) {
        const glm::vec2 pos{static_cast<float>(e.x), static_cast<float>(e.y)};
        if (!m_FirstMove)
            m_MouseDelta += pos - m_MousePos; // accumulate: several events per frame
        m_FirstMove = false;
        m_MousePos  = pos;
    });
    m_ScrollSub = events.Subscribe<MouseScrollEvent>([this](const MouseScrollEvent& e) {
        m_Scroll += static_cast<float>(e.dy);
    });
    m_FocusSub = events.Subscribe<WindowFocusEvent>([this](const WindowFocusEvent& e) {
        if (!e.focused) { // release events are lost while unfocused -> avoid stuck keys
            m_Keys.reset();
            m_Buttons.reset();
        }
    });
}

void Input::NewFrame()
{
    m_KeysPressed.reset();
    m_KeysReleased.reset();
    m_ButtonsPressed.reset();
    m_ButtonsReleased.reset();
    m_MouseDelta = glm::vec2{0.0f};
    m_Scroll     = 0.0f;
}

bool Input::IsKeyDown(int key) const { return InRange(key, Key::Last) && m_Keys.test(static_cast<std::size_t>(key)); }
bool Input::WasKeyPressed(int key) const
{
    return InRange(key, Key::Last) && m_KeysPressed.test(static_cast<std::size_t>(key));
}
bool Input::WasKeyReleased(int key) const
{
    return InRange(key, Key::Last) && m_KeysReleased.test(static_cast<std::size_t>(key));
}
bool Input::IsMouseDown(int b) const
{
    return InRange(b, MouseButton::Last) && m_Buttons.test(static_cast<std::size_t>(b));
}
bool Input::WasMousePressed(int b) const
{
    return InRange(b, MouseButton::Last) && m_ButtonsPressed.test(static_cast<std::size_t>(b));
}
bool Input::WasMouseReleased(int b) const
{
    return InRange(b, MouseButton::Last) && m_ButtonsReleased.test(static_cast<std::size_t>(b));
}

} // namespace Engine
