#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/Image.h"
#include "Engine/Scene/Frustum.h"
#include "Engine/Script/ScriptGraph.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Engine {

class AssetManager;
class AudioSystem;
class FileDialog;
class FlyCamera;
class History;
class ImGuiLayer;
class PhysicsWorld;
class Project;
class Scene;
class SceneRenderer;
class ScriptGraphEditor;
class ScriptSystem;
class Window;
struct ScriptComponent;
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
    // Optional: Play mode, collider overlay, physics inspector. The editor steps it only while playing.
    PhysicsWorld* physics = nullptr;
    // Optional: visual scripts run while playing (Begin on Play, Update per frame, End on Stop).
    ScriptSystem* scripts = nullptr;
    // Optional: scene audio while playing (Begin on Play, End on Stop), previews, audio settings.
    AudioSystem* audio = nullptr;
    // Optional: the open project (Content browser root, project settings, Build & Run, packaging).
    Project* project = nullptr;
    // ImGui layout file (empty: not saved).
    std::filesystem::path layoutFile = "editor.ini";
};

enum class PlayState : std::uint8_t { Edit, Playing, Paused };

// ImGui editor: dockspace with Viewport (scene rendered into a texture, GPU picking, outline,
// transform gizmo), Hierarchy, Inspector, Renderer settings, Stats and Assets; undo/redo of
// scene edits, multi-selection, duplicate, scene files (JSON), physics Play / Pause / Stop.
// Main thread only; the ImGui dependency stays inside the Editor library.
class Editor {
public:
    explicit Editor(const EditorContext& context);
    ~Editor(); // GPU resources go through the deferred queue; model refs stay with the application

    Editor(const Editor&)            = delete;
    Editor& operator=(const Editor&) = delete;

    // Once per frame, before rendering: ImGui frame, panels, gizmo, hotkeys. dt in seconds.
    void Update(float dt);
    // Fixed-rate tick: steps the physics while playing (or once after StepOnce). In edit mode the
    // bodies only follow the scene (Sync in Update).
    void FixedUpdate(float dt);
    // Scene into the viewport texture, then the UI into the swapchain image. physicsAlpha: time
    // since the last fixed step / fixed step (PhysicsWorld::Interpolate while playing).
    void Render(const FrameContext& frame, float physicsAlpha = 1.0f);

    // Scene camera input should only be processed while this is true.
    [[nodiscard]] bool ViewportHovered() const { return m_ViewportHovered; }
    [[nodiscard]] bool WantsKeyboard() const;   // a text field or widget is active: skip game hotkeys
    [[nodiscard]] float ViewportAspect() const; // width / height of the viewport panel

    // Selection: the last entry is the primary one (inspector, gizmo pivot).
    [[nodiscard]] Entity                  Selected() const; // primary, NullEntity if none
    [[nodiscard]] std::span<const Entity> Selection() const { return m_Selection; }
    void                                  Select(Entity entity); // single selection; NullEntity clears
    void                                  ToggleSelection(Entity entity);
    // Box selection (viewport drag): meshes whose world-bounds center and lights whose position
    // project into the rectangle (viewport pixels). additive: added to the selection.
    void SelectInRect(glm::vec2 min, glm::vec2 max, bool additive);

    // Edit operations (menu / hotkeys), all undoable.
    void DuplicateSelection(); // Ctrl+D
    void DeleteSelection();    // Del
    bool Undo();               // Ctrl+Z
    bool Redo();               // Ctrl+Y, Ctrl+Shift+Z
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    [[nodiscard]] bool HasUnsavedChanges() const;

    // Play mode: Play snapshots the scene and starts the simulation, Stop restores the snapshot
    // (selection kept by UUID). No undo history while playing; scene files are locked.
    [[nodiscard]] PlayState GetPlayState() const { return m_PlayState; }
    void                    Play();     // from Edit: snapshot + simulate; from Paused: resume
    void                    Pause();
    void                    Stop();     // back to the snapshot
    // Project: (re)loads the blueprint types / interfaces / libraries of the content directory
    // (ScriptRegistry); done at start and before play. Returns the problems found.
    std::vector<std::string> ReloadScriptRegistry();
    // Edit mode: runs the construction scripts again (after edits / Undo / Open automatically).
    void RunConstructionScripts();
    // While playing: replaces the play scene by a scene file (scripts' Open Level); Stop returns
    // to the edited scene. False if the file does not load (the current level stays).
    bool PlayLevel(const std::filesystem::path& scene);
    void                    StepOnce(); // paused: one fixed step

    // Blueprint (visual script) editor window.
    [[nodiscard]] ScriptGraphEditor& Blueprints() { return *m_Graphs; }

    // Project: Content browser root (the project's Content/, else the working directory), Build &
    // Run (saves, starts UnginePlayer with the project), packaging into a standalone game folder.
    [[nodiscard]] std::filesystem::path ContentRoot() const;
    bool BuildAndRun();
    bool PackageProject(const std::filesystem::path& outputDirectory);
    bool SaveAll(); // scene (if it has a file) + open blueprints
    // Window close request: true if the application may quit now; with unsaved changes it asks
    // (Save all / Discard / Cancel) and closes the window itself once decided.
    bool ConfirmQuit();
    // Content browser actions (also double-click / drag & drop).
    void OpenAsset(const std::filesystem::path& file); // scene, blueprint or model (instantiated)

