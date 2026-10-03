// UnginePlayer: runs a project's start scene as the game (physics, visual scripts, audio, the
// scene's primary Camera component). Started by the editor (Build & Run) with the project file, or as a
// packaged game that finds the .ungineproj next to the executable.
// Keys: Esc quits, F11 toggles fullscreen.
#include "Engine/Assets/AssetManager.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Core/Application.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"
#include "Engine/Core/Window.h"
#include "Engine/Events/Events.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/TextOverlay.h"
#include "Engine/Renderer/Vulkan/VulkanContext.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptRegistry.h"
#include "Engine/Script/ScriptSystem.h"
#include "Engine/Assets/Animation.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace Engine;

class PlayerApp final : public Application {
public:
    PlayerApp(const ApplicationDesc& desc, Project project, fs::path saveDirectory, std::uint32_t exitAfterFrames)
        : Application(desc), m_Project(std::move(project)), m_SaveDirectory(std::move(saveDirectory)),
          m_ExitAfterFrames(exitAfterFrames)
    {
        m_KeySub = GetEvents().Subscribe<KeyEvent>([this](const KeyEvent& e) {
            if (e.action != InputAction::Press)
                return;
            if (e.key == Key::Escape)
                GetWindow().RequestClose();
            else if (e.key == 300) // F11
                GetWindow().SetFullscreen(!GetWindow().IsFullscreen());
        });
    }

    [[nodiscard]] bool Failed() const { return m_Failed; }

protected:
    void OnInit() override
    {
        m_SceneRenderer = std::make_unique<SceneRenderer>(GetRenderer(), GetContext(), GetAssets());
        m_Text          = std::make_unique<TextOverlay>(GetRenderer());
        m_Physics       = std::make_unique<PhysicsWorld>(GetJobs(), GetEvents(), &GetAssets());
        m_Audio         = std::make_unique<AudioSystem>(GetAudio(), &GetAssets(), m_Physics.get());
        m_Audio->Apply(m_Project.settings.audio);
        m_Scripts       = std::make_unique<ScriptSystem>(GetEvents(), &GetInput(), m_Physics.get(), &GetAssets(), m_Audio.get());
        m_Scripts->SetInputMap(m_Project.settings.input);
        m_Scripts->SetSaveDirectory(m_SaveDirectory);

        // Blueprint types, interfaces and libraries of the project (before the scene: values of them).
        ScriptRegistry::Clear();
        for (const std::string& problem : ScriptRegistry::LoadDirectory(m_Project.ContentDirectory()))
            ENGINE_WARN("[Blueprint types] {}", problem);
        if (!LoadLevel(m_Project.StartScene())) {
            m_Failed = true;
            GetWindow().RequestClose();
        }
    }

    bool LoadLevel(const fs::path& scene)
    {
        try {
            m_Models = LoadSceneFile(scene, m_Scene, GetAssets(),
                                     {.renderer = m_SceneRenderer.get(), .camera = &m_FallbackCamera, .physics = &m_Physics->settings});
        } catch (const std::exception& e) {
            ENGINE_ERROR("Cannot load the scene: {}", e.what());
            return false;
        }
        m_Scene.UpdateTransforms();
        m_Physics->Sync(m_Scene);
        m_Audio->Begin(m_Scene);
        m_Scripts->SetCurrentLevel(m_Project.Relative(scene));
        m_Scripts->Begin(m_Scene);
        ENGINE_INFO("Playing '{}' ({})", m_Project.settings.name, PathToUtf8(scene));
        return true;
    }

