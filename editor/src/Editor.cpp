#include "Editor/Editor.h"
#include "FileDialog.h"
#include "History.h"
#include "ImGuiLayer.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Window.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

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

// Local TRS that puts an entity at `world` under its parent (false: not decomposable, e.g. skew).
bool SetWorldMatrix(Registry& registry, Entity entity, const glm::mat4& world)
{
    const Entity    parent      = registry.Get<Hierarchy>(entity).parent;
    const glm::mat4 parentWorld = parent != NullEntity ? registry.Get<WorldTransform>(parent).matrix : glm::mat4(1.0f);
    glm::vec3       scale, translation, skew;
    glm::vec4       perspective;
    glm::quat       rotation;
    if (!glm::decompose(glm::inverse(parentWorld) * world, scale, rotation, translation, skew, perspective))
        return false;
    Transform& t = registry.Get<Transform>(entity);
    t.position   = translation;
    t.rotation   = glm::normalize(rotation);
    t.scale      = scale;
    return true;
}
} // namespace

Editor::Editor(const EditorContext& context)
    : m_Ctx(context),
      m_ImGui(std::make_unique<ImGuiLayer>(context.window, context.renderer)),
      m_History(std::make_unique<History>()),
      m_FileDialog(std::make_unique<FileDialog>())
{
    m_Ctx.camera.moveRequiresLook       = true; // WASD would fight the W/E/R gizmo hotkeys otherwise
    m_Ctx.sceneRenderer.overlay.picking = true;
}

