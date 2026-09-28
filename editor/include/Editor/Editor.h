#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/Image.h"
#include "Engine/Scene/Frustum.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

class AssetManager;
class FileDialog;
class FlyCamera;
class History;
class ImGuiLayer;
class Scene;
class SceneRenderer;
class Window;
enum class LightType : std::uint8_t;
enum class PrimitiveShape : std::uint8_t;
struct EditCommand;

struct EditorContext {
    Window&        window;
    Renderer&      renderer;
    Scene&         scene;
    AssetManager&  assets;
    SceneRenderer& sceneRenderer;
    FlyCamera&     camera;
    // Models the editor acquired (scene files, asset browser, primitives). Owned by the
    // application so they outlive the editor (F1 toggle); released on New/Open scene.
    std::vector<ModelHandle>& modelRefs;
};

// ImGui editor: dockspace with Viewport (scene rendered into a texture, GPU picking, outline,
// transform gizmo), Hierarchy, Inspector, Renderer settings, Stats and Assets; undo/redo of
// scene edits, multi-selection, duplicate, scene files (JSON).
// Main thread only; the ImGui dependency stays inside the Editor library.
class Editor {
public:
    explicit Editor(const EditorContext& context);
    ~Editor(); // GPU resources go through the deferred queue; model refs stay with the application

    Editor(const Editor&)            = delete;
    Editor& operator=(const Editor&) = delete;

    // Once per frame, before rendering: ImGui frame, panels, gizmo, hotkeys. dt in seconds.
    void Update(float dt);
    // Scene into the viewport texture, then the UI into the swapchain image.
    void Render(const FrameContext& frame);

    // Scene camera input should only be processed while this is true.
    [[nodiscard]] bool ViewportHovered() const { return m_ViewportHovered; }
    [[nodiscard]] bool WantsKeyboard() const;   // a text field or widget is active: skip game hotkeys
    [[nodiscard]] float ViewportAspect() const; // width / height of the viewport panel

    // Selection: the last entry is the primary one (inspector, gizmo pivot).
    [[nodiscard]] Entity                  Selected() const; // primary, NullEntity if none
    [[nodiscard]] std::span<const Entity> Selection() const { return m_Selection; }
    void                                  Select(Entity entity); // single selection; NullEntity clears
    void                                  ToggleSelection(Entity entity);

    // Edit operations (menu / hotkeys), all undoable.
    void DuplicateSelection(); // Ctrl+D
    void DeleteSelection();    // Del
    bool Undo();               // Ctrl+Z
    bool Redo();               // Ctrl+Y, Ctrl+Shift+Z
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    [[nodiscard]] bool HasUnsavedChanges() const;

    // Scene files. New/Open replace the scene and release the editor's model refs.
    void NewScene();
    bool OpenScene(const std::filesystem::path& file); // false: error (logged, scene unchanged)
    bool SaveScene(const std::filesystem::path& file);
    [[nodiscard]] const std::filesystem::path& ScenePath() const { return m_ScenePath; }

private:
    enum class GizmoOperation { Translate, Rotate, Scale };
    struct StateEdit {
        std::uint64_t uuid = 0;
        std::string   before;
    };

    // Layout and panels
    void BuildDefaultLayout(unsigned int dockspace);
    void DrawMenuBar();
    void DrawViewport();
    void DrawGizmo(float x, float y, float width, float height);
    // Light icons (click selects), range sphere / spot cone of the selected light. True: an icon was hit.
    bool DrawLightOverlay(float x, float y, float width, float height, bool clicked);
    void DrawBvhOverlay(float x, float y, float width, float height);
    // World bounds of the selection (meshes in the selected subtrees); nullopt if there are none.
    [[nodiscard]] std::optional<Aabb> SelectionBounds() const;
    // Where new objects go: the surface under the viewport center, else in front of the camera.
    [[nodiscard]] glm::vec3 PlacementPoint(float distance) const;
    void DrawHierarchy();
    void DrawHierarchyNode(Entity entity);
    void DrawInspector();
    void DrawRendererSettings();
    void DrawStats();
    void DrawAssets();
    void DrawDialogs();
    void HandleHotkeys();
    void EnsureViewportTarget(std::uint32_t width, std::uint32_t height);
    void FocusSelected();
    void ApplyPendingEdits(); // deletes / reparents requested while the hierarchy was drawn
    void UpdateSelectionOverlay();

