#include "Engine/Renderer/TextOverlay.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/Bindless.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Renderer/Vulkan/VulkanContext.h"

#include <stb_easy_font.h>

#include <algorithm>
#include <cstring>

namespace Engine {

namespace {

struct TextVertex { // stb_easy_font's layout: position + RGBA8
    float         x, y, z;
    unsigned char color[4];
};
static_assert(sizeof(TextVertex) == 16);

struct TextPush { // mirrors text.vert
    VkDeviceAddress vertices;
    glm::vec2       screenSize;
    std::uint32_t   srgbTarget;
    std::uint32_t   pad;
};

constexpr std::size_t kMaxQuads = 65536; // per frame

bool IsSrgb(VkFormat format)
{
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

unsigned char ToByte(float v) { return static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

} // namespace

TextOverlay::TextOverlay(Renderer& renderer) : m_Renderer(renderer), m_ShaderGeneration(renderer.ShaderGeneration()) {}
TextOverlay::~TextOverlay()
{
    for (auto& [format, pipeline] : m_Pipelines) // frames in flight may still draw with them
        m_Renderer.DeferRelease(std::move(pipeline));
}

void TextOverlay::Add(std::string_view text, glm::vec2 position, glm::vec4 color, float scale)
{
    if (!text.empty())
        m_Items.push_back({std::string(text), position, color, std::max(scale, 0.25f)});
}

glm::vec2 TextOverlay::Measure(std::string_view text, float scale)
{
    std::string copy(text);
    return glm::vec2(static_cast<float>(stb_easy_font_width(copy.data())),
                     static_cast<float>(stb_easy_font_height(copy.data()))) *
           scale;
}

const Pipeline& TextOverlay::PipelineFor(VkFormat format)
{
    if (m_ShaderGeneration != m_Renderer.ShaderGeneration()) { // shaders hot-reloaded
        for (auto& [f, pipeline] : m_Pipelines)
            m_Renderer.DeferRelease(std::move(pipeline));
        m_Pipelines.clear();
        m_ShaderGeneration = m_Renderer.ShaderGeneration();
    }
    for (const auto& [f, pipeline] : m_Pipelines)
        if (f == format)
            return pipeline;
    m_Pipelines.emplace_back(format, GraphicsPipelineBuilder{}
                                         .SetShaders(ShaderPath("text.vert.spv"), ShaderPath("text.frag.spv"))
                                         .SetBlend(BlendMode::Alpha)
                                         .AddColorAttachment(format)
                                         .SetDebugName("Text overlay")
                                         .Build(m_Renderer.GetContext().Device(), m_Renderer.GetBindless().PipelineLayout()));
    return m_Pipelines.back().second;
}

void TextOverlay::Render(const FrameContext& frame)
{
    Render(frame, frame.view, frame.format, frame.extent);
}

void TextOverlay::Render(const FrameContext& frame, VkImageView view, VkFormat format, VkExtent2D extent)
{
    if (m_Items.empty() || extent.width == 0 || extent.height == 0) {
        m_Items.clear();
        return;
    }
    // Quads per item: stb_easy_font writes ~270 bytes per character at most; shadow + text.
    std::vector<TextVertex> vertices;
    std::vector<TextVertex> scratch;
    for (const Item& item : m_Items) {
        std::string text = item.text;
        scratch.resize(text.size() * 70 + 64); // stb_easy_font: up to ~4 quads (16 vertices) per char
        const auto emit = [&](glm::vec2 offset, glm::vec4 color) {
            unsigned char rgba[4] = {ToByte(color.r), ToByte(color.g), ToByte(color.b), ToByte(color.a)};
            const int     quads   = stb_easy_font_print(0.0f, 0.0f, text.data(), rgba, scratch.data(),
                                                        static_cast<int>(scratch.size() * sizeof(TextVertex)));
            for (int i = 0; i < quads * 4; ++i) {
                TextVertex v = scratch[static_cast<std::size_t>(i)];
                v.x          = item.position.x + offset.x + v.x * item.scale;
                v.y          = item.position.y + offset.y + v.y * item.scale;
                vertices.push_back(v);
            }
        };
        emit(glm::vec2(std::max(1.0f, item.scale * 0.5f)), glm::vec4(0.0f, 0.0f, 0.0f, 0.75f * item.color.a));
        emit(glm::vec2(0.0f), item.color);
        if (vertices.size() / 4 >= kMaxQuads)
            break;
    }
    m_Items.clear();
    const std::size_t quads = std::min(vertices.size() / 4, kMaxQuads);
    if (quads == 0)
        return;

    const TransientAllocation buffer = m_Renderer.AllocateTransient(quads * 4 * sizeof(TextVertex), 16);
    std::memcpy(buffer.cpu, vertices.data(), quads * 4 * sizeof(TextVertex));

    const VkCommandBuffer cmd = frame.cmd;
    VkRenderingAttachmentInfo color{};
    color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView   = view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{};
    info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea           = {{0, 0}, extent};
    info.layerCount           = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments    = &color;
    vkCmdBeginRendering(cmd, &info);
    m_Renderer.GetBindless().Bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    SetViewportScissor(cmd, extent);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, PipelineFor(format).Handle());
    const TextPush push{.vertices   = buffer.gpu,
                        .screenSize = glm::vec2(static_cast<float>(extent.width), static_cast<float>(extent.height)),
                        .srgbTarget = IsSrgb(format) ? 1u : 0u,
                        .pad        = 0};
    vkCmdPushConstants(cmd, m_Renderer.GetBindless().PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdDraw(cmd, static_cast<std::uint32_t>(quads * 6), 1, 0, 0);
    vkCmdEndRendering(cmd);
}

} // namespace Engine
