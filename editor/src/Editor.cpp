#include "Editor/Editor.h"
#include "ImGuiLayer.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Window.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"

#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <utility>

namespace Engine {

namespace {
constexpr VkFormat kViewportFormat = VK_FORMAT_R8G8B8A8_SRGB; // sampled by ImGui, encoded like the swapchain
} // namespace

Editor::Editor(const EditorContext& context)
    : m_Ctx(context), m_ImGui(std::make_unique<ImGuiLayer>(context.window, context.renderer))
{
    m_Ctx.camera.moveRequiresLook = true; // WASD would fight the W/E/R gizmo hotkeys otherwise
}

Editor::~Editor()
{
    for (ModelHandle h : m_OwnedModels)
        if (m_Ctx.assets.State(h) != AssetState::Invalid)
            m_Ctx.assets.Release(h);
    m_Ctx.camera.moveRequiresLook = false;

    if (m_ViewportImage) {
        m_ImGui->RemoveTexture(m_ViewportTexture);
        m_Ctx.renderer.DeferRelease(std::move(m_ViewportImage));
    }
    m_ImGui.reset(); // waits for the device, frees pending textures, shuts the backends down
}

bool Editor::WantsKeyboard() const
{
    return ImGui::GetIO().WantCaptureKeyboard; // text field or active widget (keyboard nav is off)
}

float Editor::ViewportAspect() const
{
    const VkExtent2D e = m_ViewportImage ? m_ViewportImage.Extent2D() : VkExtent2D{16, 9};
    return static_cast<float>(e.width) / static_cast<float>(std::max(e.height, 1u));
}

void Editor::Update(float dt)
{
    m_FrameTimes[m_FrameTimeHead] = dt * 1000.0f;
    m_FrameTimeHead               = (m_FrameTimeHead + 1) % kHistory;

    Registry& registry = m_Ctx.scene.GetRegistry();
    if (m_Selected != NullEntity && !registry.Valid(m_Selected))
        m_Selected = NullEntity;

    // Mouse look hides and warps the cursor: keep ImGui from hovering widgets meanwhile.
    ImGuiIO& io = ImGui::GetIO();
    if (m_Ctx.camera.IsCaptured())
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

    m_ImGui->NewFrame();
    DrawMenuBar();

    const ImGuiID dockspace = ImGui::GetID("EditorDockSpace");
    if (!ImGui::DockBuilderGetNode(dockspace))
        BuildDefaultLayout(dockspace); // first run (no editor.ini yet)
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport());

    DrawViewport();
    if (m_ShowHierarchy)
        DrawHierarchy();
    if (m_ShowInspector)
        DrawInspector();
    if (m_ShowRenderer)
        DrawRendererSettings();
    if (m_ShowAssets)
        DrawAssets();
    if (m_ShowStats) // after Assets: the visible tab of the shared dock node on first run
        DrawStats();
    if (m_ShowDemo)
        ImGui::ShowDemoWindow(&m_ShowDemo);

    HandleHotkeys();
    ApplyPendingEdits();

    // Inspector and gizmo edit local transforms: propagate before this frame is rendered.
    m_Ctx.scene.UpdateTransforms();
}

