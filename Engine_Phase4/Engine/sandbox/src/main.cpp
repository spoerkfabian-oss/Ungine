#include "Engine/Assets/GltfLoader.h"
#include "Engine/Core/Application.h"
#include "Engine/Events/Events.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>

class Sandbox final : public Engine::Application {
public:
    Sandbox(const Engine::ApplicationDesc& desc, std::filesystem::path modelPath)
        : Application(desc), m_ModelPath(std::move(modelPath))
    {
        m_KeySub = GetEvents().Subscribe<Engine::KeyEvent>([this](const Engine::KeyEvent& e) {
            if (e.key == Engine::Key::Escape && e.action == Engine::InputAction::Press)
                GetWindow().RequestClose();
        });
    }

protected:
    void OnInit() override
    {
        m_SceneRenderer = std::make_unique<Engine::SceneRenderer>(GetRenderer(), GetContext());

        const Engine::ModelData data = Engine::LoadGltf(m_ModelPath);
        m_Model = Engine::UploadModel(GetRenderer(), data);
        Engine::InstantiateModel(m_Scene, m_Model);

        // Frame the model: back off along +Z by its bounding radius.
        const glm::vec3 center = (m_Model->boundsMin + m_Model->boundsMax) * 0.5f;
        const float     radius = std::max(glm::length(m_Model->boundsMax - m_Model->boundsMin) * 0.5f, 0.01f);
        m_Camera.position  = center + glm::vec3(0.0f, radius * 0.3f, radius * 2.2f);
        m_Camera.nearPlane = std::clamp(radius * 0.01f, 0.001f, 0.1f);
        m_Camera.moveSpeed = std::max(radius, 0.1f);
        m_Camera.LookAt(center);
    }

    void OnUpdate(double dt) override
    {
        m_Camera.Update(GetInput(), GetWindow(), static_cast<float>(dt));
        m_Scene.UpdateTransforms();

        m_FpsTimer += dt;
        ++m_FrameCount;
        if (m_FpsTimer >= 1.0) {
            const auto& stats = m_SceneRenderer->Stats();
            GetWindow().SetTitle(std::format("Sandbox | {} FPS | {:.2f} ms | {} draws | {} tris", m_FrameCount,
                                             1000.0 * m_FpsTimer / m_FrameCount, stats.drawCalls, stats.triangles));
            m_FpsTimer   = 0.0;
            m_FrameCount = 0;
        }
    }

    void OnRender(const Engine::FrameContext& frame, double) override
    {
        const float aspect = static_cast<float>(frame.extent.width) / static_cast<float>(frame.extent.height);
        m_SceneRenderer->Render(frame, m_Scene, m_Camera.GetData(aspect));
    }

private:
    // Destruction order (reverse): renderer pipelines, model ref, scene (last model ref -> deferred release).
    std::filesystem::path                 m_ModelPath;
    Engine::Subscription                  m_KeySub;
    Engine::Scene                         m_Scene;
    std::shared_ptr<const Engine::Model>  m_Model;
    std::unique_ptr<Engine::SceneRenderer> m_SceneRenderer;
    Engine::FlyCamera                     m_Camera;

    double        m_FpsTimer   = 0.0;
    std::uint32_t m_FrameCount = 0;
};

int main(int argc, char** argv)
{
    const std::filesystem::path modelPath =
        argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("assets/models/DamagedHelmet.glb");
    if (!std::filesystem::exists(modelPath)) {
        ENGINE_ERROR("Model not found: '{}'. Usage: Sandbox <path/to/model.gltf|.glb>", modelPath.string());
        return 1;
    }

    try {
        Sandbox app({.window = {.title = "Sandbox"}, .renderer = {.vsync = true}}, modelPath);
        app.Run();
    } catch (const std::exception& e) {
        ENGINE_ERROR("Fatal: {}", e.what());
        return 1;
    }
    return 0;
}