    // Scene files. New/Open replace the scene and release the editor's model refs.
    void NewScene();
    bool OpenScene(const std::filesystem::path& file); // false: error (logged, scene unchanged)
    bool SaveScene(const std::filesystem::path& file);
    [[nodiscard]] const std::filesystem::path& ScenePath() const { return m_ScenePath; }

    // Prefabs (undoable; writing a prefab file is not undone). Errors go to the status line.
    bool   CreatePrefabFrom(Entity root, const std::filesystem::path& file); // the subtree becomes an instance
    Entity PlacePrefab(const std::filesystem::path& file, const glm::vec3& position);
    enum class PrefabOp { Apply, Revert, Unlink };
    // Apply / Unlink: the instance of `entity`; Revert: `key` of `entity` (empty: all of it, or the
    // whole instance for its root).
    bool RunPrefabOp(PrefabOp op, Entity entity, const std::string& key = {});

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
    void DrawColliderOverlay(float x, float y, float width, float height);
    // Local bounds of the entity's MeshRenderer mesh (collider fitting); nullopt without a ready mesh.
    [[nodiscard]] std::optional<std::pair<glm::vec3, glm::vec3>> MeshBounds(Entity entity) const;
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
    void DrawModelAssets(ModelHandle& toRelease);
    void DrawTextureAssets();
    void DrawSoundAssets();
    void DrawAudioSettings(); // mixer + occlusion (saved with the project)
    // Audio source icons (click selects), distance spheres of selected sources, reverb zone boxes.
    bool DrawAudioOverlay(float x, float y, float width, float height, bool clicked);
    [[nodiscard]] std::uint64_t TexturePreview(TextureHandle handle); // ImTextureID, 0: none
    void ReleaseTexturePreviews(bool all); // all: shutdown; else those not shown this frame / outdated
    void DrawDialogs();
    void DrawPrefabHeader(Entity entity);         // inspector: prefab of the entity, overrides, actions
    void DrawScriptVariables(ScriptComponent& script); // inspector: exposed variables of the graph
    // The graph of a script file: the open editor document (unsaved edits) or the file (cached by mtime).
    [[nodiscard]] const ScriptGraph* GraphFor(const std::string& file);
    // Instance roots `uuids` (whole subtrees) were rebuilt: undo restores `before`.
    void PushSubtreesChange(std::string label, const std::vector<std::uint64_t>& uuids, std::string before);
    [[nodiscard]] std::vector<Entity> OutermostRoots(const std::vector<std::uint64_t>& uuids) const;
    void DrawContentBrowser();
    void DrawProjectSettings();
    void DrawBlueprintTypes();
    void OpenTypeFile(const std::filesystem::path& file);
    void RefreshContent();
    void UpdatePendingInstances(); // models opened from the content browser: instantiate when ready
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
    void   AssignScript(Entity entity, const std::filesystem::path& graph);  // Script component, + undo
    void   AssignSound(Entity entity, const std::filesystem::path& sound);   // Audio Source (added if missing), + undo
    Entity CreateAudioEntity(const std::filesystem::path& sound, const glm::vec3& position); // + undo
    void   DestroyByUuids(std::span<const std::uint64_t> uuids);
    [[nodiscard]] std::uint64_t UuidOf(Entity entity) const;

    // Scene files
    void RequestSceneChange(std::function<void()> action); // asks first when there are unsaved changes
    void ReleaseModelRefs();

    EditorContext               m_Ctx;
    std::unique_ptr<ImGuiLayer> m_ImGui;
    std::unique_ptr<History>    m_History;
    std::unique_ptr<FileDialog> m_FileDialog;
    std::unique_ptr<ScriptGraphEditor> m_Graphs;

    // Viewport render target (RGBA8 sRGB, sampled by ImGui).
    Image         m_ViewportImage;
    std::uint64_t m_ViewportTexture = 0; // ImTextureID
    struct Preview {
        std::uint64_t texture  = 0; // ImTextureID
        std::uint32_t revision = 0; // texture content it shows
        bool          used     = false; // drawn this frame
    };
    std::unordered_map<TextureHandle, Preview> m_TexturePreviews; // Assets panel thumbnails
    bool          m_ViewportHovered = false;
    bool          m_ViewportVisible = false; // drawn this frame (else the scene render is skipped)
    int           m_FocusViewport   = 0; // frames until the viewport tab is brought to front (default layout)
    bool          m_ViewportFocused = false;
    bool          m_PickAdditive    = false; // a pick request is pending: Ctrl was held
    // Left button pressed in the viewport: click (pick on release) or box selection (dragged).
    bool      m_ViewportPress = false;
    bool      m_BoxSelecting  = false;
    glm::vec2 m_PressPos{0.0f}; // viewport pixels

