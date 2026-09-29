#pragma once
#include "Engine/Renderer/Renderer.h"

#include <imgui.h>

#include <string>
#include <vector>

namespace Engine {

class Window;

// Dear ImGui context + GLFW/Vulkan backends (dynamic rendering, volk). Main thread only.
class ImGuiLayer {
public:
    // iniFile: layout settings (empty: not saved).
    ImGuiLayer(Window& window, Renderer& renderer, std::string iniFile = "editor.ini");
    ~ImGuiLayer(); // waits for the device: the backend frees its resources immediately

    ImGuiLayer(const ImGuiLayer&)            = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    void NewFrame();

    // ImGui::Render() and draw into `target` (cleared first; COLOR_ATTACHMENT_OPTIMAL in and out).
    void Render(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent);

    // Sampled image for ImGui::Image (must be in `layout` whenever the UI is drawn).
    [[nodiscard]] ImTextureID AddTexture(VkImageView view, VkImageLayout layout);
    // Freed once the frames in flight that may draw it are done (or at shutdown).
    void RemoveTexture(ImTextureID texture);

private:
    struct PendingRemoval {
        ImTextureID   texture;
        std::uint32_t framesLeft;
    };

    Renderer&                   m_Renderer;
    std::string                 m_IniFile; // ImGui keeps the pointer
    std::vector<PendingRemoval> m_PendingRemovals; // kept here: must not outlive the backend
    bool                        m_FrameOpen = false;
};

} // namespace Engine
