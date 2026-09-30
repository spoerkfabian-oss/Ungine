#include "Editor/Editor.h"
#include "Editor/ScriptGraphEditor.h"
#include "FileDialog.h"
#include "History.h"
#include "ImGuiLayer.h"

#include "Engine/Assets/AssetManager.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"
#include "Engine/Core/Window.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptSystem.h"

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
bool SetWorldMatrix(Scene& scene, Entity entity, const glm::mat4& world)
{
    const Registry& registry    = scene.GetRegistry();
    const Entity    parent      = registry.Get<Hierarchy>(entity).parent;
    const glm::mat4 parentWorld = parent != NullEntity ? registry.Get<WorldTransform>(parent).matrix : glm::mat4(1.0f);
    glm::vec3       scale, translation, skew;
    glm::vec4       perspective;
    glm::quat       rotation;
    if (!glm::decompose(glm::inverse(parentWorld) * world, scale, rotation, translation, skew, perspective))
        return false;
    Transform& t = scene.EditTransform(entity);
    t.position   = translation;
    t.rotation   = glm::normalize(rotation);
    t.scale      = scale;
    return true;
}
} // namespace

Editor::Editor(const EditorContext& context)
    : m_Ctx(context),
      m_ImGui(std::make_unique<ImGuiLayer>(context.window, context.renderer, PathToUtf8(context.layoutFile))),
      m_History(std::make_unique<History>()),
      m_FileDialog(std::make_unique<FileDialog>()),
      m_Graphs(std::make_unique<ScriptGraphEditor>())
{
    m_Ctx.camera.moveRequiresLook       = true; // WASD would fight the W/E/R gizmo hotkeys otherwise
    m_Ctx.sceneRenderer.overlay.picking = true;
}

Editor::~Editor()
{
    Stop(); // leaving the editor while playing returns to the edit scene
    m_Ctx.camera.moveRequiresLook = false;
    m_Ctx.sceneRenderer.overlay   = {};

    if (m_ViewportImage) {
        m_ImGui->RemoveTexture(m_ViewportTexture);
        m_Ctx.renderer.DeferRelease(std::move(m_ViewportImage));
    }
    ReleaseTexturePreviews(true);
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

    if (m_ShowBlueprint) // before the viewport: new dock tabs are selected in submission order
        m_Graphs->Draw(&m_ShowBlueprint, m_Ctx.scripts);
    DrawViewport();
    if (m_ShowHierarchy)
        DrawHierarchy();
    if (m_ShowInspector)
        DrawInspector();
    if (m_ShowRenderer)
        DrawRendererSettings();
    if (m_ShowAssets)
        DrawAssets();
    if (m_ShowProjectSettings)
        DrawProjectSettings();
    if (m_ShowStats)
        DrawStats();
    if (m_ShowContent) // last of the bottom dock node: its visible tab on first run
        DrawContentBrowser();
    if (m_ShowDemo)
        ImGui::ShowDemoWindow(&m_ShowDemo);
    DrawDialogs();

    HandleHotkeys();
    ApplyPendingEdits();
    UpdatePendingInstances();
    ValidateSelection();

    // Scripts tick with the frame while playing; they see the keyboard when the viewport has it.
    if (m_Ctx.scripts && m_PlayState == PlayState::Playing)
        m_Ctx.scripts->Update(m_Ctx.scene, dt, (m_ViewportHovered || m_ViewportFocused) && !WantsKeyboard());

    // Inspector and gizmo edit local transforms: propagate before this frame is rendered.
    m_Ctx.scene.UpdateTransforms();
    // Audio follows the scene (while playing) and the editor camera (previews, no listener entity).
    if (m_Ctx.audio) {
        const CameraData view = m_Ctx.camera.GetData(ViewportAspect());
        m_Ctx.audio->Update(m_Ctx.scene, dt, &view);
    }
    // Edit mode: bodies follow the scene (collider overlay, queries); Play steps in FixedUpdate.
    if (m_Ctx.physics && m_PlayState == PlayState::Edit)
        m_Ctx.physics->Sync(m_Ctx.scene);
    UpdateSelectionOverlay();
}

