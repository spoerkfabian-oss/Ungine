#include "ImGuiLayer.h"
#include "Engine/Core/Window.h"

#include <ImGuizmo.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace Engine {

namespace {
void CheckVk(VkResult result)
{
    if (result != VK_SUCCESS)
        ENGINE_ERROR("ImGui Vulkan backend: VkResult {}", static_cast<int>(result));
}

float SrgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// ImGui's colors are authored for a UNORM target; ours is sRGB (the hardware encodes on
// write). Linearize the style once so the UI looks as designed.
void LinearizeStyle(ImGuiStyle& style)
{
    for (ImVec4& c : style.Colors) {
        c.x = SrgbToLinear(c.x);
        c.y = SrgbToLinear(c.y);
        c.z = SrgbToLinear(c.z);
    }
}
} // namespace

ImGuiLayer::ImGuiLayer(Window& window, Renderer& renderer)
    : m_Renderer(renderer)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // no keyboard nav: arrows and letters stay engine hotkeys
    io.IniFilename = "editor.ini"; // layout, next to the working directory
    io.ConfigWindowsMoveFromTitleBarOnly = true; // dragging in the viewport must not move the window

    ImGui::StyleColorsDark();
    ImGuiStyle& style      = ImGui::GetStyle();
    style.WindowRounding   = 3.0f;
    style.FrameRounding    = 2.0f;
    style.WindowBorderSize = 1.0f;
    LinearizeStyle(style);

    // Chains the Window's own GLFW callbacks (engine input keeps working).
    if (!ImGui_ImplGlfw_InitForVulkan(window.Native(), true))
        throw std::runtime_error("ImGui GLFW backend init failed");

    const VulkanContext& ctx       = renderer.GetContext();
    const Swapchain&     swapchain = renderer.GetSwapchain();
    const VkFormat       format    = swapchain.Format();

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion         = VK_API_VERSION_1_3;
    info.Instance           = ctx.Instance();
    info.PhysicalDevice     = ctx.PhysicalDevice();
    info.Device             = ctx.Device();
    info.QueueFamily        = ctx.GraphicsQueue().family;
    info.Queue              = ctx.GraphicsQueue().handle;
    info.DescriptorPoolSize = 64; // backend-owned pool: font atlas + viewport textures
    info.MinImageCount      = 2;
    // Vertex/index buffers are cycled per RenderDrawData call: must exceed the frames in flight.
    info.ImageCount          = std::max(swapchain.ImageCount(), kFramesInFlight + 1);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.MSAASamples                                         = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format; // copied during Init
    info.CheckVkResultFn = CheckVk;
    if (!ImGui_ImplVulkan_Init(&info)) {
        ImGui_ImplGlfw_Shutdown();
        throw std::runtime_error("ImGui Vulkan backend init failed");
    }
}

ImGuiLayer::~ImGuiLayer()
{
    m_Renderer.GetContext().WaitIdle();
    for (const PendingRemoval& p : m_PendingRemovals)
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(p.texture));
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::NewFrame()
{
    if (m_FrameOpen)
        ImGui::EndFrame(); // the previous frame was never rendered (e.g. swapchain out of date)

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    m_FrameOpen = true;
}

void ImGuiLayer::Render(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent)
{
    ImGui::Render();
    m_FrameOpen = false;

    // Counted in recorded frames (each one waited on its slot's fence): once kFramesInFlight + 1
    // have passed, no submitted command buffer references the texture any more.
    std::erase_if(m_PendingRemovals, [](PendingRemoval& p) {
        if (p.framesLeft-- > 0)
            return false;
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(p.texture));
        return true;
    });

    VkRenderingAttachmentInfo color{};
    color.sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView        = target;
    color.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.01f, 0.01f, 0.012f, 1.0f}};

    VkRenderingInfo info{};
    info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea           = {{0, 0}, extent};
    info.layerCount           = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments    = &color;

    vkCmdBeginRendering(cmd, &info);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

ImTextureID ImGuiLayer::AddTexture(VkImageView view, VkImageLayout layout)
{
    return reinterpret_cast<ImTextureID>(ImGui_ImplVulkan_AddTexture(view, layout));
}

void ImGuiLayer::RemoveTexture(ImTextureID texture)
{
    m_PendingRemovals.push_back({texture, kFramesInFlight + 1});
}

} // namespace Engine
