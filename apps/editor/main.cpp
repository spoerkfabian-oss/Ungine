// UngineEditor: the editor application. Started with a project file (double-click on a
// .ungineproj, or the first argument) it opens the project's start scene; without one it shows
// the project browser first (recent projects, new project from a template, open).
#include "Editor/Editor.h"
#include "Editor/ProjectLauncher.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Audio/AudioSystem.h"
#include "Engine/Core/Application.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Project.h"
#include "Engine/Core/Window.h"
#include "Engine/Events/Events.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/Vulkan/VulkanContext.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/LevelStreaming.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Script/ScriptSystem.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace Engine;

// The project browser as its own small application (runs before the editor).
class LauncherApp final : public Application {
public:
    explicit LauncherApp(const ApplicationDesc& desc) : Application(desc) {}
    [[nodiscard]] const std::optional<fs::path>& Chosen() const { return m_Chosen; }

protected:
    void OnInit() override { m_Launcher = std::make_unique<ProjectLauncher>(GetWindow(), GetRenderer()); }
    void OnUpdate(double) override
    {
        m_Launcher->Update();
        if (m_Launcher->Chosen()) {
            m_Chosen = m_Launcher->Chosen();
            GetWindow().RequestClose();
        }
    }
    void OnRender(const FrameContext& frame, double) override { m_Launcher->Render(frame); }
    void OnShutdown() override { m_Launcher.reset(); }

private:
    std::unique_ptr<ProjectLauncher> m_Launcher;
    std::optional<fs::path>          m_Chosen;
};

class EditorApp final : public Application {
public:
    EditorApp(const ApplicationDesc& desc, Project project, std::uint32_t exitAfterFrames)
        : Application(desc), m_Project(std::move(project)), m_ExitAfterFrames(exitAfterFrames)
    {
        // Closing the window with unsaved work asks first.
        m_CloseSub = GetEvents().Subscribe<WindowCloseEvent>([this](const WindowCloseEvent&) {
            if (m_Editor && !m_Editor->ConfirmQuit())
                GetWindow().CancelClose();
        });
    }

protected:
    void OnInit() override
    {
        m_SceneRenderer = std::make_unique<SceneRenderer>(GetRenderer(), GetContext(), GetAssets());
        m_Physics       = std::make_unique<PhysicsWorld>(GetJobs(), GetEvents(), &GetAssets());
        m_Audio         = std::make_unique<AudioSystem>(GetAudio(), &GetAssets(), m_Physics.get());
        m_Audio->Apply(m_Project.settings.audio);
        m_Scripts       = std::make_unique<ScriptSystem>(GetEvents(), &GetInput(), m_Physics.get(), &GetAssets(), m_Audio.get());
        m_Streamer      = std::make_unique<LevelStreamer>(GetJobs(), &GetAssets(), &GetEvents());
        m_Editor        = std::make_unique<Editor>(EditorContext{.window        = GetWindow(),
                                                                 .renderer      = GetRenderer(),
                                                                 .scene         = m_Scene,
                                                                 .assets        = GetAssets(),
                                                                 .sceneRenderer = *m_SceneRenderer,
                                                                 .camera        = m_Camera,
                                                                 .modelRefs     = m_ModelRefs,
                                                                 .physics       = m_Physics.get(),
                                                                 .scripts       = m_Scripts.get(),
                                                                 .audio         = m_Audio.get(),
                                                                 .project       = &m_Project,
                                                                 .streaming     = m_Streamer.get(),
                                                                 .layoutFile    = m_Project.SavedDirectory() / "EditorLayout.ini"});
        std::error_code ec;
        if (const fs::path start = m_Project.StartScene(); !start.empty() && fs::exists(start, ec))
            m_Editor->OpenScene(start);
        else
            m_Editor->NewScene();
    }

    void OnFixedUpdate(double dt) override { m_Editor->FixedUpdate(static_cast<float>(dt)); }