Editor::~Editor()
{
    m_Ctx.camera.moveRequiresLook = false;
    m_Ctx.sceneRenderer.overlay   = {};

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

bool Editor::CanUndo() const { return m_History->CanUndo(); }
bool Editor::CanRedo() const { return m_History->CanRedo(); }
bool Editor::HasUnsavedChanges() const { return m_History->Dirty(); }

void Editor::Update(float dt)
{
    m_FrameTimes[m_FrameTimeHead] = dt * 1000.0f;
    m_FrameTimeHead               = (m_FrameTimeHead + 1) % kHistory;

    // Pick requested a few frames ago (click in the viewport).
    if (const std::optional<Entity> picked = m_Ctx.sceneRenderer.TakePickResult())
        SelectFromClick(*picked, m_PickAdditive);
    ValidateSelection();

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
    DrawDialogs();

    HandleHotkeys();
    ApplyPendingEdits();
    ValidateSelection();

    // Inspector and gizmo edit local transforms: propagate before this frame is rendered.
    m_Ctx.scene.UpdateTransforms();
    UpdateSelectionOverlay();
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
        if (ImGui::MenuItem("New scene", "Ctrl+N"))
            RequestSceneChange([this] { NewScene(); });
        if (ImGui::MenuItem("Open scene...", "Ctrl+O"))
            RequestSceneChange([this] {
                m_DialogPurpose = DialogPurpose::OpenScene;
                m_FileDialog->Open("Open scene", FileDialog::Mode::Open,
                                   m_ScenePath.empty() ? std::filesystem::current_path() : m_ScenePath.parent_path(),
                                   {".json"});
            });
        if (ImGui::MenuItem("Save scene", "Ctrl+S")) {
            if (m_ScenePath.empty()) {
                m_DialogPurpose = DialogPurpose::SaveScene;
                m_FileDialog->Open("Save scene", FileDialog::Mode::Save, std::filesystem::current_path(),
                                   {".scene.json", ".json"}, "untitled.scene.json");
            } else {
                SaveScene(m_ScenePath);
            }
        }
        if (ImGui::MenuItem("Save scene as...", "Ctrl+Shift+S")) {
            m_DialogPurpose = DialogPurpose::SaveScene;
            m_FileDialog->Open("Save scene", FileDialog::Mode::Save,
                               m_ScenePath.empty() ? std::filesystem::current_path() : m_ScenePath.parent_path(),
                               {".scene.json", ".json"},
                               m_ScenePath.empty() ? "untitled.scene.json" : m_ScenePath.filename().string());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Esc"))
            m_Ctx.window.RequestClose();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const std::string undo = "Undo " + m_History->UndoLabel();
        const std::string redo = "Redo " + m_History->RedoLabel();
        if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, CanUndo()))
            Undo();
        if (ImGui::MenuItem(redo.c_str(), "Ctrl+Y", false, CanRedo()))
            Redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !m_Selection.empty()))
            DuplicateSelection();
        if (ImGui::MenuItem("Delete", "Del", false, !m_Selection.empty()))
            DeleteSelection();
        if (ImGui::MenuItem("Select none", nullptr, false, !m_Selection.empty()))
            Select(NullEntity);
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
    const std::string scene = m_ScenePath.empty() ? "untitled" : m_ScenePath.filename().string();
    ImGui::TextDisabled("  %s%s  %s", scene.c_str(), HasUnsavedChanges() ? "*" : "", m_Status.c_str());
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
    m_ViewportHovered  = ImGui::IsItemHovered();
    m_ViewportFocused  = ImGui::IsWindowFocused();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing();
    const bool iconHit = DrawLightOverlay(origin.x, origin.y, static_cast<float>(width), static_cast<float>(height), clicked);
    if (clicked && !iconHit) {
        // GPU picking: the entity under the cursor arrives a few frames later (Update).
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float  px    = mouse.x - origin.x;
        const float  py    = mouse.y - origin.y;
        if (px >= 0.0f && py >= 0.0f && px < static_cast<float>(width) && py < static_cast<float>(height)) {
            m_Ctx.sceneRenderer.RequestPick(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py));
            m_PickAdditive = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
        }
    }

    // Toolbar overlay.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, origin.y + 8.0f));
    const auto toolButton = [&](const char* label, bool active) {
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool pressed = ImGui::Button(label);
        if (active)
            ImGui::PopStyleColor();
        ImGui::SameLine();
        return pressed;
    };
    if (toolButton("Move", m_GizmoOperation == GizmoOperation::Translate))
        m_GizmoOperation = GizmoOperation::Translate;
    if (toolButton("Rotate", m_GizmoOperation == GizmoOperation::Rotate))
        m_GizmoOperation = GizmoOperation::Rotate;
    if (toolButton("Scale", m_GizmoOperation == GizmoOperation::Scale))
        m_GizmoOperation = GizmoOperation::Scale;
    if (toolButton(m_GizmoLocal ? "Local" : "World", false))
        m_GizmoLocal = !m_GizmoLocal;
    if (toolButton("Lights", m_ShowLightIcons))
        m_ShowLightIcons = !m_ShowLightIcons;
    ImGui::NewLine();

    DrawGizmo(origin.x, origin.y, static_cast<float>(width), static_cast<float>(height));
    ImGui::End();
}