    std::vector<Entity> m_Selection;
    std::vector<Entity> m_PendingDelete;              // hierarchy context menu
    struct PendingPrefabOp {
        PrefabOp      op;
        std::uint64_t uuid = 0;
        std::string   key;
    };
    std::optional<PendingPrefabOp> m_PendingPrefabOp; // after the panels (they read the entities)
    double                         m_PrefabPollTime = 0.0; // last RefreshPrefabInstances (ImGui time)
    std::string                    m_NewTag;               // inspector: tag being typed
    std::map<std::string, std::pair<std::filesystem::file_time_type, ScriptGraph>> m_GraphCache;
    bool m_DebugPauseAudio = false; // audio paused because the script debugger stopped
    bool                m_PendingDuplicate = false;   // hierarchy context menu
    Entity              m_ReparentChild = NullEntity; // drag & drop in the hierarchy
    Entity              m_ReparentTo    = NullEntity; // NullEntity: make it a root
    bool                m_ReparentPending = false;
    GizmoOperation      m_GizmoOperation  = GizmoOperation::Translate;
    bool                m_GizmoLocal      = true;
    bool                m_ShowLightIcons  = true;
    bool                m_ShowBvh         = false; // BVH nodes + selection bounds in the viewport
    int                 m_BvhDepth        = 8;
    bool                m_ShowColliders   = true;
    bool                m_ShowAudio       = true; // audio source icons / ranges, reverb zones
    bool                m_GameCamera      = false; // viewport renders through the scene's primary camera

    // Play mode
    PlayState   m_PlayState = PlayState::Edit;
    std::string m_PlaySnapshot; // whole scene before Play
    std::vector<ModelHandle> m_PlayModels; // of levels opened by scripts while playing (released at Stop)
    std::uint64_t            m_ConstructedRevision = ~std::uint64_t{0}; // History revision construction scripts ran for
    std::uint64_t            m_ConstructedGraphs   = 0; // graph editor revision they ran for
    bool        m_StepRequested = false;

    // Edits in progress: gizmo drag, inspector widget (one undo step each when they end).
    std::optional<std::vector<StateEdit>> m_GizmoEdit;
    std::optional<std::vector<StateEdit>> m_InspectorEdit; // primary first, then the multi-edited others
    std::string                           m_InspectorFrameState; // primary entity at the start of the inspector

    // Euler angles shown in the inspector: kept while the rotation is only edited through them, so
    // dragging past +-90 degrees does not flip (quaternion -> Euler is not unique).
    Entity    m_EulerEntity = NullEntity;
    glm::quat m_EulerSource{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 m_EulerDegrees{0.0f};

    // Panels
    bool m_ShowHierarchy = true, m_ShowInspector = true, m_ShowRenderer = true, m_ShowStats = true;
    bool m_ShowAssets = true, m_ShowDemo = false, m_ShowBlueprint = true, m_ShowContent = true;
    bool m_ShowProjectSettings = false;
    bool m_ShowTypes           = false; // Blueprint Types window
    struct TypeEdit;                    // the enum / struct / interface edited there
    std::shared_ptr<TypeEdit> m_TypeEdit;
    bool m_ProjectDirty = false; // project settings changed outside the Project Settings window (audio)
    bool m_AskQuit = false, m_QuitConfirmed = false;

    // Content browser
    struct ContentItem {
        std::filesystem::path path;
        std::string           label;
        enum class Kind { Folder, Scene, Blueprint, Prefab, Model, Texture, Sound, Type, Other } kind = Kind::Other;
    };
    std::filesystem::path                             m_ContentDir;     // shown directory
    std::vector<ContentItem>                          m_ContentItems;
    double                                            m_ContentScanTime = -1.0; // ImGui time of the last scan
    std::string                                       m_ContentFilter;
    std::filesystem::path                             m_ContentSelected;
    std::filesystem::path                             m_RenameTarget, m_DeleteTarget;
    std::string                                       m_RenameText;
    std::vector<std::pair<ModelHandle, glm::vec3>>    m_PendingInstances; // loading, then placed

    // Frame time history (ms) for the diagnostics graph.
    static constexpr std::size_t kHistory = 240;
    std::array<float, kHistory> m_FrameTimes{};
    std::size_t                 m_FrameTimeHead = 0;

    // Scene file + dialogs
    std::filesystem::path       m_ScenePath;
    std::string                 m_Status; // last file operation, shown in the menu bar
    std::function<void()>       m_PendingSceneChange; // waiting for "discard changes?"
    bool                        m_ConfirmDiscard = false;
    enum class DialogPurpose { None, OpenScene, SaveScene, LoadModel, NewScript, AssignScript, AssignSound, Package, CreatePrefab } m_DialogPurpose = DialogPurpose::None;
    Entity                      m_PrefabTarget = NullEntity; // subtree the "Create prefab" dialog saves
    Entity                      m_ScriptTarget = NullEntity; // entity whose Script component the dialog fills
    Entity                      m_SoundTarget  = NullEntity; // entity whose Audio Source the dialog fills
    std::string                 m_LoadPath = "assets/models/BoxTextured.glb";
};

} // namespace Engine
