#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/UI/UiLayout.h"

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

class Input;
class TextOverlay;
class AssetManager;

// One frame of UI input. The pointer is in framebuffer pixels of the UI's viewport (outside:
// negative); navigation flags are edges (this frame only).
struct UiInput {
    glm::vec2 pointer{-1.0f};
    bool      pointerPressed  = false;
    bool      pointerDown     = false;
    bool      pointerReleased = false;
    bool      next     = false; // focus: Tab, Down
    bool      previous = false; // focus: Shift+Tab, Up
    bool      left     = false; // focused slider: step down, else focus previous
    bool      right    = false; // focused slider: step up, else focus next
    bool      submit   = false; // Enter, Space, gamepad South
};

// Window input (mouse in window coordinates, keyboard, gamepad) for a UI covering the window.
[[nodiscard]] UiInput UiInputFromWindow(const Input& input, glm::vec2 windowSize, glm::vec2 framebufferSize);

// Main-thread UI interaction and draw-list builder (player; editor viewport while playing).
// Update with window and framebuffer dimensions (they differ on high-DPI displays) or with an
// explicit UiInput, then Draw into the game's overlay.
class UiSystem {
public:
    ~UiSystem();
    UiSystem() = default;
    UiSystem(const UiSystem&) = delete;
    UiSystem& operator=(const UiSystem&) = delete;

    void PrepareLayout(const Scene& scene, glm::vec2 framebufferSize);
    void SyncAssets(const Scene& scene, AssetManager& assets);
    void Update(Scene& scene, const Input& input, glm::vec2 windowSize, glm::vec2 framebufferSize);
    void Update(Scene& scene, const UiInput& input, glm::vec2 framebufferSize);
    void Draw(const Scene& scene, TextOverlay& overlay) const;
    [[nodiscard]] Entity Focused() const { return m_Focused; }
    [[nodiscard]] Entity Hovered() const { return m_Hovered; }
    // The pointer is over an interactive widget or drags one: clicks belong to the UI.
    [[nodiscard]] bool CapturesPointer() const { return m_Hovered != NullEntity || m_Active != NullEntity; }
    [[nodiscard]] std::span<const UiEvent> Events() const { return m_Events; }

private:
    Entity m_Focused = NullEntity;
    Entity m_Hovered = NullEntity;
    Entity m_Active = NullEntity;
    std::vector<UiLayoutItem> m_Layout;
    std::vector<UiEvent> m_Events;
    AssetManager* m_Assets = nullptr;
    std::unordered_map<std::string, TextureHandle> m_ImageHandles;
};

} // namespace Engine