    // Open Level: the scene replaces the current one (scripts, audio and physics start over; save
    // game values in memory stay). A file that does not load keeps the current level.
    void ChangeLevel(const std::string& scene)
    {
        std::error_code ec;
        const fs::path  file = fs::absolute(PathFromUtf8(scene), ec).lexically_normal(); // relative to the project root
        std::vector<ModelHandle> probe; // keeps shared models loaded across the switch
        try {
            Scene scratch;
            probe = LoadSceneFile(file, scratch, GetAssets());
        } catch (const std::exception& e) {
            ENGINE_ERROR("Open Level '{}' failed: {}", scene, e.what());
            return;
        }
        m_Scripts->End(m_Scene);
        m_Audio->End(m_Scene);
        m_Scene.Clear();
        m_Physics->Reset();
        const std::vector<ModelHandle> previous = std::exchange(m_Models, {});
        if (!LoadLevel(file)) {
            m_Failed = true;
            GetWindow().RequestClose();
        }
        for (ModelHandle h : probe)
            GetAssets().Release(h);
        for (ModelHandle h : previous)
            GetAssets().Release(h);
    }

    void OnFixedUpdate(double dt) override
    {
        if (!m_Failed)
            m_Physics->Step(m_Scene, static_cast<float>(dt));
    }

    void OnUpdate(double dt) override
    {
        if (m_Failed)
            return;
        // Mouse / camera nodes work in window coordinates of the whole window.
        m_Scripts->SetViewport({.origin = glm::vec2(0.0f), .size = GetWindow().WindowSize()});
        m_Scripts->Update(m_Scene, static_cast<float>(dt));
        if (const auto request = m_Scripts->TakeLevelRequest()) {
            if (request->quit)
                GetWindow().RequestClose();
            else
                ChangeLevel(request->scene);
        }
        UpdateAnimations(m_Scene, GetAssets(), static_cast<float>(dt));
        m_Scene.UpdateTransforms();
        // Heard from an Audio Listener, else the primary camera, else the saved camera.
        const CameraData view = m_FallbackCamera.GetData(1.0f);
        m_Audio->Update(m_Scene, static_cast<float>(dt), &view);
        if (m_ExitAfterFrames > 0 && ++m_Frames >= m_ExitAfterFrames) // smoke tests
            GetWindow().RequestClose();
    }

    void OnRender(const FrameContext& frame, double alpha) override
    {
        if (m_Failed)
            return;
        m_Physics->Interpolate(m_Scene, static_cast<float>(alpha));
        const float aspect = static_cast<float>(frame.extent.width) / static_cast<float>(std::max(frame.extent.height, 1u));
        CameraData  camera = m_FallbackCamera.GetData(aspect); // the camera saved with the scene
        if (const Entity e = m_Scene.FindPrimaryCamera(); e != NullEntity) {
            const CameraComponent& cam = m_Scene.GetRegistry().Get<CameraComponent>(e);
            camera = CameraFromWorld(m_Scene.GetRegistry().Get<WorldTransform>(e).matrix, cam.fovY, cam.nearPlane, aspect);
        }
        m_SceneRenderer->Render(frame, m_Scene, camera);
        DrawPrints(frame);
    }

    // Print String output, newest first, top left (scaled with the display's pixel density).
    void DrawPrints(const FrameContext& frame)
    {
        const glm::vec2 window = GetWindow().WindowSize();
        const float     dpi    = window.x > 0.0f ? static_cast<float>(frame.extent.width) / window.x : 1.0f;
        const float     scale  = 2.0f * dpi;
        float           y      = 8.0f * dpi;
        const auto      prints = m_Scripts->Messages();
        for (auto it = prints.rbegin(); it != prints.rend() && y < static_cast<float>(frame.extent.height); ++it) {
            m_Text->Add(it->text, {8.0f * dpi, y}, it->error ? glm::vec4(1.0f, 0.45f, 0.4f, 1.0f) : glm::vec4(0.35f, 0.8f, 1.0f, 1.0f),
                        scale);
            y += TextOverlay::Measure(it->text, scale).y + 4.0f * dpi;
        }
        m_Text->Render(frame);
    }

