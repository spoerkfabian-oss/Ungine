#pragma once
#include "Engine/Renderer/Environment.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Engine/Renderer/Vulkan/Pipeline.h"
#include "Engine/Scene/Camera.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace Engine {

class AssetManager;
class Scene;
struct Mesh;
struct Model;

enum class Tonemapper : std::uint32_t { PbrNeutral, Aces, None, Count }; // mirrors tonemap.frag

[[nodiscard]] const char* ToString(Tonemapper tonemapper);

struct SceneLighting {
    SkySettings sky;                // sun + procedural sky (also drives the IBL maps)
    float       iblIntensity = 1.0f;
};

struct PostSettings {
    float      exposure      = 1.0f;
    Tonemapper tonemapper    = Tonemapper::PbrNeutral;
    bool       bloom         = true;
    float      bloomStrength = 0.04f;  // lerp weight of the blurred image
    float      bloomRadius   = 0.005f; // upsample tent radius, in UV units
};

struct SceneRenderStats {
    std::uint32_t drawCalls   = 0;
    std::uint32_t culled      = 0; // submeshes rejected by frustum culling (camera)
    std::uint32_t shadowDraws = 0; // over all cascades
    std::uint64_t triangles   = 0;
};

// Per frame:
//   (IBL regeneration if the sky changed, compute)
//   cascaded shadow maps: depth-only per cascade, culled against each cascade
//   forward PBR pass -> HDR target (RGBA16F) + depth, frustum-culled per submesh
//   sky pass (analytic sky + sun disk where depth is still at infinity)
//   bloom: 13-tap downsample chain + tent upsample (compute, half resolution)
//   tone mapping pass (+ bloom composite) -> swapchain image
class SceneRenderer {
public:
    SceneRenderer(Renderer& renderer, const VulkanContext& ctx, const AssetManager& assets);
    ~SceneRenderer(); // defers GPU destruction through the renderer

    SceneRenderer(const SceneRenderer&)            = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    void Render(const FrameContext& frame, Scene& scene, const CameraData& camera);

    SceneLighting                         lighting;
    PostSettings                          post;
    ShadowSettings                        shadows;
    [[nodiscard]] const SceneRenderStats& Stats() const { return m_Stats; }

private:
    // One instantiated mesh this frame, shared by the shadow and main passes.
    struct DrawItem {
        const Model*    model = nullptr;
        const Mesh*     mesh  = nullptr;
        glm::mat4       world{1.0f};
        VkDeviceAddress drawData = 0;
        VkFrontFace     frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    };

    void EnsureTargets(VkExtent2D extent);
    void EnsureShadowMap();
    void CollectDrawItems(Scene& scene);
    void RenderShadows(VkCommandBuffer cmd, VkDeviceAddress frameAddress,
                       const std::array<Cascade, kMaxCascades>& cascades, std::uint32_t cascadeCount);
    void RenderMain(VkCommandBuffer cmd, const FrameContext& frame, const CameraData& camera,
                    VkDeviceAddress frameAddress);
    void RenderBloom(VkCommandBuffer cmd);
    void ReleaseTargets();
    void ReleaseShadowMap();

    Renderer&           m_Renderer;
    const AssetManager& m_Assets;
    Environment         m_Environment;
    Pipeline            m_Mesh; // cull mode + front face are dynamic
    Pipeline            m_Sky;
    Pipeline            m_Tonemap;
    Pipeline            m_Shadow, m_ShadowMasked; // depth only / alpha-tested
    Pipeline            m_BloomDown, m_BloomUp;

    // Frame targets (recreated on resize).
    Image                      m_Hdr;
    std::uint32_t              m_HdrSlot = 0;
    Image                      m_Bloom; // half resolution, one level per mip
    std::vector<ImageView>     m_BloomViews;
    std::vector<std::uint32_t> m_BloomSampled, m_BloomStorage;

    // Shadow map: one layer per cascade (recreated when the resolution changes).
    Image                                   m_ShadowMap;
    std::vector<ImageView>                  m_ShadowViews;
    std::array<std::uint32_t, kMaxCascades> m_ShadowSlots{};

    std::vector<DrawItem> m_DrawItems;
    SceneRenderStats      m_Stats;
};

} // namespace Engine
