#include "Engine/UI/UiSystem.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Input.h"
#include "Engine/Core/Platform.h"
#include "Engine/Renderer/TextOverlay.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace Engine {

namespace {

struct CanvasOrder {
    Entity entity;
    std::int32_t order;
};

bool Interactable(const UiWidget& widget)
{
    return widget.enabled && widget.interactable &&
           (widget.type == UiWidgetType::Button || widget.type == UiWidgetType::Checkbox ||
            widget.type == UiWidgetType::Slider);
}

float NormalizedValue(const UiWidget& widget)
{
    if (!std::isfinite(widget.value) || !std::isfinite(widget.minimum) || !std::isfinite(widget.maximum) ||
        widget.maximum <= widget.minimum)
        return 0.0f;
    return std::clamp((widget.value - widget.minimum) / (widget.maximum - widget.minimum), 0.0f, 1.0f);
}

void PushLabel(TextOverlay& overlay, const UiWidget& widget, const UiLayoutItem& item, glm::vec4 color)
{
    if (widget.text.empty())
        return;
    const float scale = std::max(0.25f, widget.fontSize * std::min(item.scale.x, item.scale.y) / 7.0f);
    const glm::vec2 measured = TextOverlay::Measure(widget.text, scale);
    const float inset = (widget.type == UiWidgetType::Checkbox ? 32.0f : 8.0f) * item.scale.x;
    const float x = item.rect.position.x + inset;
    const float y = item.rect.position.y + std::max(0.0f, (item.rect.size.y - measured.y) * 0.5f);
    overlay.Add(widget.text, {x, y}, color, scale);
}

} // namespace

UiSystem::~UiSystem()
{
    if (m_Assets)
        for (const auto& [path, handle] : m_ImageHandles)
            if (m_Assets->State(handle) != AssetState::Invalid)
                m_Assets->Release(handle);
}

void UiSystem::PrepareLayout(const Scene& scene, glm::vec2 framebufferSize)
{
    m_Layout.clear();
    const Registry& registry = scene.GetRegistry();
    std::vector<CanvasOrder> canvases;
    registry.ViewOf<UiCanvas>().Each([&](Entity entity, const UiCanvas& canvas) {
        if (canvas.visible)
            canvases.push_back({entity, canvas.sortOrder});
    });
    std::ranges::sort(canvases, [](const CanvasOrder& a, const CanvasOrder& b) {
        return a.order != b.order ? a.order < b.order : static_cast<std::uint64_t>(a.entity) < static_cast<std::uint64_t>(b.entity);
    });
    for (const CanvasOrder& canvas : canvases) {
        auto items = BuildUiLayout(scene, canvas.entity, framebufferSize);
        m_Layout.insert(m_Layout.end(), items.begin(), items.end());
    }
}

void UiSystem::SyncAssets(const Scene& scene, AssetManager& assets)
{
    if (m_Assets != &assets) {
        if (m_Assets)
            for (const auto& [path, handle] : m_ImageHandles)
                if (m_Assets->State(handle) != AssetState::Invalid)
                    m_Assets->Release(handle);
        m_ImageHandles.clear();
        m_Assets = &assets;
    }

    std::unordered_set<std::string> used;
    const Registry& registry = scene.GetRegistry();
    for (const UiLayoutItem& item : m_Layout) {
        if (!registry.Valid(item.entity) || item.type != UiWidgetType::Image)
            continue;
        const std::string& path = registry.Get<UiWidget>(item.entity).image;
        if (path.empty())
            continue;
        used.insert(path);
        if (!m_ImageHandles.contains(path))
            m_ImageHandles.emplace(path, assets.LoadTexture(PathFromUtf8(path), TextureKind::Color));
    }
    for (auto it = m_ImageHandles.begin(); it != m_ImageHandles.end();) {
        if (used.contains(it->first)) {
            ++it;
            continue;
        }
        if (assets.State(it->second) != AssetState::Invalid)
            assets.Release(it->second);
        it = m_ImageHandles.erase(it);
    }
}

UiInput UiInputFromWindow(const Input& input, glm::vec2 windowSize, glm::vec2 framebufferSize)
{
    UiInput ui;
    if (windowSize.x > 0.0f && windowSize.y > 0.0f)
        ui.pointer = input.MousePosition() * (framebufferSize / windowSize);
    ui.pointerPressed  = input.WasMousePressed(MouseButton::Left);
    ui.pointerDown     = input.IsMouseDown(MouseButton::Left);
    ui.pointerReleased = input.WasMouseReleased(MouseButton::Left);
    const bool tab     = input.WasKeyPressed(Key::Tab);
    const bool shift   = input.IsKeyDown(Key::LeftShift);
    ui.next     = (tab && !shift) || input.WasKeyPressed(Key::Down) || input.WasGamepadButtonPressed(GamepadButton::DpadDown);
    ui.previous = (tab && shift) || input.WasKeyPressed(Key::Up) || input.WasGamepadButtonPressed(GamepadButton::DpadUp);
    ui.left     = input.WasKeyPressed(Key::Left) || input.WasGamepadButtonPressed(GamepadButton::DpadLeft);
    ui.right    = input.WasKeyPressed(Key::Right) || input.WasGamepadButtonPressed(GamepadButton::DpadRight);
    ui.submit   = input.WasKeyPressed(Key::Enter) || input.WasKeyPressed(Key::Space) ||
                input.WasGamepadButtonPressed(GamepadButton::South);
    return ui;
}

