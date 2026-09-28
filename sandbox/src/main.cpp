#include "Editor/Editor.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Primitives.h"
#include "Engine/Core/Application.h"
#include "Engine/Events/Events.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
    Sandbox(const Engine::ApplicationDesc& desc, std::filesystem::path modelPath, std::uint32_t exitAfterFrames,
            bool startWithEditor)
        : Application(desc), m_ModelPath(std::move(modelPath)), m_ExitAfterFrames(exitAfterFrames),
          m_StartWithEditor(startWithEditor)
    {
        m_KeySub = GetEvents().Subscribe<Engine::KeyEvent>([this](const Engine::KeyEvent& e) {
            // Escape leaves an editor text field first.
            if (e.key == Engine::Key::Escape && e.action == Engine::InputAction::Press &&
                !(m_Editor && m_Editor->WantsKeyboard()))
                GetWindow().RequestClose();
        });
    }

    [[nodiscard]] bool LoadFailed() const { return m_LoadFailed; }

protected:
    void OnInit() override
    {
        m_SceneRenderer = std::make_unique<Engine::SceneRenderer>(GetRenderer(), GetContext(), GetAssets());
        SetEditorEnabled(m_StartWithEditor);

        m_LoadStart = std::chrono::steady_clock::now();
        m_Model     = GetAssets().LoadModel(m_ModelPath); // returns immediately; loads on a worker

        m_LoadedSub = GetEvents().Subscribe<Engine::AssetLoadedEvent<Engine::Model>>(
            [this](const Engine::AssetLoadedEvent<Engine::Model>& e) {
                if (e.handle == m_Model)
                    OnModelLoaded();
                else if (e.handle == m_Ground)
                    OnGroundLoaded();
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
        if (GetInput().WasKeyPressed(Engine::Key::F1))
            SetEditorEnabled(!m_Editor);

        // With the editor, the camera only reacts to the viewport (or while it is looking around).
        if (!m_Editor || m_Editor->ViewportHovered() || m_Camera.IsCaptured())
            m_Camera.Update(GetInput(), GetWindow(), static_cast<float>(dt));
        m_Scene.UpdateTransforms();
        if (!m_Editor || !m_Editor->WantsKeyboard())
            UpdateLookControls(static_cast<float>(dt));
        if (m_Editor)
            m_Editor->Update(static_cast<float>(dt));

        if (m_LoadDone && m_ExitAfterFrames > 0 && ++m_FramesSinceLoad >= m_ExitAfterFrames)
            GetWindow().RequestClose();

        m_FpsTimer += dt;
        ++m_FrameCount;
        if (m_FpsTimer >= 1.0) {
            const auto& stats  = m_SceneRenderer->Stats();
            const auto& post   = m_SceneRenderer->post;
            const char* status = m_LoadFailed ? " | load failed" : (m_LoadDone ? "" : " | loading...");
            GetWindow().SetTitle(std::format(
                "Sandbox | {} FPS | {:.2f} ms | {} draws ({} culled, {} shadow) | {} tris | {} x{:.2f}{}{}{} | "
                "debug {}{}",
                m_FrameCount, 1000.0 * m_FpsTimer / m_FrameCount, stats.drawCalls, stats.culled, stats.shadowDraws,
                stats.triangles, Engine::ToString(post.tonemapper), stats.exposure, post.autoExposure ? " (auto)" : "",
                post.bloom ? " | bloom" : "", m_SceneRenderer->ao.enabled ? " | AO" : "",
                Engine::ToString(post.debugView), status));
            m_FpsTimer   = 0.0;
            m_FrameCount = 0;
        }
    }

    void OnRender(const Engine::FrameContext& frame, double) override
    {
        if (m_Editor) {
            m_Editor->Render(frame);
            return;
        }
        const float aspect = static_cast<float>(frame.extent.width) / static_cast<float>(frame.extent.height);
        m_SceneRenderer->Render(frame, m_Scene, m_Camera.GetData(aspect));
    }

    void OnShutdown() override
    {
        m_Editor.reset();
        GetAssets().Release(m_Model);
        if (m_Ground)
            GetAssets().Release(m_Ground);
    }

private:
    // F1 toggles between the editor (dockable panels, scene in a viewport) and the plain game view.
    void SetEditorEnabled(bool enabled)
    {
        if (enabled == static_cast<bool>(m_Editor))
            return;
        if (enabled)
            m_Editor = std::make_unique<Engine::Editor>(Engine::EditorContext{
                .window        = GetWindow(),
                .renderer      = GetRenderer(),
                .scene         = m_Scene,
                .assets        = GetAssets(),
                .sceneRenderer = *m_SceneRenderer,
                .camera        = m_Camera});
        else
            m_Editor.reset(); // waits for the GPU once
    }

    // T: next tone mapper, -/=: exposure (compensation with auto exposure), X: auto exposure,
    // B: bloom, P: shadows, C: cascade colors, O: ambient occlusion, V: debug view (AO, normals),
    // arrow keys: rotate the sun (regenerates the IBL maps).
    void UpdateLookControls(float dt)
    {
        const Engine::Input& input = GetInput();
        auto&                post  = m_SceneRenderer->post;
        if (input.WasKeyPressed(Engine::Key::T)) {
            const auto next = (static_cast<std::uint32_t>(post.tonemapper) + 1) %
                              static_cast<std::uint32_t>(Engine::Tonemapper::Count);
            post.tonemapper = static_cast<Engine::Tonemapper>(next);
        }
        if (input.WasKeyPressed(Engine::Key::Minus))
            post.exposure /= 1.25f;
        if (input.WasKeyPressed(Engine::Key::Equal))
            post.exposure *= 1.25f;
        if (input.WasKeyPressed(Engine::Key::B))
            post.bloom = !post.bloom;
        if (input.WasKeyPressed(Engine::Key::X))
            post.autoExposure = !post.autoExposure;
        if (input.WasKeyPressed(Engine::Key::V)) {
            const auto next = (static_cast<std::uint32_t>(post.debugView) + 1) %
                              static_cast<std::uint32_t>(Engine::DebugView::Count);
            post.debugView = static_cast<Engine::DebugView>(next);
        }
        if (input.WasKeyPressed(Engine::Key::O))
            m_SceneRenderer->ao.enabled = !m_SceneRenderer->ao.enabled;
        auto& shadows = m_SceneRenderer->shadows;
        if (input.WasKeyPressed(Engine::Key::P))
            shadows.enabled = !shadows.enabled;
        if (input.WasKeyPressed(Engine::Key::C))
            shadows.debugCascades = !shadows.debugCascades;

        const auto  axis  = [&](int positive, int negative) {
            return (input.IsKeyDown(positive) ? 1.0f : 0.0f) - (input.IsKeyDown(negative) ? 1.0f : 0.0f);
        };
        const float yaw   = axis(Engine::Key::Right, Engine::Key::Left);
        const float pitch = axis(Engine::Key::Up, Engine::Key::Down);
        if (yaw != 0.0f || pitch != 0.0f) {
            // From the current direction: the editor may have changed it.
            glm::vec3&  sunDirection = m_SceneRenderer->lighting.sky.sunDirection;
            const glm::vec3 current  = -glm::normalize(sunDirection);
            const float sunYaw       = std::atan2(current.x, current.z) + yaw * dt;
            const float sunElevation = std::clamp(std::asin(std::clamp(current.y, -1.0f, 1.0f)) + pitch * dt, -0.1f, 1.5f);
            const glm::vec3 toSun{std::cos(sunElevation) * std::sin(sunYaw), std::sin(sunElevation),
                                  std::cos(sunElevation) * std::cos(sunYaw)};
            sunDirection = -toSun;
        }
    }

    void OnModelLoaded()
    {
        const Engine::Model* model = GetAssets().Get(m_Model);
        const Engine::Entity root = Engine::InstantiateModel(m_Scene, m_Model, *model);
        m_LoadDone                = true;
        if (m_Editor)
            m_Editor->Select(root);

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

        // Ground plane under the model (receives the shadows), shadow range scaled to the scene.
        Engine::MaterialData ground{.name = "Ground", .baseColorFactor = glm::vec4(0.35f, 0.35f, 0.33f, 1.0f),
                                    .metallic = 0.0f, .roughness = 0.85f};
        m_Ground       = GetAssets().CreateModel(Engine::MakePlane("Ground", radius * 12.0f, ground));
        m_GroundHeight = model->boundsMin.y;
        m_SceneRenderer->shadows.maxDistance = radius * 10.0f;
        m_SceneRenderer->ao.radius           = radius * 0.2f;
    }

    void OnGroundLoaded()
    {
        const Engine::Entity root = Engine::InstantiateModel(m_Scene, m_Ground, *GetAssets().Get(m_Ground));
        m_Scene.GetRegistry().Get<Engine::Transform>(root).position.y = m_GroundHeight;
    }

    std::filesystem::path                  m_ModelPath;
    std::uint32_t                          m_ExitAfterFrames = 0;
    bool                                   m_StartWithEditor = false;
    Engine::Subscription                   m_KeySub, m_LoadedSub, m_FailedSub;
    Engine::Scene                          m_Scene;
    Engine::ModelHandle                    m_Model;
    Engine::ModelHandle                    m_Ground;
    float                                  m_GroundHeight = 0.0f;
    std::unique_ptr<Engine::SceneRenderer> m_SceneRenderer;
    Engine::FlyCamera                      m_Camera;
    std::unique_ptr<Engine::Editor>        m_Editor; // references the members above: declared after them

    std::chrono::steady_clock::time_point m_LoadStart;
    bool                                  m_LoadDone        = false;
    bool                                  m_LoadFailed      = false;
    std::uint32_t                         m_FramesSinceLoad = 0;

    double        m_FpsTimer   = 0.0;
    std::uint32_t m_FrameCount = 0;
};

int main(int argc, char** argv)
{
    // Usage: Sandbox [path/to/model.gltf|.glb] [--frames N] [--editor]
    std::filesystem::path modelPath = "assets/models/WaterBottle.glb";
    std::uint32_t         frames    = 0;
    bool                  editor    = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--frames" && i + 1 < argc)
            frames = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (arg == "--editor")
            editor = true;
        else
            modelPath = arg;
    }
    if (!std::filesystem::exists(modelPath)) {
        ENGINE_ERROR("Model not found: '{}'. Usage: Sandbox [path/to/model.gltf|.glb] [--frames N] [--editor]",
                     modelPath.string());
        return 1;
    }

    // Exit code: 0 ok, 1 fatal error or model failed to load, 2 validation errors (incl. teardown).
    bool loadFailed = false;
    try {
        Sandbox app({.window = {.title = "Sandbox"}, .renderer = {.vsync = true}}, modelPath, frames, editor);
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