    // Selection helpers
    [[nodiscard]] bool                IsSelected(Entity entity) const;
    [[nodiscard]] std::vector<Entity> SelectionRoots() const; // selected, without selected ancestors
    void                              SelectFromClick(Entity entity, bool additive);
    void                              ValidateSelection();

    // Commands
    void   PushCommand(EditCommand command);
    void   PushStateChange(std::string label, std::vector<StateEdit> before); // after = current states
    void   PushCreated(std::string label, std::span<const Entity> roots);     // already created
    Entity CreateEntity(std::string name, Entity parent);                     // + undo
    Entity CreateLight(LightType type, Entity parent);                        // + undo
    Entity CreatePrimitiveEntity(PrimitiveShape shape);                       // + undo
    void   Reparent(Entity child, Entity parent);                             // keeps the world transform, + undo
    void   DestroyByUuids(std::span<const std::uint64_t> uuids);
    [[nodiscard]] std::uint64_t UuidOf(Entity entity) const;

    // Scene files
    void RequestSceneChange(std::function<void()> action); // asks first when there are unsaved changes
    void ReleaseModelRefs();

    EditorContext               m_Ctx;
    std::unique_ptr<ImGuiLayer> m_ImGui;
    std::unique_ptr<History>    m_History;
    std::unique_ptr<FileDialog> m_FileDialog;

    // Viewport render target (RGBA8 sRGB, sampled by ImGui).
    Image         m_ViewportImage;
    std::uint64_t m_ViewportTexture = 0; // ImTextureID
    bool          m_ViewportHovered = false;
    bool          m_ViewportFocused = false;
    bool          m_PickAdditive    = false; // a pick request is pending: Ctrl was held

    std::vector<Entity> m_Selection;
    std::vector<Entity> m_PendingDelete;              // hierarchy context menu
    bool                m_PendingDuplicate = false;   // hierarchy context menu
    Entity              m_ReparentChild = NullEntity; // drag & drop in the hierarchy
    Entity              m_ReparentTo    = NullEntity; // NullEntity: make it a root
    bool                m_ReparentPending = false;
    GizmoOperation      m_GizmoOperation  = GizmoOperation::Translate;
    bool                m_GizmoLocal      = true;
    bool                m_ShowLightIcons  = true;
    bool                m_ShowBvh         = false; // BVH nodes + selection bounds in the viewport
    int                 m_BvhDepth        = 8;

    // Edits in progress: gizmo drag, inspector widget (one undo step each when they end).
    std::optional<std::vector<StateEdit>> m_GizmoEdit;
    std::optional<StateEdit>              m_InspectorEdit;
    std::string                           m_InspectorFrameState; // primary entity at the start of the inspector

    // Euler angles shown in the inspector: kept while the rotation is only edited through them, so
    // dragging past +-90 degrees does not flip (quaternion -> Euler is not unique).
    Entity    m_EulerEntity = NullEntity;
    glm::quat m_EulerSource{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 m_EulerDegrees{0.0f};

    // Panels
    bool m_ShowHierarchy = true, m_ShowInspector = true, m_ShowRenderer = true, m_ShowStats = true;
    bool m_ShowAssets = true, m_ShowDemo = false;

    // Frame time history (ms) for the diagnostics graph.
    static constexpr std::size_t kHistory = 240;
    std::array<float, kHistory> m_FrameTimes{};
    std::size_t                 m_FrameTimeHead = 0;

    // Scene file + dialogs
    std::filesystem::path       m_ScenePath;
    std::string                 m_Status; // last file operation, shown in the menu bar
    std::function<void()>       m_PendingSceneChange; // waiting for "discard changes?"
    bool                        m_ConfirmDiscard = false;
    enum class DialogPurpose { None, OpenScene, SaveScene, LoadModel } m_DialogPurpose = DialogPurpose::None;
    std::string                 m_LoadPath = "assets/models/BoxTextured.glb";
};

} // namespace Engine
