#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Application.h"
#include "Engine/Events/Events.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <string_view>

class Sandbox final : public Engine::Application {
public:
    // exitAfterFrames > 0: close that many frames after the model finished loading (smoke tests).
    Sandbox(const Engine::ApplicationDesc& desc, std::filesystem::path modelPath, std::uint32_t exitAfterFrames)
        : Application(desc), m_ModelPath(std::move(modelPath)), m_ExitAfterFrames(exitAfterFrames)
    {
        m_KeySub = GetEvents().Subscribe<Engine::KeyEvent>([this](const Engine::KeyEvent& e) {
            if (e.key == Engine::Key::Escape && e.action == Engine::InputAction::Press)
                GetWindow().RequestClose();
        });
    }

    [[nodiscard]] bool LoadFailed() const { return m_LoadFailed; }

protected:
    void OnInit() override
    {
        m_SceneRenderer = std::make_unique<Engine::SceneRenderer>(GetRenderer(), GetContext(), GetAssets());

        m_LoadStart = std::chrono::steady_clock::now();
        m_Model     = GetAssets().LoadModel(m_ModelPath); // returns immediately; loads on a worker

        m_LoadedSub = GetEvents().Subscribe<Engine::AssetLoadedEvent<Engine::Model>>(
            [this](const Engine::AssetLoadedEvent<Engine::Model>& e) {
                if (e.handle == m_Model)
                    OnModelLoaded();
            });
        m_FailedSub = GetEvents().Subscribe<Engine::AssetFailedEvent<Engine::Model>>(
            [this](const Engine::AssetFailedEvent<Engine::Model>& e) {
                if (e.handle == m_Model) {
                    m_LoadFailed = true;
                    m_LoadDone   = true;
                }
            });
    }

    void OnUpdate(double dt) override
    {
        m_Camera.Update(GetInput(), GetWindow(), static_cast<float>(dt));
        m_Scene.UpdateTransforms();

        if (m_LoadDone && m_ExitAfterFrames > 0 && ++m_FramesSinceLoad >= m_ExitAfterFrames)
            GetWindow().RequestClose();

        m_FpsTimer += dt;
        ++m_FrameCount;
        if (m_FpsTimer >= 1.0) {
            const auto& stats  = m_SceneRenderer->Stats();
            const char* status = m_LoadFailed ? " | load failed" : (m_LoadDone ? "" : " | loading...");
            GetWindow().SetTitle(std::format("Sandbox | {} FPS | {:.2f} ms | {} draws | {} tris{}", m_FrameCount,
                                             1000.0 * m_FpsTimer / m_FrameCount, stats.drawCalls, stats.triangles,
                                             status));
            m_FpsTimer   = 0.0;
            m_FrameCount = 0;
        }
    }

    void OnRender(const Engine::FrameContext& frame, double) override
    {
        const float aspect = static_cast<float>(frame.extent.width) / static_cast<float>(frame.extent.height);
        m_SceneRenderer->Render(frame, m_Scene, m_Camera.GetData(aspect));
    }

    void OnShutdown() override { GetAssets().Release(m_Model); }

private:
    void OnModelLoaded()
    {
        const Engine::Model* model = GetAssets().Get(m_Model);
        Engine::InstantiateModel(m_Scene, m_Model, *model);
        m_LoadDone = true;

        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_LoadStart).count();
        ENGINE_INFO("Loaded '{}' in {:.1f} ms", model->name, ms);

        // Frame the model: back off along +Z by its bounding radius.
        const glm::vec3 center = (model->boundsMin + model->boundsMax) * 0.5f;
        const float     radius = std::max(glm::length(model->boundsMax - model->boundsMin) * 0.5f, 0.01f);
        m_Camera.position  = center + glm::vec3(0.0f, radius * 0.3f, radius * 2.2f);
        m_Camera.nearPlane = std::clamp(radius * 0.01f, 0.001f, 0.1f);
        m_Camera.moveSpeed = std::max(radius, 0.1f);
        m_Camera.LookAt(center);
    }

    std::filesystem::path                  m_ModelPath;
    std::uint32_t                          m_ExitAfterFrames = 0;
    Engine::Subscription                   m_KeySub, m_LoadedSub, m_FailedSub;
    Engine::Scene                          m_Scene;
    Engine::ModelHandle                    m_Model;
    std::unique_ptr<Engine::SceneRenderer> m_SceneRenderer;
    Engine::FlyCamera                      m_Camera;

    std::chrono::steady_clock::time_point m_LoadStart;
    bool                                  m_LoadDone        = false;
    bool                                  m_LoadFailed      = false;
    std::uint32_t                         m_FramesSinceLoad = 0;

    double        m_FpsTimer   = 0.0;
    std::uint32_t m_FrameCount = 0;
};

int main(int argc, char** argv)
{
    // Usage: Sandbox [path/to/model.gltf|.glb] [--frames N]
    std::filesystem::path modelPath = "assets/models/BoxTextured.glb";
    std::uint32_t         frames    = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--frames" && i + 1 < argc)
            frames = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else
            modelPath = arg;
    }
    if (!std::filesystem::exists(modelPath)) {
        ENGINE_ERROR("Model not found: '{}'. Usage: Sandbox [path/to/model.gltf|.glb] [--frames N]",
                     modelPath.string());
        return 1;
    }

    // Exit code: 0 ok, 1 fatal error or model failed to load, 2 validation errors (incl. teardown).
    bool loadFailed = false;
    try {
        Sandbox app({.window = {.title = "Sandbox"}, .renderer = {.vsync = true}}, modelPath, frames);
        app.Run();
        loadFailed = app.LoadFailed();
    } catch (const std::exception& e) {
        ENGINE_ERROR("Fatal: {}", e.what());
        return 1;
    }
    if (loadFailed)
        return 1;
    if (const std::uint32_t errors = Engine::VulkanContext::ValidationErrorCount(); errors > 0) {
        ENGINE_ERROR("{} Vulkan validation error(s)", errors);
        return 2;
    }
    return 0;
}
