#pragma once
#include "Engine/Animation/AnimGraph.h"
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/Image.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

class AnimationSystem;
class AssetManager;
class FileDialog;
class ImGuiLayer;
class Scene;
class SceneRenderer;
struct Model;

// The animation graph window (.uanim): parameters, layers (mode, weight, bone mask), the selected
// layer's state machine on a canvas (states, transitions, entry, Any State), details of the
// selected state (clip / 1D / 2D blend space with samples and a plot, notifies) or transition
// (duration, exit time, conditions), IK goals, problems, and a live preview of the preview model
// (own scene + renderer + AnimationSystem; parameter values and IK targets set from the window).
// Edits apply live to the preview and the scene's AnimationSystem (unsaved, via Provide) and are
// undoable; Save writes the file. Main thread.
class AnimGraphEditor {
public:
    struct Context {
        Renderer&        renderer;
        AssetManager&    assets;
        ImGuiLayer&      imgui;
        AnimationSystem* sceneAnimation = nullptr; // the edited scene's graphs (live edits)
    };
    explicit AnimGraphEditor(const Context& context);
    ~AnimGraphEditor();

    AnimGraphEditor(const AnimGraphEditor&)            = delete;
    AnimGraphEditor& operator=(const AnimGraphEditor&) = delete;

    // Opens a file (a missing one starts as the default graph). Asks nothing: the caller saves first.
    void Open(const std::filesystem::path& file);
    void Close();
    [[nodiscard]] bool IsOpen() const { return m_Open; }
    [[nodiscard]] const std::filesystem::path& File() const { return m_File; }
    [[nodiscard]] bool Dirty() const;
    bool Save(std::string* error = nullptr);
    void Revert();

    void Draw(float dt);
    void Render(const FrameContext& frame);

    // Automation (tests).
    [[nodiscard]] AnimGraph& Graph() { return m_Graph; }
    void Commit();
    bool Undo();
    bool Redo();
    void SelectLayer(std::size_t layer) { m_Layer = layer; }
    void SelectState(int state) { m_State = state, m_Transition = -1; }
    void SelectTransition(int transition) { m_Transition = transition, m_State = -1; }
    void SetPreviewParameter(const std::string& name, float value) { m_PreviewValues[name] = value; }
    [[nodiscard]] const std::vector<std::string>& Problems() const { return m_Problems; }
    [[nodiscard]] AnimationSystem& Preview() { return *m_Preview; }
    [[nodiscard]] Entity PreviewEntity() const { return m_PreviewRoot; }
    [[nodiscard]] const Model* PreviewModel() const;

private:
    void DrawToolbar();
    void DrawSidebar();
    void DrawParameters();
    void DrawLayers();
    void DrawIk();
    void DrawCanvas();
    void DrawDetails();
    void DrawState(AnimState& state);
    void DrawBlendSpace(AnimState& state);
    void DrawTransition(AnimTransition& transition);
    void DrawGraphSettings();
    void DrawPreview(float dt);
    bool PathField(const char* label, std::string& path, const std::vector<std::string>& extensions, int purpose,
                   std::size_t index = 0);
    bool ClipCombo(const char* label, std::string& clip);
    bool BoneCombo(const char* label, std::string& bone);
    bool ParameterCombo(const char* label, std::string& parameter, bool numericOnly);
    void RenameState(AnimLayer& layer, const std::string& from, const std::string& to);
    void Apply();
    void LoadPreviewModel();
    void EnsureTarget(std::uint32_t width, std::uint32_t height);
    void ReleaseTarget();

    Context                   m_Ctx;
    bool                      m_Open = false;
    std::filesystem::path     m_File;
    AnimGraph                 m_Graph, m_Committed, m_Saved;
    std::optional<AnimGraph>  m_Applied;
    std::vector<AnimGraph>    m_Undo, m_Redo;
    std::vector<std::string>  m_Problems;
    std::string               m_Status;
    std::unique_ptr<FileDialog> m_Dialog;
    int                       m_DialogPurpose = 0; // 1 preview model, 2 notify sound, 3 notify effect
    std::size_t               m_DialogIndex   = 0;

    std::size_t m_Layer = 0;
    int         m_State = -1, m_Transition = -1;
    int         m_LinkFrom = -2; // making a transition: source state (-1 = Any State), -2 none
    glm::vec2   m_Pan{40.0f, 40.0f};
    int         m_Dragging = -1;
    int         m_MenuState = -2; // state of the open context menu (-1 = Any State)

    // Preview.
    std::unique_ptr<Scene>           m_Scene;
    std::unique_ptr<AnimationSystem> m_Preview;
    std::unique_ptr<SceneRenderer>   m_Renderer;
    ModelHandle                      m_Model, m_Ground;
    std::string                      m_ModelPath;
    Entity                           m_PreviewRoot = NullEntity;
    std::map<std::string, float>     m_PreviewValues;
    std::map<std::string, glm::vec3> m_IkTargets; // preview targets of TwoBone / LookAt / Fabrik goals
    bool                             m_Paused = false;
    float                            m_Speed  = 1.0f;
    float                            m_Yaw = 0.8f, m_Pitch = 0.3f, m_Distance = 3.0f;
    glm::vec3                        m_Target{0.0f, 0.5f, 0.0f};
    bool                             m_Framed = false;
    Image                            m_Image;
    std::uint64_t                    m_Texture = 0;
    bool                             m_PreviewVisible = false;
};

} // namespace Engine