void Editor::Render(const FrameContext& frame)
{
    const VkCommandBuffer cmd = frame.cmd;
    if (m_ViewportImage) {
        // Shared by all frames in flight: the previous frame's UI pass may still sample it.
        CmdImageBarrier(cmd, {.image     = m_ViewportImage.Handle(),
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              .srcStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                              .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
        m_Ctx.sceneRenderer.Render(frame, m_Ctx.scene, m_Ctx.camera.GetData(ViewportAspect()),
                                   {.image  = m_ViewportImage.Handle(),
                                    .view   = m_ViewportImage.View(),
                                    .format = kViewportFormat,
                                    .extent = m_ViewportImage.Extent2D()});
        CmdImageBarrier(cmd, {.image     = m_ViewportImage.Handle(),
                              .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                              .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                              .dstStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
    }

    GpuScope scope(&m_Ctx.renderer.Profiler(), cmd, "Editor UI");
    m_ImGui->Render(cmd, frame.view, frame.extent);
}

void Editor::EnsureViewportTarget(std::uint32_t width, std::uint32_t height)
{
    if (m_ViewportImage && m_ViewportImage.Extent().width == width && m_ViewportImage.Extent().height == height)
        return;
    if (m_ViewportImage) { // resized: frames in flight may still draw the old one
        m_ImGui->RemoveTexture(m_ViewportTexture);
        m_Ctx.renderer.DeferRelease(std::move(m_ViewportImage));
    }
    m_ViewportImage   = Image(m_Ctx.renderer.GetContext(),
                              {.extent    = {width, height, 1},
                               .format    = kViewportFormat,
                               .usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               .debugName = "EditorViewport"});
    m_ViewportTexture = m_ImGui->AddTexture(m_ViewportImage.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Editor::BuildDefaultLayout(ImGuiID dockspace)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);

    ImGuiID center = dockspace;
    const ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    const ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    ImGuiID       rightBottom = 0;
    const ImGuiID rightTop    = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.45f, nullptr, &rightBottom);

    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", rightTop);
    ImGui::DockBuilderDockWindow("Renderer", rightBottom);
    ImGui::DockBuilderDockWindow("Stats", bottom);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderFinish(dockspace);
}

void Editor::DrawMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
        return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Exit", "Esc"))
            m_Ctx.window.RequestClose();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Hierarchy", nullptr, &m_ShowHierarchy);
        ImGui::MenuItem("Inspector", nullptr, &m_ShowInspector);
        ImGui::MenuItem("Renderer", nullptr, &m_ShowRenderer);
        ImGui::MenuItem("Stats", nullptr, &m_ShowStats);
        ImGui::MenuItem("Assets", nullptr, &m_ShowAssets);
        ImGui::Separator();
        ImGui::MenuItem("ImGui demo", nullptr, &m_ShowDemo);
        ImGui::EndMenu();
    }
    ImGui::TextDisabled("  F1: editor on/off | RMB + WASD/QE: fly | W/E/R: move/rotate/scale | F: focus | Del");
    ImGui::EndMainMenuBar();
}

void Editor::DrawViewport()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin("Viewport");
    ImGui::PopStyleVar();
    if (!visible) {
        m_ViewportHovered = false;
        ImGui::End();
        return;
    }

    const ImVec2        avail  = ImGui::GetContentRegionAvail();
    const std::uint32_t width  = static_cast<std::uint32_t>(std::max(avail.x, 1.0f));
    const std::uint32_t height = static_cast<std::uint32_t>(std::max(avail.y, 1.0f));
    EnsureViewportTarget(width, height);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Image(ImTextureRef(m_ViewportTexture), ImVec2(static_cast<float>(width), static_cast<float>(height)));
    m_ViewportHovered = ImGui::IsItemHovered();
    m_ViewportFocused = ImGui::IsWindowFocused();

    // Toolbar overlay.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, origin.y + 8.0f));
    const auto toolButton = [&](const char* label, bool active) {
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool clicked = ImGui::Button(label);
        if (active)
            ImGui::PopStyleColor();
        ImGui::SameLine();
        return clicked;
    };
    if (toolButton("Move", m_GizmoOperation == GizmoOperation::Translate))
        m_GizmoOperation = GizmoOperation::Translate;
    if (toolButton("Rotate", m_GizmoOperation == GizmoOperation::Rotate))
        m_GizmoOperation = GizmoOperation::Rotate;
    if (toolButton("Scale", m_GizmoOperation == GizmoOperation::Scale))
        m_GizmoOperation = GizmoOperation::Scale;
    if (toolButton(m_GizmoLocal ? "Local" : "World", false))
        m_GizmoLocal = !m_GizmoLocal;
    ImGui::NewLine();

    DrawGizmo(origin.x, origin.y, static_cast<float>(width), static_cast<float>(height));
    ImGui::End();
}

