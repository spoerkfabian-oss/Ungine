#pragma once
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/Pipeline.h"
#include "Engine/Scene/Camera.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

class AssetManager;
class Scene;

struct SceneLighting {
    glm::vec3 sunDirection{-0.4f, -1.0f, -0.3f}; // direction the light travels
    glm::vec3 sunColor{1.0f, 0.96f, 0.9f};
    float     sunIntensity = 1.0f; // LDR until tone mapping lands
    glm::vec3 ambient{0.12f, 0.13f, 0.15f};
    glm::vec3 clearColor{0.02f, 0.02f, 0.03f};
};

struct SceneRenderStats {
    std::uint32_t drawCalls = 0;
    std::uint64_t triangles = 0;
};

// Forward opaque pass over every (WorldTransform, MeshRenderer) entity whose model is Ready.
// Upcoming: frustum culling, PBR, shadows, HDR target + post-processing.
class SceneRenderer {
public:
    SceneRenderer(Renderer& renderer, const VulkanContext& ctx, const AssetManager& assets);

    void Render(const FrameContext& frame, Scene& scene, const CameraData& camera);

    SceneLighting                         lighting;
    [[nodiscard]] const SceneRenderStats& Stats() const { return m_Stats; }

private:
    Renderer&           m_Renderer;
    const AssetManager& m_Assets;
    Pipeline            m_Opaque;      // back-face culled
    Pipeline         m_DoubleSided; // no culling
    SceneRenderStats m_Stats;
};

} // namespace Engine