    void OnUpdate(double dt) override
    {
        if (m_Editor->ViewportHovered() || m_Camera.IsCaptured())
            m_Camera.Update(GetInput(), GetWindow(), static_cast<float>(dt));
        m_Editor->Update(static_cast<float>(dt));

        m_TitleTimer += dt;
        if (m_TitleTimer > 0.5) {
            m_TitleTimer             = 0.0;
            const fs::path&   scene  = m_Editor->ScenePath();
            const std::string title  = std::format("{} - {}{} - Ungine Editor", m_Project.settings.name,
                                                   scene.empty() ? std::string("untitled") : PathToUtf8(scene.filename()),
                                                   m_Editor->HasUnsavedChanges() ? "*" : "");
            GetWindow().SetTitle(title);
        }
        if (m_ExitAfterFrames > 0 && ++m_Frames >= m_ExitAfterFrames) // smoke tests
            GetWindow().RequestClose();
    }

    void OnRender(const FrameContext& frame, double alpha) override { m_Editor->Render(frame, static_cast<float>(alpha)); }

    void OnShutdown() override
    {
        m_Editor.reset(); // stops playing, unloads streamed levels
        m_Scripts->End(m_Scene);
        m_Streamer.reset();
        m_Audio.reset(); // releases its sounds
        m_Scene.Clear();
        for (ModelHandle h : m_ModelRefs)
            if (GetAssets().State(h) != AssetState::Invalid)
                GetAssets().Release(h);
        m_ModelRefs.clear();
    }

private:
    Project                        m_Project;
    std::uint32_t                  m_ExitAfterFrames = 0;
    std::uint32_t                  m_Frames          = 0;
    double                         m_TitleTimer      = 1.0;
    Subscription                   m_CloseSub;
    Scene                          m_Scene;
    FlyCamera                      m_Camera;
    std::vector<ModelHandle>       m_ModelRefs;
    std::unique_ptr<SceneRenderer> m_SceneRenderer;
    std::unique_ptr<PhysicsWorld>  m_Physics;
    std::unique_ptr<AudioSystem>   m_Audio;
    std::unique_ptr<ScriptSystem>  m_Scripts;
    std::unique_ptr<LevelStreamer> m_Streamer;
    std::unique_ptr<Editor>        m_Editor; // references the members above: declared after them
};

} // namespace

int main(int argc, char** argv)
{
    // Usage: UngineEditor [project.ungineproj] [--frames N]
    fs::path                       projectFile;
    std::uint32_t                  frames = 0;
    const std::vector<std::string> args   = CommandLineUtf8(argc, argv);
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--frames" && i + 1 < args.size())
            frames = static_cast<std::uint32_t>(std::strtoul(args[++i].c_str(), nullptr, 10));
        else
            projectFile = PathFromUtf8(args[i]);
    }
    (void)Log::OpenFile(PathToUtf8(UserConfigDirectory() / "editor.log").c_str());

    try {
        if (projectFile.empty()) {
            ApplicationDesc desc;
            desc.window        = {.title = "Ungine - Project Browser", .width = 1100, .height = 680};
            desc.pipelineCache = UserConfigDirectory() / "pipelines.bin";
            desc.assets.textures.cacheDirectory = UserConfigDirectory() / "cache" / "textures";
            LauncherApp launcher(desc);
            launcher.Run();
            if (!launcher.Chosen())
                return 0; // closed without choosing
            projectFile = *launcher.Chosen();
        }

        std::string error;
        std::optional<Project> project = Project::Load(projectFile, &error);
        if (!project) {
            ENGINE_ERROR("Cannot open project: {}", error);
            return 1;
        }
        AddRecentProject(*project);
        std::error_code ec;
        fs::create_directories(project->SavedDirectory(), ec);
        fs::current_path(project->Root(), ec); // relative paths (model paths typed in the editor) start there

        ApplicationDesc desc;
        desc.window        = {.title = project->settings.name + " - Ungine Editor", .width = 1600, .height = 900, .maximized = true};
        desc.renderer      = {.vsync = true};
        desc.pipelineCache = project->SavedDirectory() / "pipelines.bin";
        desc.assets.textures.cacheDirectory = project->SavedDirectory() / "Cache" / "Textures";
        EditorApp app(desc, std::move(*project), frames);
        app.Run();
    } catch (const std::exception& e) {
        ENGINE_ERROR("Fatal: {}", e.what());
        Log::CloseFile();
        return 1;
    }
    Log::CloseFile();
    return VulkanContext::ValidationErrorCount() > 0 ? 2 : 0;
}
