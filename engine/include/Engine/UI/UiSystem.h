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

// Main-thread UI interaction and draw-list builder for the player. Update with window and
// framebuffer dimensions (they differ on high-DPI displays), then Draw into the game's overlay.
class UiSystem {
public:
    ~UiSystem();
    UiSystem() = default;
    UiSystem(const UiSystem&) = delete;
    UiSystem& operator=(const UiSystem&) = delete;

    void PrepareLayout(const Scene& scene, glm::vec2 framebufferSize);
    void SyncAssets(const Scene& scene, AssetManager& assets);
    void Update(Scene& scene, const Input& input, glm::vec2 windowSize, glm::vec2 framebufferSize);
    void Draw(const Scene& scene, TextOverlay& overlay) const;
    [[nodiscard]] Entity Focused() const { return m_Focused; }
    [[nodiscard]] Entity Hovered() const { return m_Hovered; }
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
