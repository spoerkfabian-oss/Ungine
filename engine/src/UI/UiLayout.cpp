#include "Engine/UI/UiLayout.h"

#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace Engine {

namespace {

glm::vec2 Finite(glm::vec2 value, glm::vec2 fallback)
{
    for (int axis = 0; axis < 2; ++axis)
        if (!std::isfinite(value[axis]))
            value[axis] = fallback[axis];
    return value;
}

} // namespace

std::vector<UiLayoutItem> BuildUiLayout(const Scene& scene, Entity canvasEntity, glm::vec2 viewportSize)
{
    std::vector<UiLayoutItem> result;
    const Registry& registry = scene.GetRegistry();
    if (!registry.Valid(canvasEntity) || !registry.Has<UiCanvas>(canvasEntity) ||
        !std::isfinite(viewportSize.x) || !std::isfinite(viewportSize.y) || viewportSize.x <= 0.0f ||
        viewportSize.y <= 0.0f)
        return result;

    const UiCanvas& canvas = registry.Get<UiCanvas>(canvasEntity);
    if (!canvas.visible || !std::isfinite(canvas.designSize.x) || !std::isfinite(canvas.designSize.y) ||
        canvas.designSize.x <= 0.0f || canvas.designSize.y <= 0.0f)
        return result;

    UiRect root{glm::vec2(0.0f), viewportSize};
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    if (canvas.scaleWithViewport) {
        const float scale = std::min(viewportSize.x / canvas.designSize.x, viewportSize.y / canvas.designSize.y);
        root.size = canvas.designSize * scale;
        root.position = (viewportSize - root.size) * 0.5f;
        scaleX = scaleY = scale;
    }

    std::uint64_t order = 0;
    std::function<void(Entity, const UiRect&, float, float)> visit;
    visit = [&](Entity parent, const UiRect& parentRect, float parentScaleX, float parentScaleY) {
        for (const Entity child : registry.Get<Hierarchy>(parent).children) {
            if (!registry.Valid(child))
                continue;
            const UiWidget* widget = registry.TryGet<UiWidget>(child);
            UiRect rect = parentRect;
            float childScaleX = parentScaleX;
            float childScaleY = parentScaleY;
            if (widget) {
                if (!widget->visible)
                    continue;
                const glm::vec2 anchorMin = glm::clamp(Finite(widget->anchorMin, glm::vec2{0.0f}),
                                                       glm::vec2{0.0f}, glm::vec2{1.0f});
                const glm::vec2 anchorMax = glm::clamp(Finite(widget->anchorMax, glm::vec2{0.0f}),
                                                       glm::vec2{0.0f}, glm::vec2{1.0f});
                const glm::vec2 offsetMin = Finite(widget->offsetMin, glm::vec2{0.0f});
                const glm::vec2 offsetMax = Finite(widget->offsetMax, glm::vec2{160.0f, 48.0f});
                const glm::vec2 pivot = glm::clamp(Finite(widget->pivot, glm::vec2{0.0f}),
                                                   glm::vec2{0.0f}, glm::vec2{1.0f});
                glm::vec2 minimum = parentRect.position + anchorMin * parentRect.size +
                                    offsetMin * glm::vec2{parentScaleX, parentScaleY};
                glm::vec2 maximum = parentRect.position + anchorMax * parentRect.size +
                                    offsetMax * glm::vec2{parentScaleX, parentScaleY};
                for (int axis = 0; axis < 2; ++axis) {
                    if (std::abs(anchorMax[axis] - anchorMin[axis]) < 1.0e-6f) {
                        const float extent = maximum[axis] - minimum[axis];
                        const float shift = pivot[axis] * extent;
                        minimum[axis] -= shift;
                        maximum[axis] -= shift;
                    }
                }
                rect.position = minimum;
                rect.size = glm::max(maximum - minimum, glm::vec2{0.0f});
                result.push_back({.entity = child, .type = widget->type, .rect = rect,
                                  .scale = {parentScaleX, parentScaleY}, .order = order++});
            }
            visit(child, rect, childScaleX, childScaleY);
        }
    };
    visit(canvasEntity, root, scaleX, scaleY);
    return result;
}

} // namespace Engine
