#pragma once
#include "Engine/Renderer/Environment.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/Pipeline.h"
#include "Engine/Scene/Camera.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

class AssetManager;
class Scene;

enum class Tonemapper : std::uint32_t { PbrNeutral, Aces, None, Count }; // mirrors tonemap.frag

[[nodiscard]] const char* ToString(Tonemapper tonemapper);

struct SceneLighting {
    SkySettings sky;                // sun + procedural sky (also drives the IBL maps)
    float       iblIntensity = 1.0f;
};

struct PostSettings {
    float      exposure   = 1.0f;
    Tonemapper tonemapper = Tonemapper::PbrNeutral;
};

struct SceneRenderStats {
    std::uint32_t drawCalls = 0;
    std::uint32_t culled    = 0; // submeshes rejected by frustum culling
    std::uint64_t triangles = 0;
};

// Per frame:
//   (IBL regeneration if the sky changed, compute)
//   forward PBR pass -> HDR target (RGBA16F) + depth, frustum-culled per submesh
//   sky pass (analytic sky + sun disk where depth is still at infinity)
//   tone mapping pass -> swapchain image
class SceneRenderer {
public:
    SceneRenderer(Renderer& renderer, const VulkanContext& ctx, const AssetManager& assets);
    ~SceneRenderer(); // defers GPU destruction through the renderer

    SceneRenderer(const SceneRenderer&)            = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    void Render(const FrameContext& frame, Scene& scene, const CameraData& camera);

    SceneLighting                         lighting;
    PostSettings                          post;
    [[nodiscard]] const SceneRenderStats& Stats() const { return m_Stats; }

private:
    void EnsureHdrTarget(VkExtent2D extent);
    void DrawMeshes(VkCommandBuffer cmd, Scene& scene, const CameraData& camera, VkDeviceAddress frameAddress);

    Renderer&           m_Renderer;
    const AssetManager& m_Assets;
    Environment         m_Environment;
    Pipeline            m_Mesh;    // cull mode + front face are dynamic
    Pipeline            m_Sky;
    Pipeline            m_Tonemap;

    Image         m_Hdr;
    std::uint32_t m_HdrSlot = 0;

    SceneRenderStats m_Stats;
};

} // namespace Engine
