#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/Image.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

class AssetManager;
class FlyCamera;
class ImGuiLayer;
class Scene;
class SceneRenderer;
class Window;

struct EditorContext {
    Window&        window;
    Renderer&      renderer;
    Scene&         scene;
    AssetManager&  assets;
    SceneRenderer& sceneRenderer;
    FlyCamera&     camera;
};

// ImGui editor: dockspace with Viewport (scene rendered into a texture, transform gizmo),
// Hierarchy, Inspector, Renderer settings, Frame diagnostics (CPU/GPU timings, VRAM) and Assets.
// Main thread only; the ImGui dependency stays inside the Editor library.
class Editor {
public:
    explicit Editor(const EditorContext& context);
    ~Editor(); // releases the models it loaded; GPU resources go through the deferred queue

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

    [[nodiscard]] Entity Selected() const { return m_Selected; }
    void                 Select(Entity entity) { m_Selected = entity; }

private:
    enum class GizmoOperation { Translate, Rotate, Scale };

    void BuildDefaultLayout(unsigned int dockspace);
    void DrawMenuBar();
    void DrawViewport();
    void DrawGizmo(float x, float y, float width, float height);
    void DrawHierarchy();
    void DrawHierarchyNode(Entity entity);
    void DrawInspector();
    void DrawRendererSettings();
    void DrawStats();
    void DrawAssets();
    void HandleHotkeys();
    void EnsureViewportTarget(std::uint32_t width, std::uint32_t height);
    void DeleteSelected();
    void FocusSelected();
    void ApplyPendingEdits(); // deletes / reparents requested while the hierarchy was drawn

    EditorContext               m_Ctx;
    std::unique_ptr<ImGuiLayer> m_ImGui;

    // Viewport render target (RGBA8 sRGB, sampled by ImGui).
    Image         m_ViewportImage;
    std::uint64_t m_ViewportTexture = 0; // ImTextureID
    bool          m_ViewportHovered = false;
    bool          m_ViewportFocused = false;

    Entity         m_Selected = NullEntity;
    Entity         m_PendingDelete = NullEntity;
    Entity         m_ReparentChild = NullEntity; // drag & drop in the hierarchy
    Entity         m_ReparentTo    = NullEntity; // NullEntity: make it a root
    GizmoOperation m_GizmoOperation = GizmoOperation::Translate;
    bool           m_GizmoLocal     = true;

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

    // Assets loaded through the editor (released in the destructor).
    std::vector<ModelHandle> m_OwnedModels;
    std::string              m_LoadPath = "assets/models/BoxTextured.glb";
};

} // namespace Engine