void Editor::DrawGizmo(float x, float y, float width, float height)
{
    Registry&    registry = m_Ctx.scene.GetRegistry();
    const Entity primary  = Selected();
    if (primary == NullEntity) {
        m_GizmoEdit.reset();
        return;
    }

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(x, y, width, height);

    // The engine projection is reverse-Z with an infinite far plane; ImGuizmo handles both.
    const CameraData camera = m_Ctx.camera.GetData(width / std::max(height, 1.0f));
    const glm::mat4  before = registry.Get<WorldTransform>(primary).matrix;
    glm::mat4        world  = before;

    const ImGuizmo::OPERATION op = m_GizmoOperation == GizmoOperation::Translate ? ImGuizmo::TRANSLATE
                                   : m_GizmoOperation == GizmoOperation::Rotate  ? ImGuizmo::ROTATE
                                                                                 : ImGuizmo::SCALE;
    const ImGuizmo::MODE mode = m_GizmoLocal || op == ImGuizmo::SCALE ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const bool changed = ImGuizmo::Manipulate(glm::value_ptr(camera.view), glm::value_ptr(camera.projection), op, mode,
                                              glm::value_ptr(world));

    // One undo step per drag, covering every moved root.
    const std::vector<Entity> roots = SelectionRoots();
    if (ImGuizmo::IsUsing() && !m_GizmoEdit) {
        m_GizmoEdit.emplace();
        for (Entity e : roots)
            m_GizmoEdit->push_back({UuidOf(e), SnapshotEntityState(m_Ctx.scene, e)});
    }
    if (changed) {
        // The same world-space delta for all roots: they move / turn / scale about the primary's pivot.
        const glm::mat4 delta = world * glm::inverse(before);
        for (Entity e : roots)
            SetWorldMatrix(registry, e, e == primary ? world : delta * registry.Get<WorldTransform>(e).matrix);
    }
    if (!ImGuizmo::IsUsing() && m_GizmoEdit) {
        const char* label = op == ImGuizmo::TRANSLATE ? "Move" : op == ImGuizmo::ROTATE ? "Rotate" : "Scale";
        PushStateChange(label, std::move(*m_GizmoEdit));
        m_GizmoEdit.reset();
    }
}

namespace {

// World -> viewport pixels through clip space; segments are clipped against w = epsilon (in front of
// the camera). The engine flips Y with the viewport, so NDC +Y is up.
struct ViewportProjector {
    glm::mat4 viewProj;
    ImVec2    origin;
    ImVec2    size;

    [[nodiscard]] ImVec2 ToScreen(const glm::vec4& clip) const
    {
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        return {origin.x + (ndc.x * 0.5f + 0.5f) * size.x, origin.y + (0.5f - ndc.y * 0.5f) * size.y};
    }
    [[nodiscard]] bool Project(const glm::vec3& p, ImVec2& out) const
    {
        const glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
        if (clip.w <= 1e-4f)
            return false;
        out = ToScreen(clip);
        return true;
    }
    void Line(ImDrawList* list, const glm::vec3& a, const glm::vec3& b, ImU32 color) const
    {
        constexpr float kMinW = 1e-4f;
        glm::vec4       ca    = viewProj * glm::vec4(a, 1.0f);
        glm::vec4       cb    = viewProj * glm::vec4(b, 1.0f);
        if (ca.w <= kMinW && cb.w <= kMinW)
            return;
        if (ca.w <= kMinW)
            ca = glm::mix(ca, cb, (kMinW - ca.w) / (cb.w - ca.w));
        else if (cb.w <= kMinW)
            cb = glm::mix(cb, ca, (kMinW - cb.w) / (ca.w - cb.w));
        list->AddLine(ToScreen(ca), ToScreen(cb), color, 1.5f);
    }
    // Circle of `radius` around `center` in the plane spanned by u and v (unit vectors).
    void Circle(ImDrawList* list, const glm::vec3& center, const glm::vec3& u, const glm::vec3& v, float radius,
                ImU32 color) const
    {
        constexpr int kSegments = 48;
        glm::vec3     prev      = center + u * radius;
        for (int i = 1; i <= kSegments; ++i) {
            const float     t    = glm::two_pi<float>() * static_cast<float>(i) / kSegments;
            const glm::vec3 next = center + (u * std::cos(t) + v * std::sin(t)) * radius;
            Line(list, prev, next, color);
            prev = next;
        }
    }
};

ImU32 LightColor(const Light& light, float alpha)
{
    const glm::vec3 c = light.color / std::max({light.color.r, light.color.g, light.color.b, 1e-4f});
    return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, alpha));
}

} // namespace

