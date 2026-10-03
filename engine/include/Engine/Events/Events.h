#pragma once
#include <array>
#include <cstdint>

namespace Engine {

// Values match GLFW so the platform layer can forward them without translation.
enum class InputAction : int { Release = 0, Press = 1, Repeat = 2 };

namespace Key {
inline constexpr int Space = 32, Minus = 45, Equal = 61;
inline constexpr int A = 65, B = 66, C = 67, D = 68, E = 69, O = 79, P = 80, Q = 81, S = 83, T = 84;
inline constexpr int V = 86, W = 87, X = 88;
inline constexpr int F = 70, G = 71, H = 72, J = 74, K = 75, L = 76, R = 82;
inline constexpr int Delete = 261, Right = 262, Left = 263, Down = 264, Up = 265;
inline constexpr int Escape = 256, Enter = 257, Tab = 258, F1 = 290, F5 = 294;
inline constexpr int LeftShift = 340, LeftControl = 341;
inline constexpr int Last = 348;
} // namespace Key

namespace MouseButton {
inline constexpr int Left = 0, Right = 1, Middle = 2;
inline constexpr int Last = 7;
} // namespace MouseButton

namespace GamepadButton {
inline constexpr int South = 0, East = 1, West = 2, North = 3;
inline constexpr int LeftBumper = 4, RightBumper = 5, Back = 6, Start = 7;
inline constexpr int DpadUp = 11, DpadRight = 12, DpadDown = 13, DpadLeft = 14;
inline constexpr int Last = 14;
} // namespace GamepadButton

namespace GamepadAxis {
inline constexpr int LeftX = 0, LeftY = 1, RightX = 2, RightY = 3, LeftTrigger = 4, RightTrigger = 5;
inline constexpr int Last = 5;
} // namespace GamepadAxis

// --- Window ---
struct WindowCloseEvent {};
struct WindowFocusEvent       { bool focused; };
struct FramebufferResizeEvent { std::uint32_t width, height; };

// --- Input ---
struct KeyEvent         { int key; int scancode; InputAction action; int mods; };
struct CharEvent        { std::uint32_t codepoint; };
struct MouseButtonEvent { int button; InputAction action; int mods; };
struct MouseMoveEvent   { double x, y; };
struct MouseScrollEvent { double dx, dy; };
struct GamepadStateEvent {
    bool connected = false;
    std::array<std::uint8_t, GamepadButton::Last + 1> buttons{};
    std::array<float, GamepadAxis::Last + 1> axes{};
};

// Custom engine/game events: any plain struct works, no registration needed.

} // namespace Engine
