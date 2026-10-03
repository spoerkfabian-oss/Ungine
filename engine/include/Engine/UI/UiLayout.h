#pragma once
#include "Engine/ECS/Entity.h"
#include "Engine/Scene/Components.h"

#include <glm/glm.hpp>

#include <vector>

namespace Engine {

class Scene;

struct UiRect {
    glm::vec2 position{0.0f}; // top-left in framebuffer pixels
    glm::vec2 size{0.0f};
    [[nodiscard]] bool Contains(glm::vec2 point) const
    {
        return point.x >= position.x && point.y >= position.y && point.x < position.x + size.x &&
               point.y < position.y + size.y;
    }
};

struct UiLayoutItem {
    Entity entity = NullEntity;
    UiWidgetType type = UiWidgetType::Panel;
    UiRect rect;
    glm::vec2 scale{1.0f};
    std::uint64_t order = 0;
};

enum class UiEventType : std::uint8_t { Clicked, ValueChanged, CheckedChanged };
struct UiEvent {
    Entity entity = NullEntity;
    UiEventType type = UiEventType::Clicked;
    float value = 0.0f;
    bool checked = false;
};

// Resolves a canvas and its widget descendants in stable hierarchy order. Invalid dimensions
// return an empty layout; hidden canvases and disabled widgets are omitted.
[[nodiscard]] std::vector<UiLayoutItem> BuildUiLayout(const Scene& scene, Entity canvas,
                                                       glm::vec2 viewportSize);

} // namespace Engine
