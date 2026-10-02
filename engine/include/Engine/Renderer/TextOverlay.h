#pragma once
#include "Engine/Renderer/Vulkan/Pipeline.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

class Renderer;
struct FrameContext;

// Debug / script text drawn over a rendered image (stb_easy_font: a small built-in bitmap-like
// font, no assets). Queue text with Add during the frame, then Render after the scene. Main thread.
class TextOverlay {
public:
    explicit TextOverlay(Renderer& renderer);
    ~TextOverlay(); // pipelines go through the renderer's deferred release (destroy before the renderer)

    TextOverlay(const TextOverlay&)            = delete;
    TextOverlay& operator=(const TextOverlay&) = delete;

    // position: top-left corner in pixels (y down); color in sRGB; scale 1 = 7 px glyphs.
    // Each line gets a dark shadow for readability.
    void Add(std::string_view text, glm::vec2 position, glm::vec4 color = glm::vec4(1.0f), float scale = 2.0f);
    // Size in pixels (lines separated by '\n').
    [[nodiscard]] static glm::vec2 Measure(std::string_view text, float scale = 2.0f);
    [[nodiscard]] bool             Empty() const { return m_Items.empty(); }

    // Draws the queued text into `image` (COLOR_ATTACHMENT_OPTIMAL before and after) and clears
    // the queue. Without a target, the frame's swapchain image.
    void Render(const FrameContext& frame);
    void Render(const FrameContext& frame, VkImageView view, VkFormat format, VkExtent2D extent);

private:
    struct Item {
        std::string text;
        glm::vec2   position;
        glm::vec4   color;
        float       scale;
    };
    const Pipeline& PipelineFor(VkFormat format);

    Renderer&                                  m_Renderer;
    std::vector<Item>                          m_Items;
    std::vector<std::pair<VkFormat, Pipeline>> m_Pipelines;
    std::uint64_t                              m_ShaderGeneration = 0;
};

} // namespace Engine
