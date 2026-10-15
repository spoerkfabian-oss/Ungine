#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Particles/ParticleEffect.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/Image.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

class AssetManager;
class FileDialog;
class ImGuiLayer;
class ParticleWorld;
class Scene;
class SceneRenderer;

// The particle effect window (.uparticle): emitters, their module stacks by stage (spawn,
// initialize, update, events) with curve / gradient editors, render settings, problems, and a live
// preview (own scene + renderer, orbit camera). Edits apply live to the preview and to the scene's
// ParticleWorld (unsaved, via Provide) and are undoable; Save writes the file. Main thread.
class ParticleEditor {
public:
    struct Context {
        Renderer&      renderer;
        AssetManager&  assets;
        ImGuiLayer&    imgui;
        ParticleWorld* sceneParticles = nullptr; // the edited scene's effects (live edits)
    };
    explicit ParticleEditor(const Context& context);
    ~ParticleEditor(); // GPU resources go through the deferred queues

    ParticleEditor(const ParticleEditor&)            = delete;
    ParticleEditor& operator=(const ParticleEditor&) = delete;

    // Opens a file (a missing one starts as the default effect). Asks nothing: the caller saves first.
    void Open(const std::filesystem::path& file);
    void Close(); // drops unsaved edits (the scene shows the file again)
    [[nodiscard]] bool IsOpen() const { return m_Open; }
    [[nodiscard]] const std::filesystem::path& File() const { return m_File; }
    [[nodiscard]] bool Dirty() const;
    bool Save(std::string* error = nullptr);
    void Revert();

    void Draw(float dt);                    // the window (ImGui frame)
    void Render(const FrameContext& frame); // the preview image, before the UI pass

    // Automation (tests): the edited effect; call Commit() after changing it.
    [[nodiscard]] ParticleEffect& Effect() { return m_Effect; }
    void Commit(); // one undo step, live update
    bool Undo();
    bool Redo();
    void SelectEmitter(std::size_t index) { m_Emitter = index; }
    [[nodiscard]] std::size_t SelectedEmitter() const { return m_Emitter; }
    [[nodiscard]] const ParticleWorld& Preview() const { return *m_Preview; }
    [[nodiscard]] const std::vector<std::string>& Problems() const { return m_Problems; }

private:
    void DrawToolbar();
    void DrawEmitters();
    void DrawEmitter(ParticleEmitter& emitter);
    void DrawStage(ParticleEmitter& emitter, ParticleStage stage);
    void DrawRender(ParticleEmitter& emitter);
    void DrawPreview(float dt);
    bool PathField(const char* label, std::string& path, const std::vector<std::string>& extensions, int purpose);
    void Apply(); // live: preview + scene (Provide), problems
    void EnsureTarget(std::uint32_t width, std::uint32_t height);
    void ReleaseTarget();

    Context                     m_Ctx;
    bool                        m_Open = false;
    std::filesystem::path       m_File;
    ParticleEffect              m_Effect;    // being edited
    ParticleEffect              m_Committed; // last undo state
    ParticleEffect              m_Saved;     // as on disk
    std::optional<ParticleEffect> m_Applied; // last version handed to the worlds
    std::vector<ParticleEffect> m_Undo, m_Redo;
    std::vector<std::string>    m_Problems;
    std::size_t                 m_Emitter = 0;
    std::unique_ptr<FileDialog> m_Dialog;
    int                         m_DialogPurpose = 0; // 1 texture, 2 mesh
    std::string                 m_Status;

    // Preview: its own scene, effect world and renderer.
    std::unique_ptr<Scene>         m_Scene;
    std::unique_ptr<ParticleWorld> m_Preview;
    std::unique_ptr<SceneRenderer> m_Renderer;
    ModelHandle                    m_Ground;
    bool                           m_ShowGround = true;
    bool                           m_Paused     = false;
    float                          m_Speed      = 1.0f;
    float                          m_Yaw = 0.6f, m_Pitch = 0.35f, m_Distance = 6.0f;
    glm::vec3                      m_Target{0.0f, 1.0f, 0.0f};
    Image                          m_Image;
    std::uint64_t                  m_Texture = 0; // ImTextureID
    bool                           m_PreviewVisible = false;
};

} // namespace Engine
