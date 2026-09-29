#pragma once
#include "Engine/Events/EventBus.h"
#include "Engine/Events/Events.h"

#include <glm/glm.hpp>

#include <bitset>

namespace Engine {

// Polling-style input state built from events. Call NewFrame() before polling OS events.
class Input {
public:
    explicit Input(EventBus& events);

    void NewFrame();              // clears per-frame deltas/edges
    void SuppressNextMouseDelta() { m_FirstMove = true; } // after cursor capture/warp

    [[nodiscard]] bool IsKeyDown(int key) const;
    [[nodiscard]] bool WasKeyPressed(int key) const;      // pressed this frame
    [[nodiscard]] bool WasKeyReleased(int key) const;     // released this frame
    [[nodiscard]] bool IsMouseDown(int button) const;
    [[nodiscard]] bool WasMousePressed(int button) const;

    [[nodiscard]] glm::vec2 MousePosition() const { return m_MousePos; }
    [[nodiscard]] glm::vec2 MouseDelta()    const { return m_MouseDelta; }
    [[nodiscard]] float     ScrollDelta()   const { return m_Scroll; }

private:
    std::bitset<Key::Last + 1>         m_Keys, m_KeysPressed, m_KeysReleased;
    std::bitset<MouseButton::Last + 1> m_Buttons, m_ButtonsPressed;
    glm::vec2 m_MousePos{0.0f}, m_MouseDelta{0.0f};
    float     m_Scroll    = 0.0f;
    bool      m_FirstMove = true;

    Subscription m_KeySub, m_ButtonSub, m_MoveSub, m_ScrollSub, m_FocusSub;
};

} // namespace Engine