bool Editor::DrawLightOverlay(float x, float y, float width, float height, bool clicked)
{
    if (!m_ShowLightIcons)
        return false;
    Registry&               registry = m_Ctx.scene.GetRegistry();
    const CameraData        camera   = m_Ctx.camera.GetData(width / std::max(height, 1.0f));
    const ViewportProjector projector{camera.projection * camera.view, ImVec2(x, y), ImVec2(width, height)};
    ImDrawList*             list = ImGui::GetWindowDrawList();
    list->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);

    constexpr float kIconRadius = 7.0f;
    const ImVec2    mouse       = ImGui::GetIO().MousePos;
    Entity          hit         = NullEntity;
    float           hitDistance = kIconRadius + 3.0f;
    registry.ViewOf<Light, WorldTransform>().Each([&](Entity e, Light& light, WorldTransform& world) {
        ImVec2 p;
        if (!projector.Project(glm::vec3(world.matrix[3]), p))
            return;
        const bool selected = IsSelected(e);
        list->AddCircleFilled(p, kIconRadius, LightColor(light, 0.9f));
        list->AddCircle(p, kIconRadius + 1.0f, selected ? IM_COL32(255, 200, 40, 255) : IM_COL32(0, 0, 0, 200), 0,
                        selected ? 2.5f : 1.5f);
        if (light.type == LightType::Spot) // direction tick
            projector.Line(list, glm::vec3(world.matrix[3]),
                           glm::vec3(world.matrix[3]) - glm::normalize(glm::vec3(world.matrix[2])) * (EffectiveRange(light) * 0.15f),
                           LightColor(light, 0.9f));
        const float d = std::hypot(mouse.x - p.x, mouse.y - p.y);
        if (d < hitDistance) {
            hitDistance = d;
            hit         = e;
        }
    });
    if (clicked && hit != NullEntity)
        SelectFromClick(hit, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);

    // Selected lights: range sphere (three great circles) or spot cone.
    for (Entity e : m_Selection) {
        if (!registry.Has<Light>(e))
            continue;
        const Light&     light = registry.Get<Light>(e);
        const glm::mat4& world = registry.Get<WorldTransform>(e).matrix;
        const glm::vec3  pos   = world[3];
        const float      range = EffectiveRange(light);
        const ImU32      color = LightColor(light, 0.8f);
        if (light.type == LightType::Point) {
            projector.Circle(list, pos, {1, 0, 0}, {0, 1, 0}, range, color);
            projector.Circle(list, pos, {1, 0, 0}, {0, 0, 1}, range, color);
            projector.Circle(list, pos, {0, 1, 0}, {0, 0, 1}, range, color);
        } else {
            const glm::vec3 dir  = glm::normalize(-glm::vec3(world[2]));
            const glm::vec3 u    = glm::normalize(glm::vec3(world[0]));
            const glm::vec3 v    = glm::normalize(glm::cross(dir, u));
            const auto      cone = [&](float angle, ImU32 c) {
                const glm::vec3 center = pos + dir * (range * std::cos(angle));
                const float     radius = range * std::sin(angle);
                projector.Circle(list, center, u, v, radius, c);
                for (const glm::vec3& edge : {u, -u, v, -v})
                    projector.Line(list, pos, center + edge * radius, c);
            };
            cone(light.outerConeAngle, color);
            if (light.innerConeAngle > 0.0f && light.innerConeAngle < light.outerConeAngle)
                cone(light.innerConeAngle, LightColor(light, 0.35f));
        }
    }
    list->PopClipRect();
    return clicked && hit != NullEntity;
}

