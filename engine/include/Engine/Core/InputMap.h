#pragma once
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

// Named input actions and axes of a project (.ungineproj "input"): blueprints use them (Input
// Action events, Is Action Down, Get Axis) so key bindings live in one place. Keys by name (see
// KeyNames(): "A".."Z", "0".."9", "Space", "Left", ...) plus "MouseLeft", "MouseRight", "MouseMiddle".
struct InputActionBinding {
    std::string              name;
    std::vector<std::string> keys;
    bool operator==(const InputActionBinding&) const = default;
};

struct InputAxisKey {
    std::string key;
    float       scale = 1.0f; // contribution while held (axis value = clamped sum)
    bool operator==(const InputAxisKey&) const = default;
};

struct InputAxisBinding {
    std::string               name;
    std::vector<InputAxisKey> keys;
    bool operator==(const InputAxisBinding&) const = default;
};

struct InputMap {
    std::vector<InputActionBinding> actions;
    std::vector<InputAxisBinding>   axes;

    [[nodiscard]] const InputActionBinding* FindAction(std::string_view name) const
    {
        const auto it = std::ranges::find(actions, name, &InputActionBinding::name);
        return it != actions.end() ? &*it : nullptr;
    }
    [[nodiscard]] const InputAxisBinding* FindAxis(std::string_view name) const
    {
        const auto it = std::ranges::find(axes, name, &InputAxisBinding::name);
        return it != axes.end() ? &*it : nullptr;
    }
    bool operator==(const InputMap&) const = default;
};

} // namespace Engine