void UiSystem::Update(Scene& scene, const Input& input, glm::vec2 windowSize, glm::vec2 framebufferSize)
{
    Update(scene, UiInputFromWindow(input, windowSize, framebufferSize), framebufferSize);
}

void UiSystem::Update(Scene& scene, const UiInput& input, glm::vec2 framebufferSize)
{
    m_Events.clear();
    PrepareLayout(scene, framebufferSize);
    const Registry& registry = scene.GetRegistry();

    const auto findItem = [&](Entity entity) {
        return std::ranges::find(m_Layout, entity, &UiLayoutItem::entity);
    };
    if (m_Focused != NullEntity) {
        const auto it = findItem(m_Focused);
        if (it == m_Layout.end() || !registry.Valid(m_Focused) || !Interactable(registry.Get<UiWidget>(m_Focused)))
            m_Focused = NullEntity;
    }
    if (m_Active != NullEntity) {
        const auto it = findItem(m_Active);
        if (it == m_Layout.end() || !registry.Valid(m_Active) || !Interactable(registry.Get<UiWidget>(m_Active)))
            m_Active = NullEntity;
    }

    const glm::vec2 pointer = input.pointer;
    m_Hovered = NullEntity;
    for (auto it = m_Layout.rbegin(); it != m_Layout.rend(); ++it) {
        if (!registry.Valid(it->entity))
            continue;
        const UiWidget& widget = registry.Get<UiWidget>(it->entity);
        if (Interactable(widget) && it->rect.Contains(pointer)) {
            m_Hovered = it->entity;
            break;
        }
    }

    std::vector<Entity> targets;
    for (const UiLayoutItem& item : m_Layout)
        if (registry.Valid(item.entity) && Interactable(registry.Get<UiWidget>(item.entity)))
            targets.push_back(item.entity);
    const auto moveFocus = [&](int direction) {
        if (targets.empty()) {
            m_Focused = NullEntity;
            return;
        }
        const auto current = std::ranges::find(targets, m_Focused);
        const std::ptrdiff_t index = current == targets.end() ?
                                         (direction >= 0 ? -1 : 0) : current - targets.begin();
        const auto count = static_cast<std::ptrdiff_t>(targets.size());
        m_Focused = targets[static_cast<std::size_t>((index + direction + count) % count)];
    };
    const auto focusedIsSlider = [&] {
        return m_Focused != NullEntity && registry.Valid(m_Focused) &&
               registry.Get<UiWidget>(m_Focused).type == UiWidgetType::Slider;
    };
    if (input.next || (input.right && !focusedIsSlider()))
        moveFocus(1);
    else if (input.previous || (input.left && !focusedIsSlider()))
        moveFocus(-1);

    const auto activate = [&](Entity entity) {
        if (entity == NullEntity || !registry.Valid(entity))
            return;
        UiWidget& widget = scene.GetRegistry().Get<UiWidget>(entity);
        if (widget.type == UiWidgetType::Button)
            m_Events.push_back({entity, UiEventType::Clicked});
        else if (widget.type == UiWidgetType::Checkbox) {
            widget.checked = !widget.checked;
            m_Events.push_back({entity, UiEventType::CheckedChanged, widget.checked ? 1.0f : 0.0f, widget.checked});
        }
    };

    const auto setSliderFromPointer = [&](Entity entity) {
        if (entity == NullEntity || !registry.Valid(entity))
            return;
        UiWidget& widget = scene.GetRegistry().Get<UiWidget>(entity);
        if (widget.type != UiWidgetType::Slider || widget.maximum <= widget.minimum)
            return;
        const auto item = findItem(entity);
        if (item == m_Layout.end() || item->rect.size.x <= 0.0f)
            return;
        const float t = std::clamp((pointer.x - item->rect.position.x) / item->rect.size.x, 0.0f, 1.0f);
        const float value = widget.minimum + t * (widget.maximum - widget.minimum);
        if (std::isfinite(value) && (!std::isfinite(widget.value) || std::abs(value - widget.value) > 1.0e-5f)) {
            widget.value = value;
            m_Events.push_back({entity, UiEventType::ValueChanged, value, false});
        }
    };

    if (input.pointerPressed) {
        m_Focused = m_Hovered;
        m_Active = m_Hovered;
        setSliderFromPointer(m_Active);
    }
    if (m_Active != NullEntity && input.pointerDown)
        setSliderFromPointer(m_Active);
    if (input.pointerReleased) {
        if (m_Active != NullEntity && m_Active == m_Hovered)
            activate(m_Active);
        m_Active = NullEntity;
    }

    if (m_Focused != NullEntity && input.submit)
        activate(m_Focused);
    if (focusedIsSlider()) {
        UiWidget& widget = scene.GetRegistry().Get<UiWidget>(m_Focused);
        if (std::isfinite(widget.minimum) && std::isfinite(widget.maximum) && widget.maximum > widget.minimum) {
            const float step = std::max((widget.maximum - widget.minimum) * 0.01f, 0.001f);
            float value = std::isfinite(widget.value) ? std::clamp(widget.value, widget.minimum, widget.maximum)
                                                     : widget.minimum;
            if (input.left)
                value = std::max(value - step, widget.minimum);
            else if (input.right)
                value = std::min(value + step, widget.maximum);
            if (value != widget.value) {
                widget.value = value;
                m_Events.push_back({m_Focused, UiEventType::ValueChanged, widget.value, false});
            }
        }
    }
}