void Editor::HandleHotkeys()
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || m_Ctx.camera.IsCaptured() || ImGui::IsAnyItemActive() || m_FileDialog->IsOpen())
        return;

    // Global shortcuts (any editor window).
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (io.KeyShift)
                Redo();
            else
                Undo();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
            Redo();
        if (ImGui::IsKeyPressed(ImGuiKey_D, false))
            DuplicateSelection();
        if (ImGui::IsKeyPressed(ImGuiKey_N, false))
            RequestSceneChange([this] { NewScene(); });
        if (ImGui::IsKeyPressed(ImGuiKey_O, false))
            RequestSceneChange([this] {
                m_DialogPurpose = DialogPurpose::OpenScene;
                m_FileDialog->Open("Open scene", FileDialog::Mode::Open,
                                   m_ScenePath.empty() ? std::filesystem::current_path() : m_ScenePath.parent_path(),
                                   {".json"});
            });
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            if (m_ScenePath.empty() || io.KeyShift) {
                m_DialogPurpose = DialogPurpose::SaveScene;
                m_FileDialog->Open("Save scene", FileDialog::Mode::Save,
                                   m_ScenePath.empty() ? std::filesystem::current_path() : m_ScenePath.parent_path(),
                                   {".scene.json", ".json"},
                                   m_ScenePath.empty() ? "untitled.scene.json" : m_ScenePath.filename().string());
            } else {
                SaveScene(m_ScenePath);
            }
        }
        return;
    }

    // Viewport tools.
    if (!(m_ViewportHovered || m_ViewportFocused))
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
        DeleteSelection();
}

void Editor::FocusSelected()
{
    const Entity primary = Selected();
    if (primary == NullEntity)
        return;
    // Look at the pivot from a comfortable distance, keeping the current viewing direction.
    const glm::vec3 target   = glm::vec3(m_Ctx.scene.GetRegistry().Get<WorldTransform>(primary).matrix[3]);
    const float     distance = std::clamp(glm::length(target - m_Ctx.camera.position), 1.0f, 10.0f);
    m_Ctx.camera.position    = target - m_Ctx.camera.Forward() * distance;
    m_Ctx.camera.LookAt(target);
}

// --- Selection --------------------------------------------------------------------------------

Entity Editor::Selected() const
{
    return m_Selection.empty() ? NullEntity : m_Selection.back();
}

void Editor::Select(Entity entity)
{
    m_Selection.clear();
    if (entity != NullEntity)
        m_Selection.push_back(entity);
}

void Editor::ToggleSelection(Entity entity)
{
    if (entity == NullEntity)
        return;
    if (const auto it = std::ranges::find(m_Selection, entity); it != m_Selection.end())
        m_Selection.erase(it);
    else
        m_Selection.push_back(entity);
}

bool Editor::IsSelected(Entity entity) const
{
    return std::ranges::find(m_Selection, entity) != m_Selection.end();
}

void Editor::SelectFromClick(Entity entity, bool additive)
{
    if (additive)
        ToggleSelection(entity);
    else
        Select(entity);
}

void Editor::ValidateSelection()
{
    const Registry& registry = m_Ctx.scene.GetRegistry();
    std::erase_if(m_Selection, [&](Entity e) { return !registry.Valid(e) || !registry.Has<Hierarchy>(e); });
}

std::vector<Entity> Editor::SelectionRoots() const
{
    std::vector<Entity> roots;
    for (Entity e : m_Selection) {
        const bool coveredByAncestor = std::ranges::any_of(
            m_Selection, [&](Entity other) { return other != e && m_Ctx.scene.IsAncestor(other, e); });
        if (!coveredByAncestor)
            roots.push_back(e);
    }
    return roots;
}

void Editor::UpdateSelectionOverlay()
{
    // Outline every mesh below the selected entities (selecting a model root outlines the model).
    const Registry&     registry = m_Ctx.scene.GetRegistry();
    std::vector<Entity> outlined;
    std::vector<Entity> stack(m_Selection.begin(), m_Selection.end());
    while (!stack.empty()) {
        const Entity e = stack.back();
        stack.pop_back();
        if (registry.Has<MeshRenderer>(e))
            outlined.push_back(e);
        const auto& children = registry.Get<Hierarchy>(e).children;
        stack.insert(stack.end(), children.begin(), children.end());
    }
    m_Ctx.sceneRenderer.overlay.picking  = true;
    m_Ctx.sceneRenderer.overlay.outlined = std::move(outlined);
}

} // namespace Engine
