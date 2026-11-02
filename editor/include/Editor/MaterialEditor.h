#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Material/MaterialGraph.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/Image.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Engine {

class AssetManager;
class FileDialog;
class ImGuiLayer;
class Scene;
class SceneRenderer;

// The material window: a node graph (.umat: canvas, parameters, settings, node details,
// problems) or an instance (.uminst: parent + parameter overrides), with a live preview (own scene
// + renderer, sphere / box / plane, orbit camera). Edits reach the preview and the scene at once
// (AssetManager::ProvideMaterial*, unsaved) and are undoable; Save writes the file. Main thread.
class MaterialEditor {
public:
    struct Context {
        Renderer&     renderer;
        AssetManager& assets;
        ImGuiLayer&   imgui;
    };
    explicit MaterialEditor(const Context& context);
    ~MaterialEditor();

    MaterialEditor(const MaterialEditor&)            = delete;
    MaterialEditor& operator=(const MaterialEditor&) = delete;

    // Opens a .umat or .uminst (a missing file starts as the default material / an instance
    // without parent). Asks nothing: the caller saves first.
    void Open(const std::filesystem::path& file);
    void Close(); // drops unsaved edits (the scene shows the file again)
    [[nodiscard]] bool IsOpen() const { return m_Open; }
    [[nodiscard]] bool IsInstance() const { return m_IsInstance; }
    [[nodiscard]] const std::filesystem::path& File() const { return m_File; }
    [[nodiscard]] bool Dirty() const;
    bool Save(std::string* error = nullptr);
    void Revert();

    void Draw(float dt);                    // the window (ImGui frame)
    void Render(const FrameContext& frame); // the preview image, before the UI pass

    // Automation (tests): the edited content; call Commit() after changing it.
    [[nodiscard]] MaterialGraph&        Graph() { return m_Graph; }
    [[nodiscard]] MaterialInstanceFile& Instance() { return m_Instance; }
    void Commit(); // one undo step, live update
    bool Undo();
    bool Redo();
    [[nodiscard]] const std::vector<MaterialProblem>& Problems() const { return m_Problems; }
    [[nodiscard]] std::set<std::uint32_t>& Selection() { return m_Selected; }
    // Canvas screen rectangle (tests: mouse automation) and a node's position on screen.
    [[nodiscard]] glm::vec4 CanvasRect() const { return m_CanvasRect; }
    [[nodiscard]] glm::vec2 NodeScreenPosition(std::uint32_t node) const;
    [[nodiscard]] std::uint32_t PreviewFrames() const { return m_PreviewFrames; }

private:
    void DrawToolbar();
    void DrawSidebar();
    void DrawSettings();
    void DrawParameters();
    void DrawDetails();
    void DrawProblems();
    void DrawInstance();
    void DrawCanvas();
    void DrawPalette();
    void DrawPreview();
    bool TextureField(const char* label, std::string& path, TextureKind* kind);
    void Apply(); // live: AssetManager (preview + scene), problems
    void EnsureTarget(std::uint32_t width, std::uint32_t height);
    void ReleaseTarget();
    void SetPreviewShape(int shape);
    [[nodiscard]] std::optional<MaterialGraph> ParentGraph() const; // instance: the graph at the end of the chain

    struct State {
        MaterialGraph        graph;
        MaterialInstanceFile instance;
        bool operator==(const State&) const = default;
    };
    [[nodiscard]] State Current() const { return {m_Graph, m_Instance}; }
    void Restore(const State& state);

    Context                     m_Ctx;
    bool                        m_Open       = false;
    bool                        m_IsInstance = false;
    bool                        m_Exists     = false; // the file existed when opened / was saved
    std::filesystem::path       m_File;
    MaterialGraph               m_Graph;
    MaterialInstanceFile        m_Instance;
    State                       m_Committed, m_Saved;
    std::optional<State>        m_Applied;
    std::vector<State>          m_Undo, m_Redo;
    std::vector<MaterialProblem> m_Problems;
    std::unique_ptr<FileDialog> m_Dialog;
    int                         m_DialogPurpose = 0; // 1: texture of m_DialogTarget, 2: instance parent
    std::string*                m_DialogTarget  = nullptr;
    std::string                 m_Status;
    mutable std::optional<MaterialGraph> m_ParentCache; // instance: resolved parent graph (refreshed on parent change)
    mutable std::string                  m_ParentCacheKey;

    // Canvas
    glm::vec2               m_Pan{0.0f};
    float                   m_Zoom = 1.0f;
    std::set<std::uint32_t> m_Selected;
    bool                    m_DraggingNodes = false;
    struct LinkDrag {
        std::uint32_t node   = 0;
        std::string   pin;
        bool          output = true;
    };
    std::optional<LinkDrag> m_LinkDrag;
    std::optional<LinkDrag> m_PendingLink; // palette opened by dropping a link on the canvas
    glm::vec2               m_PaletteAt{0.0f}; // graph position for nodes created from the palette
    char                    m_Search[64] = {};
    glm::vec4               m_CanvasRect{0.0f}; // x, y, w, h (screen)
    bool                    m_FrameAll = true;

    // Preview: its own scene + renderer.
    std::unique_ptr<Scene>         m_Scene;
    std::unique_ptr<SceneRenderer> m_Renderer;
    ModelHandle                    m_Shape;
    ModelHandle                    m_Ground;
    int                            m_ShapeKind  = 0; // 0 sphere, 1 box, 2 plane
    bool                           m_ShowGround = true;
    float                          m_Yaw = 0.6f, m_Pitch = 0.3f, m_Distance = 3.2f;
    Image                          m_Image;
    std::uint64_t                  m_Texture = 0; // ImTextureID
    bool                           m_PreviewVisible = false;
    std::uint32_t                  m_PreviewFrames  = 0;
};

} // namespace Engine