    void OnShutdown() override
    {
        m_Scripts->End(m_Scene);
        const AudioSystemStats& audio = m_Audio->Stats();
        ENGINE_INFO("Audio: {} source(s), {} playing, {} voice(s) on '{}'", audio.sources, audio.playing,
                    m_Audio->Engine().Stats().voices,
                    GetAudio().HasDevice() ? GetAudio().DeviceName() : std::string("no device"));
        m_Audio->End(m_Scene);
        m_Audio.reset(); // releases its sounds
        m_Scene.Clear();
        for (ModelHandle h : m_Models)
            GetAssets().Release(h);
    }

private:
    Project                        m_Project;
    fs::path                       m_SaveDirectory;
    std::uint32_t                  m_ExitAfterFrames = 0;
    std::uint32_t                  m_Frames          = 0;
    bool                           m_Failed          = false;
    Subscription                   m_KeySub;
    Scene                          m_Scene;
    FlyCamera                      m_FallbackCamera;
    std::vector<ModelHandle>       m_Models;
    std::unique_ptr<SceneRenderer> m_SceneRenderer;
    std::unique_ptr<TextOverlay>   m_Text;
    std::unique_ptr<PhysicsWorld>  m_Physics;
    std::unique_ptr<AudioSystem>   m_Audio;
    std::unique_ptr<ScriptSystem>  m_Scripts;
};

// The project next to the executable (packaged game).
fs::path FindProjectNextToExecutable()
{
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(ExecutableDirectory(), ec))
        if (e.path().extension() == Project::kExtension)
            return e.path();
    return {};
}

} // namespace

int main(int argc, char** argv)
{
    // Usage: UnginePlayer [project.ungineproj] [--frames N]
    fs::path                       projectFile;
    std::uint32_t                  frames = 0;
    const std::vector<std::string> args   = CommandLineUtf8(argc, argv);
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--frames" && i + 1 < args.size())
            frames = static_cast<std::uint32_t>(std::strtoul(args[++i].c_str(), nullptr, 10));
        else
            projectFile = PathFromUtf8(args[i]);
    }
    if (projectFile.empty())
        projectFile = FindProjectNextToExecutable();
    if (projectFile.empty()) {
        ENGINE_ERROR("No project: UnginePlayer <project.ungineproj> (or a .ungineproj next to the executable)");
        return 1;
    }
    std::string                  error;
    const std::optional<Project> project = Project::Load(projectFile, &error);
    if (!project) {
        ENGINE_ERROR("Cannot open project: {}", error);
        return 1;
    }
    if (project->settings.startScene.empty()) {
        ENGINE_ERROR("Project '{}' has no start scene (Project Settings in the editor)", project->settings.name);
        return 1;
    }

    // Relative paths in scripts (e.g. "Content/Sounds/hit.wav") start at the project root, as in the editor.
    std::error_code ec;
    fs::current_path(project->Root(), ec);

    // Caches: the project's Saved/ during development (shared with the editor), else per user.
    const fs::path  saved = fs::is_directory(project->SavedDirectory(), ec)
                                ? project->SavedDirectory()
                                : UserConfigDirectory() / "Games" / PathFromUtf8(project->settings.name);
    fs::create_directories(saved, ec);
    (void)Log::OpenFile(PathToUtf8(saved / "player.log").c_str());

    bool failed = false;
    try {
        ApplicationDesc desc;
        desc.window        = {.title      = project->settings.name,
                              .width      = project->settings.windowWidth,
                              .height     = project->settings.windowHeight,
                              .resizable  = true,
                              .fullscreen = project->settings.fullscreen};
        desc.renderer      = {.vsync = project->settings.vsync};
        desc.pipelineCache = saved / "pipelines.bin";
        desc.assets.textures.cacheDirectory = saved / "Cache" / "Textures";
        PlayerApp app(desc, *project, saved / "SaveGames", frames);
        app.Run();
        failed = app.Failed();
    } catch (const std::exception& e) {
        ENGINE_ERROR("Fatal: {}", e.what());
        Log::CloseFile();
        return 1;
    }
    Log::CloseFile();
    if (failed)
        return 1;
    return VulkanContext::ValidationErrorCount() > 0 ? 2 : 0;
}