void UiSystem::Draw(const Scene& scene, TextOverlay& overlay) const
{
    const Registry& registry = scene.GetRegistry();
    for (const UiLayoutItem& item : m_Layout) {
        if (!registry.Valid(item.entity))
            continue;
        const UiWidget& widget = registry.Get<UiWidget>(item.entity);
        const bool focused = item.entity == m_Focused;
        const bool hovered = item.entity == m_Hovered;
        glm::vec4 background = widget.background;
        if (!widget.enabled)
            background *= glm::vec4{0.55f, 0.55f, 0.55f, 0.75f};
        else if (hovered)
            background = glm::min(background + glm::vec4{0.12f, 0.12f, 0.14f, 0.0f}, glm::vec4{1.0f});

        const UiRect& rect = item.rect;
        switch (widget.type) {
        case UiWidgetType::Text:
            break;
        case UiWidgetType::Panel:
        case UiWidgetType::Button:
            overlay.AddRect(rect.position, rect.size, background);
            break;
        case UiWidgetType::Image:
            overlay.AddRect(rect.position, rect.size, background);
            if (m_Assets)
                if (const auto texture = m_ImageHandles.find(widget.image); texture != m_ImageHandles.end())
                    overlay.AddImage(m_Assets->TableEntry(texture->second), rect.position, rect.size, widget.color);
            break;
        case UiWidgetType::Checkbox: {
            const float side = std::min(rect.size.y, 24.0f * std::min(item.scale.x, item.scale.y));
            overlay.AddRect(rect.position, {side, side}, background);
            if (widget.checked)
                overlay.AddRect(rect.position + glm::vec2{side * 0.22f}, glm::vec2{side * 0.56f}, widget.color);
            break;
        }
        case UiWidgetType::Slider: {
            const float height = std::max(4.0f, 6.0f * std::min(item.scale.x, item.scale.y));
            const float y = rect.position.y + (rect.size.y - height) * 0.5f;
            overlay.AddRect({rect.position.x, y}, {rect.size.x, height}, background);
            overlay.AddRect({rect.position.x, y}, {rect.size.x * NormalizedValue(widget), height}, widget.color);
            const float knob = std::max(10.0f, 18.0f * std::min(item.scale.x, item.scale.y));
            overlay.AddRect({rect.position.x + rect.size.x * NormalizedValue(widget) - knob * 0.5f,
                             rect.position.y + (rect.size.y - knob) * 0.5f},
                            {knob, knob}, focused ? glm::vec4{1.0f, 0.85f, 0.35f, 1.0f} : widget.color);
            break;
        }
        case UiWidgetType::ProgressBar:
            overlay.AddRect(rect.position, rect.size, background);
            overlay.AddRect(rect.position, {rect.size.x * NormalizedValue(widget), rect.size.y}, widget.color);
            break;
        }
        if (focused && widget.type != UiWidgetType::Text) {
            const float border = std::max(1.0f, 2.0f * std::min(item.scale.x, item.scale.y));
            const glm::vec4 focus{1.0f, 0.78f, 0.2f, 1.0f};
            overlay.AddRect(rect.position, {rect.size.x, border}, focus);
            overlay.AddRect({rect.position.x, rect.position.y + rect.size.y - border}, {rect.size.x, border}, focus);
            overlay.AddRect(rect.position, {border, rect.size.y}, focus);
            overlay.AddRect({rect.position.x + rect.size.x - border, rect.position.y}, {border, rect.size.y}, focus);
        }
        PushLabel(overlay, widget, item, widget.enabled ? widget.color : widget.color * 0.55f);
    }
}

} // namespace Engine
