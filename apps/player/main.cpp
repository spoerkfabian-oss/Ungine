// UnginePlayer: runs a project's start scene as the game (physics, visual scripts, audio, the
// scene's primary Camera component). Started by the editor (Build & Run) with the project file, or as a
// packaged game that finds the .ungineproj next to the executable.
// Keys: Esc opens/closes the pause menu, F11 toggles fullscreen. Open Level loads the next scene in
// the background behind a loading screen; streaming volumes / Load Stream Level add sub-levels.
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
#include "Engine/Scene/LevelStreaming.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Script/ScriptRegistry.h"
#include "Engine/Script/ScriptSystem.h"
#include "Engine/Assets/Animation.h"
#include "Engine/UI/UiSystem.h"

#include <array>
#include <algorithm>
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
            if (e.key == 300) // F11
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
        m_Streamer = std::make_unique<LevelStreamer>(GetJobs(), &GetAssets(), &GetEvents());
        m_Streamer->SetUnloadHook([this](Scene& scene, std::span<const Entity> roots) { m_Scripts->EndPlayFor(scene, roots); });
        m_Scripts->SetLevelStreamer(m_Streamer.get());
        m_Loader = std::make_unique<LevelLoader>(GetJobs(), &GetAssets());
        LoadLoadingScreen();

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
        StartLevel(scene);
        return true;
    }

    void StartLevel(const fs::path& scene)
    {
        m_Scene.UpdateTransforms();
        m_Physics->Sync(m_Scene);
        m_Audio->Begin(m_Scene);
        m_Scripts->SetCurrentLevel(m_Project.Relative(scene));
        m_Scripts->Begin(m_Scene);
        CreatePauseMenu();
        ENGINE_INFO("Playing '{}' ({})", m_Project.settings.name, PathToUtf8(scene));
    }

    // Open Level: the next scene is read and its models loaded in the background while a loading
    // screen shows (the current level waits); then it replaces the current one (scripts, audio,
    // physics and streamed levels start over; save game values in memory stay). A file that does
    // not load keeps the current level.
    void ChangeLevel(const std::string& scene)
    {
        std::error_code ec;
        const fs::path  file = fs::absolute(PathFromUtf8(scene), ec).lexically_normal(); // relative to the project root
        if (!m_Loader->Begin(file)) {
            ENGINE_WARN("Open Level '{}' ignored: another level is loading", scene);
            return;
        }
        m_LoadingName = scene;
        m_Audio->SetPaused(true);
    }

    [[nodiscard]] bool LoadingLevel() const { return m_Loader && m_Loader->State() != LevelState::Unloaded; }

    void UpdateLevelLoad()
    {
        m_Loader->Update();
        if (m_Loader->State() == LevelState::Failed) {
            ENGINE_ERROR("Open Level '{}' failed: {}", m_LoadingName, m_Loader->Error());
            m_Loader->Cancel();
            m_Audio->SetPaused(false);
            return;
        }
        if (m_Loader->State() != LevelState::Ready)
            return;
        const fs::path                     file   = m_Loader->File();
        std::optional<LevelLoader::Result> result = m_Loader->Take();
        m_Scripts->End(m_Scene);
        m_Audio->End(m_Scene);
        m_Audio->SetPaused(false);
        m_Scene.Clear();
        m_Streamer->Reset(); // the streamed levels went with the scene
        m_Physics->Reset();
        const std::vector<ModelHandle> previous = std::exchange(m_Models, {}); // released after the new level holds its models
        try {
            (void)InstantiatePreparedScene(*result->scene, m_Scene, &GetAssets(), result->models,
                                           {.renderer = m_SceneRenderer.get(), .camera = &m_FallbackCamera, .physics = &m_Physics->settings});
        } catch (const std::exception& e) {
            ENGINE_ERROR("Open Level '{}' failed: {}", m_LoadingName, e.what());
            m_Failed = true;
            GetWindow().RequestClose();
        }
        for (const auto& [key, handle] : result->models)
            m_Models.push_back(handle);
        for (ModelHandle h : previous)
            GetAssets().Release(h);
        if (!m_Failed)
            StartLevel(file);
    }

    // The project's loading screen scene (UI canvases), if it has one.
    void LoadLoadingScreen()
    {
        if (m_Project.settings.loadingScreen.empty())
            return;
        const fs::path file = (m_Project.Root() / PathFromUtf8(m_Project.settings.loadingScreen)).lexically_normal();
        try {
            m_LoadingModels = LoadSceneFile(file, m_LoadingScene, GetAssets());
            m_LoadingScene.UpdateTransforms();
            m_HasLoadingScreen = true;
        } catch (const std::exception& e) {
            ENGINE_WARN("Loading screen '{}' not used: {}", m_Project.settings.loadingScreen, e.what());
        }
    }

    void DrawLoadingScreen(const FrameContext& frame)
    {
        const glm::vec2 size(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
        const float     progress = m_Loader->Progress();
        m_Text->AddRect(glm::vec2(0.0f), size, glm::vec4(0.02f, 0.02f, 0.03f, 1.0f)); // covers the old frame
        if (m_HasLoadingScreen) {
            m_LoadingScene.GetRegistry().ViewOf<UiWidget, Tags>().Each([&](Entity, UiWidget& widget, const Tags& tags) {
                if (tags.Has("LoadingProgress"))
                    widget.value = widget.minimum + progress * (widget.maximum - widget.minimum);
            });
            m_LoadingUi.PrepareLayout(m_LoadingScene, size);
            m_LoadingUi.SyncAssets(m_LoadingScene, GetAssets());
            m_LoadingUi.Draw(m_LoadingScene, *m_Text);
        } else { // built in: name and a bar in the lower third
            const glm::vec2 window = GetWindow().WindowSize();
            const float     dpi    = window.x > 0.0f ? size.x / window.x : 1.0f;
            const glm::vec2 bar(std::min(size.x * 0.5f, 640.0f * dpi), 8.0f * dpi);
            const glm::vec2 at((size.x - bar.x) * 0.5f, size.y * 0.7f);
            m_Text->Add("Loading " + m_LoadingName, {at.x, at.y - 28.0f * dpi}, glm::vec4(0.85f, 0.88f, 0.95f, 1.0f), 2.0f * dpi);
            m_Text->AddRect(at, bar, glm::vec4(0.15f, 0.16f, 0.2f, 1.0f));
            m_Text->AddRect(at, {bar.x * std::clamp(progress, 0.0f, 1.0f), bar.y}, glm::vec4(0.35f, 0.75f, 1.0f, 1.0f));
        }
        m_Text->Render(frame);
    }

    // The game camera: the scene's primary Camera component, else the camera saved with the scene.
    [[nodiscard]] CameraData CurrentCamera(float aspect)
    {
        if (const Entity e = m_Scene.FindPrimaryCamera(); e != NullEntity) {
            const CameraComponent& cam = m_Scene.GetRegistry().Get<CameraComponent>(e);
            return CameraFromWorld(m_Scene.GetRegistry().Get<WorldTransform>(e).matrix, cam.fovY, cam.nearPlane, aspect);
        }
        return m_FallbackCamera.GetData(aspect);
    }

    void OnFixedUpdate(double dt) override
    {
        if (!m_Failed && !m_Paused && !LoadingLevel())
            m_Physics->Step(m_Scene, static_cast<float>(dt));
    }

    void OnUpdate(double dt) override
    {
        if (m_Failed)
            return;
        if (LoadingLevel()) { // the current level waits behind the loading screen
            UpdateLevelLoad();
            return;
        }
        // Mouse / camera nodes work in window coordinates of the whole window.
        m_Scripts->SetViewport({.origin = glm::vec2(0.0f), .size = GetWindow().WindowSize()});
        if (GetInput().WasKeyPressed(Key::Escape))
            SetPaused(!m_Paused);
        if (!m_Paused)
            m_Scripts->Update(m_Scene, static_cast<float>(dt));
        if (!m_Paused) {
            if (const auto request = m_Scripts->TakeLevelRequest()) {
                if (request->quit)
                    GetWindow().RequestClose();
                else
                    ChangeLevel(request->scene);
            }
        }
        const VkExtent2D framebuffer = GetWindow().FramebufferExtent();
        m_Ui.Update(m_Scene, GetInput(), GetWindow().WindowSize(),
                    {static_cast<float>(framebuffer.width), static_cast<float>(framebuffer.height)});
        m_Ui.SyncAssets(m_Scene, GetAssets());
        if (m_MenuFullscreen != NullEntity && m_Scene.GetRegistry().Valid(m_MenuFullscreen))
            m_Scene.GetRegistry().Get<UiWidget>(m_MenuFullscreen).checked = GetWindow().IsFullscreen();
        for (const UiEvent& event : m_Ui.Events()) {
            HandleMenuEvent(event);
            m_Scripts->DispatchUiEvent(m_Scene, event);
        }
        UpdateAnimations(m_Scene, GetAssets(), static_cast<float>(dt));
        m_Scene.UpdateTransforms();
        // Sub-levels follow the streaming sources (else the camera).
        if (!m_Paused) {
            m_Streamer->Update(m_Scene, CurrentCamera(1.0f).position);
            m_Scene.UpdateTransforms();
        }
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
        if (LoadingLevel()) {
            DrawLoadingScreen(frame);
            return;
        }
        m_Physics->Interpolate(m_Scene, static_cast<float>(alpha));
        const float aspect = static_cast<float>(frame.extent.width) / static_cast<float>(std::max(frame.extent.height, 1u));
        m_SceneRenderer->Render(frame, m_Scene, CurrentCamera(aspect));
        m_Ui.Draw(m_Scene, *m_Text);
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
        m_Streamer->Reset();
        m_Loader->Cancel();
        for (ModelHandle h : m_Models)
            GetAssets().Release(h);
        m_LoadingScene.Clear();
        for (ModelHandle h : m_LoadingModels)
            GetAssets().Release(h);
    }

private:
    Entity AddUiEntity(std::string name, Entity parent, const UiWidget& widget)
    {
        const Entity entity = m_Scene.CreateEntity(std::move(name), parent);
        m_Scene.GetRegistry().Emplace<UiWidget>(entity, widget);
        return entity;
    }

    void CreatePauseMenu()
    {
        Registry& registry = m_Scene.GetRegistry();
        m_MenuCanvas = m_Scene.CreateEntity("Pause Menu");
        registry.Emplace<UiCanvas>(m_MenuCanvas, UiCanvas{.designSize = {1280.0f, 720.0f},
                                                          .sortOrder = 100, .visible = false});
        const auto panel = [&](const char* name) {
            return AddUiEntity(name, m_MenuCanvas,
                               UiWidget{.type = UiWidgetType::Panel,
                                        .anchorMin = {0.5f, 0.5f}, .anchorMax = {0.5f, 0.5f},
                                        .offsetMax = {460.0f, 430.0f}, .pivot = {0.5f, 0.5f},
                                        .background = {0.035f, 0.045f, 0.07f, 0.97f}});
        };
        m_MenuMainPanel = panel("Pause Panel");
        m_MenuOptionsPanel = panel("Options Panel");
        registry.Get<UiWidget>(m_MenuOptionsPanel).visible = false;
        const auto button = [&](Entity parent, const char* name, const char* text, float top) {
            return AddUiEntity(name, parent,
                               UiWidget{.type = UiWidgetType::Button,
                                        .offsetMin = {30.0f, top}, .offsetMax = {430.0f, top + 58.0f},
                                        .color = {1.0f, 1.0f, 1.0f, 1.0f},
                                        .background = {0.14f, 0.2f, 0.3f, 1.0f}, .text = text,
                                        .fontSize = 24.0f});
        };
        AddUiEntity("Paused", m_MenuMainPanel,
                    UiWidget{.type = UiWidgetType::Text,
                             .offsetMin = {30.0f, 24.0f}, .offsetMax = {430.0f, 72.0f},
                             .color = {1.0f, 0.84f, 0.36f, 1.0f}, .text = "PAUSED", .fontSize = 32.0f});
        m_MenuResume = button(m_MenuMainPanel, "Resume", "Resume", 100.0f);
        m_MenuOptions = button(m_MenuMainPanel, "Open Options", "Options", 178.0f);
        m_MenuQuit = button(m_MenuMainPanel, "Quit", "Quit game", 256.0f);

        AddUiEntity("Options", m_MenuOptionsPanel,
                    UiWidget{.type = UiWidgetType::Text,
                             .offsetMin = {30.0f, 24.0f}, .offsetMax = {430.0f, 72.0f},
                             .color = {1.0f, 0.84f, 0.36f, 1.0f}, .text = "OPTIONS", .fontSize = 32.0f});
        const float musicVolume = m_Audio->Engine().BusVolume(AudioBus::Music);
        m_MenuMusic = AddUiEntity("Music volume", m_MenuOptionsPanel,
                                  UiWidget{.type = UiWidgetType::Slider,
                                           .offsetMin = {30.0f, 110.0f}, .offsetMax = {430.0f, 166.0f},
                                           .color = {0.25f, 0.75f, 1.0f, 1.0f}, .text = "Music volume",
                                           .value = musicVolume, .fontSize = 20.0f});
        m_MenuFullscreen = AddUiEntity("Fullscreen", m_MenuOptionsPanel,
                                       UiWidget{.type = UiWidgetType::Checkbox,
                                                .offsetMin = {30.0f, 194.0f}, .offsetMax = {430.0f, 246.0f},
                                                .color = {1.0f, 1.0f, 1.0f, 1.0f}, .text = "Fullscreen",
                                                .fontSize = 20.0f, .checked = GetWindow().IsFullscreen()});
        m_MenuBack = button(m_MenuOptionsPanel, "Back", "Back", 310.0f);
    }

    void SetPaused(bool paused)
    {
        if (m_Paused == paused)
            return;
        m_Paused = paused;
        if (m_MenuCanvas != NullEntity && m_Scene.GetRegistry().Valid(m_MenuCanvas))
            m_Scene.GetRegistry().Get<UiCanvas>(m_MenuCanvas).visible = paused;
        if (m_MenuMainPanel != NullEntity && m_Scene.GetRegistry().Valid(m_MenuMainPanel))
            m_Scene.GetRegistry().Get<UiWidget>(m_MenuMainPanel).visible = true;
        if (m_MenuOptionsPanel != NullEntity && m_Scene.GetRegistry().Valid(m_MenuOptionsPanel))
            m_Scene.GetRegistry().Get<UiWidget>(m_MenuOptionsPanel).visible = false;
        if (paused) {
            constexpr std::array buses{AudioBus::World, AudioBus::Music, AudioBus::Ambient};
            for (std::size_t i = 0; i < buses.size(); ++i) {
                m_PrePauseMuted[i] = m_Audio->Engine().BusMuted(buses[i]);
                m_Audio->Engine().SetBusMuted(buses[i], true);
            }
        } else {
            constexpr std::array buses{AudioBus::World, AudioBus::Music, AudioBus::Ambient};
            for (std::size_t i = 0; i < buses.size(); ++i)
                m_Audio->Engine().SetBusMuted(buses[i], m_PrePauseMuted[i]);
        }
    }

    void HandleMenuEvent(const UiEvent& event)
    {
        if (!m_Paused)
            return;
        if (event.entity == m_MenuResume)
            SetPaused(false);
        else if (event.entity == m_MenuOptions) {
            m_Scene.GetRegistry().Get<UiWidget>(m_MenuMainPanel).visible = false;
            m_Scene.GetRegistry().Get<UiWidget>(m_MenuOptionsPanel).visible = true;
        } else if (event.entity == m_MenuBack) {
            m_Scene.GetRegistry().Get<UiWidget>(m_MenuOptionsPanel).visible = false;
            m_Scene.GetRegistry().Get<UiWidget>(m_MenuMainPanel).visible = true;
        } else if (event.entity == m_MenuQuit)
            GetWindow().RequestClose();
        else if (event.entity == m_MenuMusic && event.type == UiEventType::ValueChanged) {
            m_Audio->Engine().SetBusVolume(AudioBus::Music, event.value);
            m_Project.settings.audio.volume[static_cast<std::size_t>(AudioBus::Music)] = event.value;
        } else if (event.entity == m_MenuFullscreen && event.type == UiEventType::CheckedChanged)
            GetWindow().SetFullscreen(event.checked);
    }

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
    UiSystem                      m_Ui;
    Entity                        m_MenuCanvas = NullEntity;
    Entity                        m_MenuMainPanel = NullEntity;
    Entity                        m_MenuOptionsPanel = NullEntity;
    Entity                        m_MenuResume = NullEntity;
    Entity                        m_MenuOptions = NullEntity;
    Entity                        m_MenuQuit = NullEntity;
    Entity                        m_MenuBack = NullEntity;
    Entity                        m_MenuMusic = NullEntity;
    Entity                        m_MenuFullscreen = NullEntity;
    std::array<bool, 3>           m_PrePauseMuted{};
    bool                          m_Paused = false;
    std::unique_ptr<PhysicsWorld>  m_Physics;
    std::unique_ptr<AudioSystem>   m_Audio;
    std::unique_ptr<ScriptSystem>  m_Scripts;
    std::unique_ptr<LevelStreamer> m_Streamer;
    std::unique_ptr<LevelLoader>   m_Loader;
    std::string                    m_LoadingName;
    Scene                          m_LoadingScene; // the project's loading screen (UI)
    UiSystem                       m_LoadingUi;
    std::vector<ModelHandle>       m_LoadingModels;
    bool                           m_HasLoadingScreen = false;
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