void Editor::Render(const FrameContext& frame, float physicsAlpha)
{
    // Paused: the fixed tick keeps running without steps, so show the last step as it is.
    if (m_Ctx.physics && m_PlayState != PlayState::Edit)
        m_Ctx.physics->Interpolate(m_Ctx.scene, m_PlayState == PlayState::Playing ? physicsAlpha : 1.0f);

    const VkCommandBuffer cmd = frame.cmd;
    if (m_ViewportImage && m_ViewportVisible) {
        // Shared by all frames in flight: the previous frame's UI pass may still sample it.
        CmdImageBarrier(cmd, {.image     = m_ViewportImage.Handle(),
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              .srcStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                              .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
        CameraData camera = m_Ctx.camera.GetData(ViewportAspect());
        if (m_GameCamera) // preview through the game camera (what the player shows)
            if (const Entity e = m_Ctx.scene.FindPrimaryCamera(); e != NullEntity) {
                const CameraComponent& cam = m_Ctx.scene.GetRegistry().Get<CameraComponent>(e);
                camera = CameraFromWorld(m_Ctx.scene.GetRegistry().Get<WorldTransform>(e).matrix, cam.fovY, cam.nearPlane,
                                         ViewportAspect());
            }
        m_Ctx.sceneRenderer.Render(frame, m_Ctx.scene, camera,
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

    ImGui::DockBuilderDockWindow("Content", bottom);
    ImGui::DockBuilderDockWindow("Blueprint", center); // tab behind the viewport
    m_FocusViewport = 2;
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
                                   m_ScenePath.empty() ? ContentRoot() : m_ScenePath.parent_path(),
                                   {".json"});
            });
        if (ImGui::MenuItem("Save scene", "Ctrl+S")) {
            if (m_ScenePath.empty()) {
                m_DialogPurpose = DialogPurpose::SaveScene;
                m_FileDialog->Open("Save scene", FileDialog::Mode::Save, ContentRoot(),
                                   {".scene.json", ".json"}, "untitled.scene.json");
            } else {
                SaveScene(m_ScenePath);
            }
        }
        if (ImGui::MenuItem("Save scene as...", "Ctrl+Shift+S")) {
            m_DialogPurpose = DialogPurpose::SaveScene;
            m_FileDialog->Open("Save scene", FileDialog::Mode::Save,
                               m_ScenePath.empty() ? ContentRoot() : m_ScenePath.parent_path(),
                               {".scene.json", ".json"},
                               m_ScenePath.empty() ? "untitled.scene.json" : m_ScenePath.filename().string());
        }
        if (ImGui::MenuItem("Save all"))
            SaveAll();
        ImGui::Separator();
        if (m_Ctx.project) {
            if (ImGui::MenuItem("Project settings..."))
                m_ShowProjectSettings = true;
            if (ImGui::MenuItem("Show project folder"))
                (void)OpenInFileBrowser(m_Ctx.project->Root());
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Exit", "Esc"))
            m_Ctx.window.RequestClose();
        ImGui::EndMenu();
    }
    if (m_Ctx.project && ImGui::BeginMenu("Build")) {
        if (ImGui::MenuItem("Build & Run", "Ctrl+B"))
            BuildAndRun();
        if (ImGui::MenuItem("Package project...")) {
            m_DialogPurpose = DialogPurpose::Package;
            m_FileDialog->Open("Package into folder", FileDialog::Mode::Folder, m_Ctx.project->Root().parent_path(), {});
        }
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
        ImGui::MenuItem("Content browser", nullptr, &m_ShowContent);
        if (ImGui::MenuItem("Blueprint", nullptr, &m_ShowBlueprint) && m_ShowBlueprint)
            m_Graphs->Focus();
        ImGui::Separator();
        ImGui::MenuItem("ImGui demo", nullptr, &m_ShowDemo);
        ImGui::EndMenu();
    }
    if (m_Ctx.physics || m_Ctx.scripts) {
        ImGui::Separator();
        const bool playing = m_PlayState == PlayState::Playing;
        if (playing)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
        if (ImGui::MenuItem(m_PlayState == PlayState::Edit ? "Play" : "Resume", "Ctrl+P", playing, !playing))
            Play();
        if (playing)
            ImGui::PopStyleColor();
        if (ImGui::MenuItem("Pause", nullptr, m_PlayState == PlayState::Paused, playing))
            Pause();
        if (ImGui::MenuItem("Step", nullptr, false, m_PlayState != PlayState::Edit))
            StepOnce();
        if (ImGui::MenuItem("Stop", "Ctrl+P", false, m_PlayState != PlayState::Edit))
            Stop();
        ImGui::Separator();
    }
    const std::string scene = m_ScenePath.empty() ? "untitled" : m_ScenePath.filename().string();
    ImGui::TextDisabled("  %s%s  %s", scene.c_str(), HasUnsavedChanges() ? "*" : "", m_Status.c_str());
    ImGui::EndMainMenuBar();
}

void Editor::DrawViewport()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // Default layout: the viewport becomes the visible tab once the dock node has its tabs (new
    // tabs are selected in their first frame, focus wins afterwards).
    if (m_FocusViewport > 0 && --m_FocusViewport == 0)
        ImGui::SetNextWindowFocus();
    const bool visible = ImGui::Begin("Viewport");
    ImGui::PopStyleVar();
    m_ViewportVisible = visible; // hidden tab / collapsed: the scene is not rendered (Render)
    if (!visible) {
        m_ViewportHovered = false;
        m_ViewportPress = m_BoxSelecting = false;
        ImGui::End();
        return;
    }

    const ImVec2        avail  = ImGui::GetContentRegionAvail();
    const std::uint32_t width  = static_cast<std::uint32_t>(std::max(avail.x, 1.0f));
    const std::uint32_t height = static_cast<std::uint32_t>(std::max(avail.y, 1.0f));
    EnsureViewportTarget(width, height);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Image(ImTextureRef(m_ViewportTexture), ImVec2(static_cast<float>(width), static_cast<float>(height)));
    // Content browser drops: models are placed where the cursor points, blueprints go to the selection.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("UNGINE_CONTENT")) {
            const std::filesystem::path file = PathFromUtf8(static_cast<const char*>(payload->Data));
            const std::string           ext  = file.extension().string();
            if (ext == ".ugraph") {
                if (Selected() != NullEntity)
                    AssignScript(Selected(), file);
                else
                    m_Status = "Select an entity to give it the script";
            } else if (IsSoundFile(file)) { // a new audio source where the cursor points
                const ImVec2     mouse = ImGui::GetIO().MousePos;
                const glm::vec2  ndc((mouse.x - origin.x) / static_cast<float>(width) * 2.0f - 1.0f,
                                     1.0f - (mouse.y - origin.y) / static_cast<float>(height) * 2.0f);
                const CameraData cam = m_Ctx.camera.GetData(static_cast<float>(width) / std::max(static_cast<float>(height), 1.0f));
                const glm::vec4  far = glm::inverse(cam.projection * cam.view) * glm::vec4(ndc, 0.5f, 1.0f);
                const glm::vec3  dir = glm::normalize(glm::vec3(far) / far.w - cam.position);
                glm::vec3        target = cam.position + dir * std::max(m_Ctx.camera.moveSpeed, 1.0f) * 2.0f;
                if (const auto hit = m_Ctx.sceneRenderer.Spatial().Raycast(cam.position, dir, 10000.0f))
                    target = cam.position + dir * hit->distance;
                CreateAudioEntity(file, target);
            } else {
                const std::size_t before = m_PendingInstances.size();
                OpenAsset(file);
                if (m_PendingInstances.size() > before) { // a model: under the cursor
                    const ImVec2     mouse  = ImGui::GetIO().MousePos;
                    const glm::vec2  ndc((mouse.x - origin.x) / static_cast<float>(width) * 2.0f - 1.0f,
                                         1.0f - (mouse.y - origin.y) / static_cast<float>(height) * 2.0f);
                    const CameraData cam    = m_Ctx.camera.GetData(static_cast<float>(width) / std::max(static_cast<float>(height), 1.0f));
                    const glm::mat4  inv    = glm::inverse(cam.projection * cam.view);
                    const glm::vec4  far    = inv * glm::vec4(ndc, 0.5f, 1.0f);
                    const glm::vec3  dir    = glm::normalize(glm::vec3(far) / far.w - cam.position);
                    glm::vec3        target = cam.position + dir * std::max(m_Ctx.camera.moveSpeed, 1.0f) * 2.0f;
                    if (const auto hit = m_Ctx.sceneRenderer.Spatial().Raycast(cam.position, dir, 10000.0f))
                        target = cam.position + dir * hit->distance;
                    else if (dir.y < -1e-3f)
                        target = cam.position + dir * (-cam.position.y / dir.y); // ground plane y = 0
                    m_PendingInstances.back().second = target;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    m_ViewportHovered  = ImGui::IsItemHovered();
    m_ViewportFocused  = ImGui::IsWindowFocused();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing();
    bool iconHit = DrawLightOverlay(origin.x, origin.y, static_cast<float>(width), static_cast<float>(height), clicked);
    if (m_ShowAudio)
        iconHit = DrawAudioOverlay(origin.x, origin.y, static_cast<float>(width), static_cast<float>(height), clicked && !iconHit) || iconHit;
    if (m_ShowBvh)
        DrawBvhOverlay(origin.x, origin.y, static_cast<float>(width), static_cast<float>(height));
    if (m_ShowColliders && m_Ctx.physics)
        DrawColliderOverlay(origin.x, origin.y, static_cast<float>(width), static_cast<float>(height));
    // Script prints (like UE's on-screen debug messages), newest at the top.
    if (m_Ctx.scripts && m_Ctx.scripts->Running()) {
        const auto  messages = m_Ctx.scripts->Messages();
        ImDrawList* list     = ImGui::GetWindowDrawList();
        float       y        = origin.y + 40.0f;
        for (auto it = messages.rbegin(); it != messages.rend() && y < origin.y + static_cast<float>(height) - 20.0f; ++it) {
            const ImU32 color = it->error ? IM_COL32(255, 90, 90, 255) : IM_COL32(120, 200, 255, 255);
            list->AddText(ImVec2(origin.x + 11.0f, y + 1.0f), IM_COL32(0, 0, 0, 200), it->text.c_str());
            list->AddText(ImVec2(origin.x + 10.0f, y), color, it->text.c_str());
            y += ImGui::GetTextLineHeightWithSpacing();
        }
    }
    if (m_PlayState != PlayState::Edit) // frame: this is the simulated scene, changes are temporary
        ImGui::GetWindowDrawList()->AddRect(origin, ImVec2(origin.x + static_cast<float>(width), origin.y + static_cast<float>(height)),
                                            m_PlayState == PlayState::Playing ? IM_COL32(60, 200, 90, 255)
                                                                              : IM_COL32(230, 170, 40, 255),
                                            0.0f, 3.0f);
    // Left button: a click picks (on release), a drag selects everything in the box.
    const ImGuiIO&  io    = ImGui::GetIO();
    const glm::vec2 mouse = glm::vec2(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
    if (clicked && !iconHit) {
        m_ViewportPress = true;
        m_BoxSelecting  = false;
        m_PressPos      = mouse;
    }
    if (m_ViewportPress) {
        const glm::vec2 lo = glm::min(m_PressPos, mouse), hi = glm::max(m_PressPos, mouse);
        if (!m_BoxSelecting && glm::distance(m_PressPos, mouse) > 4.0f && !ImGuizmo::IsUsing())
            m_BoxSelecting = true;
        if (m_BoxSelecting) {
            ImDrawList* list = ImGui::GetWindowDrawList();
            const ImVec2 a(origin.x + lo.x, origin.y + lo.y), b(origin.x + hi.x, origin.y + hi.y);
            list->AddRectFilled(a, b, IM_COL32(90, 150, 255, 40));
            list->AddRect(a, b, IM_COL32(90, 150, 255, 200));
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const bool additive = io.KeyCtrl || io.KeyShift;
            if (m_BoxSelecting) {
                SelectInRect(lo, hi, additive);
            } else if (m_PressPos.x >= 0.0f && m_PressPos.y >= 0.0f && m_PressPos.x < static_cast<float>(width) &&
                       m_PressPos.y < static_cast<float>(height)) {
                // GPU picking: the entity under the cursor arrives a few frames later (Update).
                m_Ctx.sceneRenderer.RequestPick(static_cast<std::uint32_t>(m_PressPos.x),
                                                static_cast<std::uint32_t>(m_PressPos.y));
                m_PickAdditive = additive;
            }
            m_ViewportPress = m_BoxSelecting = false;
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
    if (toolButton("BVH", m_ShowBvh))
        m_ShowBvh = !m_ShowBvh;
    if (m_Ctx.physics && toolButton("Colliders", m_ShowColliders))
        m_ShowColliders = !m_ShowColliders;
    if (toolButton("Audio", m_ShowAudio))
        m_ShowAudio = !m_ShowAudio;
    if (toolButton("Game cam", m_GameCamera))
        m_GameCamera = !m_GameCamera;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Look through the scene's primary Camera component (as the player will)");
    ImGui::NewLine();

    if (!m_GameCamera) // the gizmo works in the editor camera
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
            SetWorldMatrix(m_Ctx.scene, e, e == primary ? world : delta * registry.Get<WorldTransform>(e).matrix);
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

bool Editor::DrawAudioOverlay(float x, float y, float width, float height, bool clicked)
{
    Registry&               registry = m_Ctx.scene.GetRegistry();
    const CameraData        camera   = m_Ctx.camera.GetData(width / std::max(height, 1.0f));
    const ViewportProjector projector{camera.projection * camera.view, ImVec2(x, y), ImVec2(width, height)};
    ImDrawList*             list = ImGui::GetWindowDrawList();
    list->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);

    // Reverb zones: oriented boxes (entity transform).
    registry.ViewOf<ReverbZone, WorldTransform>().Each([&](Entity e, ReverbZone& zone, WorldTransform& world) {
        const glm::vec3& h = zone.halfExtents;
        glm::vec3        c[8];
        for (int i = 0; i < 8; ++i)
            c[i] = glm::vec3(world.matrix * glm::vec4((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z, 1.0f));
        static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                              {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        const ImU32 color = IsSelected(e) ? IM_COL32(90, 255, 220, 255) : IM_COL32(60, 190, 170, 140);
        for (const auto& edge : kEdges)
            projector.Line(list, c[edge[0]], c[edge[1]], color);
    });

    // Sources: speaker icons (click selects), distance spheres of the selected ones.
    constexpr float kIcon       = 7.0f;
    const ImVec2    mouse       = ImGui::GetIO().MousePos;
    Entity          hit         = NullEntity;
    float           hitDistance = kIcon + 3.0f;
    registry.ViewOf<AudioSource, WorldTransform>().Each([&](Entity e, AudioSource& source, WorldTransform& world) {
        const glm::vec3 pos = world.matrix[3];
        ImVec2          p;
        if (!projector.Project(pos, p))
            return;
        const bool  selected = IsSelected(e);
        const bool  playing  = m_Ctx.audio && m_Ctx.audio->Running() && m_Ctx.audio->IsPlaying(e);
        const ImU32 fill     = playing ? IM_COL32(120, 230, 120, 240) : IM_COL32(120, 190, 255, 230);
        const ImU32 outline  = selected ? IM_COL32(255, 200, 40, 255) : IM_COL32(0, 0, 0, 200);
        // Speaker: box + cone.
        const ImVec2 box0(p.x - kIcon, p.y - kIcon * 0.4f), box1(p.x - kIcon * 0.3f, p.y + kIcon * 0.4f);
        list->AddRectFilled(box0, box1, fill);
        list->AddTriangleFilled(ImVec2(box1.x, p.y - kIcon * 0.4f), ImVec2(p.x + kIcon, p.y - kIcon),
                                ImVec2(p.x + kIcon, p.y + kIcon), fill);
        list->AddQuadFilled(ImVec2(box1.x, p.y - kIcon * 0.4f), ImVec2(p.x + kIcon, p.y - kIcon),
                            ImVec2(p.x + kIcon, p.y + kIcon), ImVec2(box1.x, p.y + kIcon * 0.4f), fill);
        list->AddCircle(p, kIcon + 3.0f, outline, 0, selected ? 2.5f : 1.0f);
        if (!source.spatial)
            list->AddText(ImVec2(p.x + kIcon + 4.0f, p.y - kIcon), IM_COL32(200, 200, 200, 200), "2D");
        const float d = std::hypot(mouse.x - p.x, mouse.y - p.y);
        if (d < hitDistance) {
            hitDistance = d;
            hit         = e;
        }
        if (selected && source.spatial)
            for (const auto& [radius, color] : {std::pair{source.minDistance, IM_COL32(120, 190, 255, 200)},
                                                std::pair{source.maxDistance, IM_COL32(120, 190, 255, 90)}}) {
                projector.Circle(list, pos, {1, 0, 0}, {0, 1, 0}, radius, color);
                projector.Circle(list, pos, {1, 0, 0}, {0, 0, 1}, radius, color);
                projector.Circle(list, pos, {0, 1, 0}, {0, 0, 1}, radius, color);
            }
    });
    if (clicked && hit != NullEntity)
        SelectFromClick(hit, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
    list->PopClipRect();
    return clicked && hit != NullEntity;
}

void Editor::DrawBvhOverlay(float x, float y, float width, float height)
{
    const CameraData        camera = m_Ctx.camera.GetData(width / std::max(height, 1.0f));
    const ViewportProjector projector{camera.projection * camera.view, ImVec2(x, y), ImVec2(width, height)};
    ImDrawList*             list = ImGui::GetWindowDrawList();
    list->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);

    const auto box = [&](const Aabb& b, ImU32 color) {
        const glm::vec3 c[8] = {{b.min.x, b.min.y, b.min.z}, {b.max.x, b.min.y, b.min.z}, {b.max.x, b.max.y, b.min.z},
                                {b.min.x, b.max.y, b.min.z}, {b.min.x, b.min.y, b.max.z}, {b.max.x, b.min.y, b.max.z},
                                {b.max.x, b.max.y, b.max.z}, {b.min.x, b.max.y, b.max.z}};
        static constexpr int kEdges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                              {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& e : kEdges)
            projector.Line(list, c[e[0]], c[e[1]], color);
    };
    // Fat node boxes, colored by depth (leaves dimmer).
    m_Ctx.sceneRenderer.Spatial().MeshTree().ForEachNode([&](const Aabb& b, int depth, bool leaf) {
        if (depth > m_BvhDepth)
            return;
        const float hue = std::fmod(static_cast<float>(depth) * 0.13f, 1.0f);
        float       r, g, bl;
        ImGui::ColorConvertHSVtoRGB(hue, 0.7f, 1.0f, r, g, bl);
        box(b, ImGui::GetColorU32(ImVec4(r, g, bl, leaf ? 0.35f : 0.8f)));
    });
    if (const std::optional<Aabb> selection = SelectionBounds())
        box(*selection, IM_COL32(255, 220, 60, 255));
    list->PopClipRect();
}

void Editor::DrawColliderOverlay(float x, float y, float width, float height)
{
    const CameraData        camera = m_Ctx.camera.GetData(width / std::max(height, 1.0f));
    const ViewportProjector projector{camera.projection * camera.view, ImVec2(x, y), ImVec2(width, height)};
    ImDrawList*             list = ImGui::GetWindowDrawList();
    list->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + height), true);

    // Static gray, kinematic yellow, active green, sleeping blue, character cyan, trigger magenta.
    const auto color = [&](const ColliderDebugShape& s) {
        float alpha = IsSelected(s.entity) ? 1.0f : 0.6f;
        if (s.trigger)
            return ImGui::GetColorU32(ImVec4(1.0f, 0.3f, 1.0f, alpha));
        switch (s.activity) {
        case BodyActivity::Kinematic: return ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.2f, alpha));
        case BodyActivity::Active: return ImGui::GetColorU32(ImVec4(0.3f, 1.0f, 0.4f, alpha));
        case BodyActivity::Sleeping: return ImGui::GetColorU32(ImVec4(0.35f, 0.55f, 1.0f, alpha));
        case BodyActivity::Character: return ImGui::GetColorU32(ImVec4(0.2f, 0.9f, 1.0f, alpha));
        default: alpha *= 0.7f; return ImGui::GetColorU32(ImVec4(0.75f, 0.75f, 0.75f, alpha));
        }
    };
    const glm::vec3 eye = glm::inverse(camera.view)[3];
    m_Ctx.physics->ForEachCollider([&](const ColliderDebugShape& s) {
        const glm::mat4& m      = s.transform;
        const glm::vec3  center = m[3];
        const glm::vec3  ax     = glm::normalize(glm::vec3(m[0])), ay = glm::normalize(glm::vec3(m[1])),
                        az     = glm::normalize(glm::vec3(m[2]));
        const ImU32 c = color(s);
        switch (s.shape) {
        case ColliderShape::Box:
        case ColliderShape::Mesh: {
            glm::vec3 corners[8];
            for (int i = 0; i < 8; ++i)
                corners[i] = center + ax * ((i & 1) ? s.halfExtents.x : -s.halfExtents.x) +
                             ay * ((i & 2) ? s.halfExtents.y : -s.halfExtents.y) +
                             az * ((i & 4) ? s.halfExtents.z : -s.halfExtents.z);
            for (int i = 0; i < 8; ++i)
                for (int bit : {1, 2, 4})
                    if (!(i & bit))
                        projector.Line(list, corners[i], corners[i | bit], c);
            break;
        }
        case ColliderShape::Sphere: {
            projector.Circle(list, center, ax, ay, s.radius, c);
            projector.Circle(list, center, ax, az, s.radius, c);
            projector.Circle(list, center, ay, az, s.radius, c);
            // Silhouette: circle facing the camera.
            const glm::vec3 view = glm::normalize(center - eye);
            const glm::vec3 u    = glm::normalize(glm::cross(view, std::abs(view.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
            projector.Circle(list, center, u, glm::cross(view, u), s.radius, c);
            break;
        }
        case ColliderShape::Capsule: {
            const glm::vec3 top = center + ay * s.halfHeight, bottom = center - ay * s.halfHeight;
            projector.Circle(list, top, ax, az, s.radius, c);
            projector.Circle(list, bottom, ax, az, s.radius, c);
            for (const glm::vec3& side : {ax, -ax, az, -az})
                projector.Line(list, top + side * s.radius, bottom + side * s.radius, c);
            // Hemisphere arcs in the two vertical planes.
            constexpr int kArc = 12;
            for (const glm::vec3& side : {ax, az}) {
                glm::vec3 prevTop = top + side * s.radius, prevBottom = bottom + side * s.radius;
                for (int i = 1; i <= kArc; ++i) {
                    const float     t  = glm::pi<float>() * static_cast<float>(i) / kArc;
                    const glm::vec3 d  = side * std::cos(t);
                    const glm::vec3 nt = top + (d + ay * std::sin(t)) * s.radius;
                    const glm::vec3 nb = bottom + (d - ay * std::sin(t)) * s.radius;
                    projector.Line(list, prevTop, nt, c);
                    projector.Line(list, prevBottom, nb, c);
                    prevTop    = nt;
                    prevBottom = nb;
                }
            }
            break;
        }
        }
    });
    list->PopClipRect();
}

std::optional<Aabb> Editor::SelectionBounds() const
{
    const Registry&     registry = m_Ctx.scene.GetRegistry();
    const SpatialIndex& spatial  = m_Ctx.sceneRenderer.Spatial();
    std::optional<Aabb> bounds;
    std::vector<Entity> stack(m_Selection.begin(), m_Selection.end());
    while (!stack.empty()) {
        const Entity e = stack.back();
        stack.pop_back();
        std::optional<Aabb> b = registry.Has<MeshRenderer>(e) ? spatial.Bounds(e) : std::nullopt;
        if (!b && registry.Has<Light>(e)) { // a quarter of the range: the light and its closest surroundings
            const glm::vec3 p = registry.Get<WorldTransform>(e).matrix[3];
            const float     r = EffectiveRange(registry.Get<Light>(e)) * 0.25f;
            b                 = Aabb{p - glm::vec3(r), p + glm::vec3(r)};
        }
        if (b)
            bounds = bounds ? Aabb{glm::min(bounds->min, b->min), glm::max(bounds->max, b->max)} : *b;
        const auto& children = registry.Get<Hierarchy>(e).children;
        stack.insert(stack.end(), children.begin(), children.end());
    }
    return bounds;
}

glm::vec3 Editor::PlacementPoint(float distance) const
{
    const glm::vec3 origin  = m_Ctx.camera.position;
    const glm::vec3 forward = m_Ctx.camera.Forward();
    if (const auto hit = m_Ctx.sceneRenderer.Spatial().Raycast(origin, forward, distance * 4.0f))
        return origin + forward * hit->distance;
    return origin + forward * distance;
}

void Editor::HandleHotkeys()
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || m_Ctx.camera.IsCaptured() || ImGui::IsAnyItemActive() || m_FileDialog->IsOpen())
        return;
    if (m_Graphs->Focused()) { // the graph editor has its own undo / copy / delete keys
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
            if (m_PlayState == PlayState::Edit)
                Play();
            else
                Stop();
        }
        return;
    }

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
        if (ImGui::IsKeyPressed(ImGuiKey_P, false)) {
            if (m_PlayState == PlayState::Edit)
                Play();
            else
                Stop();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_B, false) && m_Ctx.project)
            BuildAndRun();
        if (ImGui::IsKeyPressed(ImGuiKey_N, false))
            RequestSceneChange([this] { NewScene(); });
        if (ImGui::IsKeyPressed(ImGuiKey_O, false))
            RequestSceneChange([this] {
                m_DialogPurpose = DialogPurpose::OpenScene;
                m_FileDialog->Open("Open scene", FileDialog::Mode::Open,
                                   m_ScenePath.empty() ? ContentRoot() : m_ScenePath.parent_path(),
                                   {".json"});
            });
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            if (m_ScenePath.empty() || io.KeyShift) {
                m_DialogPurpose = DialogPurpose::SaveScene;
                m_FileDialog->Open("Save scene", FileDialog::Mode::Save,
                                   m_ScenePath.empty() ? ContentRoot() : m_ScenePath.parent_path(),
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
    // Frame the selection's bounds (keeping the viewing direction); without meshes, the pivot.
    glm::vec3 target   = glm::vec3(m_Ctx.scene.GetRegistry().Get<WorldTransform>(primary).matrix[3]);
    float     distance = std::clamp(glm::length(target - m_Ctx.camera.position), 1.0f, 10.0f);
    if (const std::optional<Aabb> bounds = SelectionBounds()) {
        target              = (bounds->min + bounds->max) * 0.5f;
        const float radius  = std::max(glm::length(bounds->max - bounds->min) * 0.5f, 1e-3f);
        const float halfFov = m_Ctx.camera.fovY * 0.5f;
        distance            = std::max(radius / std::sin(halfFov) * 1.1f, m_Ctx.camera.nearPlane * 10.0f);
    }
    m_Ctx.camera.position = target - m_Ctx.camera.Forward() * distance;
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

void Editor::SelectInRect(glm::vec2 min, glm::vec2 max, bool additive)
{
    if (!m_ViewportImage)
        return;
    const VkExtent2D extent = m_ViewportImage.Extent2D();
    const glm::vec2  size(static_cast<float>(extent.width), static_cast<float>(extent.height));
    const CameraData camera   = m_Ctx.camera.GetData(size.x / std::max(size.y, 1.0f));
    const glm::mat4  viewProj = camera.projection * camera.view;
    const auto inside = [&](const glm::vec3& p) {
        const glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
        if (clip.w <= 1e-4f)
            return false;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const glm::vec2 px((ndc.x * 0.5f + 0.5f) * size.x, (0.5f - ndc.y * 0.5f) * size.y);
        return glm::all(glm::greaterThanEqual(px, min)) && glm::all(glm::lessThanEqual(px, max));
    };

    Registry&           registry = m_Ctx.scene.GetRegistry();
    const SpatialIndex& spatial  = m_Ctx.sceneRenderer.Spatial();
    std::vector<Entity> hits;
    registry.ViewOf<MeshRenderer>().Each([&](Entity e, MeshRenderer&) {
        if (const std::optional<Aabb> b = spatial.Bounds(e); b && inside((b->min + b->max) * 0.5f))
            hits.push_back(e);
    });
    if (m_ShowLightIcons)
        registry.ViewOf<Light, WorldTransform>().Each([&](Entity e, Light&, WorldTransform& world) {
            if (inside(glm::vec3(world.matrix[3])) && std::ranges::find(hits, e) == hits.end())
                hits.push_back(e);
        });
    if (m_ShowAudio)
        registry.ViewOf<AudioSource, WorldTransform>().Each([&](Entity e, AudioSource&, WorldTransform& world) {
            if (inside(glm::vec3(world.matrix[3])) && std::ranges::find(hits, e) == hits.end())
                hits.push_back(e);
        });
    if (!additive)
        m_Selection.clear();
    for (Entity e : hits)
        if (!IsSelected(e))
            m_Selection.push_back(e);
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