void Editor::DrawGizmo(float x, float y, float width, float height)
{
    Registry& registry = m_Ctx.scene.GetRegistry();
    if (m_Selected == NullEntity || !registry.Has<Transform>(m_Selected))
        return;

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(x, y, width, height);

    // The engine projection is reverse-Z with an infinite far plane; ImGuizmo handles both.
    const CameraData camera = m_Ctx.camera.GetData(width / std::max(height, 1.0f));
    glm::mat4        world  = registry.Get<WorldTransform>(m_Selected).matrix;

    const ImGuizmo::OPERATION op = m_GizmoOperation == GizmoOperation::Translate ? ImGuizmo::TRANSLATE
                                   : m_GizmoOperation == GizmoOperation::Rotate  ? ImGuizmo::ROTATE
                                                                                 : ImGuizmo::SCALE;
    const ImGuizmo::MODE mode = m_GizmoLocal || op == ImGuizmo::SCALE ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    if (!ImGuizmo::Manipulate(glm::value_ptr(camera.view), glm::value_ptr(camera.projection), op, mode,
                              glm::value_ptr(world)))
        return;

    // Back to the local transform relative to the parent.
    const Entity    parent      = registry.Get<Hierarchy>(m_Selected).parent;
    const glm::mat4 parentWorld = parent != NullEntity ? registry.Get<WorldTransform>(parent).matrix : glm::mat4(1.0f);
    glm::vec3       scale, translation, skew;
    glm::vec4       perspective;
    glm::quat       rotation;
    if (glm::decompose(glm::inverse(parentWorld) * world, scale, rotation, translation, skew, perspective)) {
        Transform& t = registry.Get<Transform>(m_Selected);
        t.position   = translation;
        t.rotation   = glm::normalize(rotation);
        t.scale      = scale;
    }
}

void Editor::HandleHotkeys()
{
    if (ImGui::GetIO().WantTextInput || m_Ctx.camera.IsCaptured() || !(m_ViewportHovered || m_ViewportFocused))
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false))
        m_GizmoOperation = GizmoOperation::Translate;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false))
        m_GizmoOperation = GizmoOperation::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false))
        m_GizmoOperation = GizmoOperation::Scale;
    if (ImGui::IsKeyPressed(ImGuiKey_F, false))
        FocusSelected();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        DeleteSelected();
}

void Editor::ApplyPendingEdits()
{
    Registry& registry = m_Ctx.scene.GetRegistry();
    if (m_PendingDelete != NullEntity) {
        if (registry.Valid(m_PendingDelete)) {
            m_Ctx.scene.DestroyEntity(m_PendingDelete);
            if (!registry.Valid(m_Selected))
                m_Selected = NullEntity;
        }
        m_PendingDelete = NullEntity;
    }

    const Entity child = std::exchange(m_ReparentChild, NullEntity);
    const Entity parent = std::exchange(m_ReparentTo, NullEntity);
    if (!registry.Valid(child) || (parent != NullEntity && !registry.Valid(parent)) || child == parent)
        return;
    // Refuse cycles: the new parent must not live below the child.
    for (Entity e = parent; e != NullEntity; e = registry.Get<Hierarchy>(e).parent)
        if (e == child)
            return;
    if (registry.Get<Hierarchy>(child).parent == parent)
        return;

    // Keep the world transform (world matrices are from the last UpdateTransforms).
    const glm::mat4 childWorld  = registry.Get<WorldTransform>(child).matrix;
    const glm::mat4 parentWorld = parent != NullEntity ? registry.Get<WorldTransform>(parent).matrix : glm::mat4(1.0f);
    m_Ctx.scene.SetParent(child, parent);
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    if (glm::decompose(glm::inverse(parentWorld) * childWorld, scale, rotation, translation, skew, perspective)) {
        Transform& t = registry.Get<Transform>(child);
        t.position   = translation;
        t.rotation   = glm::normalize(rotation);
        t.scale      = scale;
    }
}

void Editor::DeleteSelected()
{
    if (m_Selected != NullEntity)
        m_PendingDelete = m_Selected;
}

void Editor::FocusSelected()
{
    Registry& registry = m_Ctx.scene.GetRegistry();
    if (m_Selected == NullEntity || !registry.Has<WorldTransform>(m_Selected))
        return;
    // Look at the pivot from a comfortable distance, keeping the current viewing direction.
    const glm::vec3 target   = glm::vec3(registry.Get<WorldTransform>(m_Selected).matrix[3]);
    const float     distance = std::clamp(glm::length(target - m_Ctx.camera.position), 1.0f, 10.0f);
    m_Ctx.camera.position    = target - m_Ctx.camera.Forward() * distance;
    m_Ctx.camera.LookAt(target);
}

} // namespace Engine
