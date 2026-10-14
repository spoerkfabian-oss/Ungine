#pragma once
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/Buffer.h"
#include "Engine/Renderer/Vulkan/Pipeline.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace Engine {

class AssetManager;
class Frustum;
struct ParticleRenderData;

struct ParticleRenderStats {
    std::uint32_t batches   = 0; // drawn (after frustum culling)
    std::uint32_t particles = 0; // sprites + mesh instances
    std::uint32_t ribbonPoints = 0;
    std::uint32_t drawCalls = 0;
};

// Draws ParticleWorld render data inside the lighting pass (HDR target, read-only depth): sprite
// quads, ribbon strips and mesh instances, unlit, alpha-blended (back to front) or additive. The
// particle data is copied into per-frame host-visible buffers (grown on demand).
class ParticleRenderer {
public:
    ParticleRenderer(Renderer& renderer, const AssetManager& assets, VkFormat colorFormat, VkFormat depthFormat);
    ~ParticleRenderer();

    ParticleRenderer(const ParticleRenderer&)            = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    void CreatePipelines(); // also on shader hot reload
    // Outside a rendering scope: copies + sorts this frame's particles.
    void Prepare(const ParticleRenderData& data, const Frustum& frustum, const glm::vec3& camera, std::uint32_t frameIndex);
    // Inside the lighting pass (bindless bound).
    void Draw(VkCommandBuffer cmd, VkDeviceAddress frameAddress, std::uint32_t depthSlot, std::uint32_t frameIndex);
    [[nodiscard]] const ParticleRenderStats& Stats() const { return m_Stats; }

private:
    struct BatchDraw {
        std::uint32_t batch   = 0; // index into the prepared batches
        std::uint32_t first   = 0; // in this frame's buffer
        std::uint32_t count   = 0;
        bool          ribbon  = false;
    };
    void Ensure(Buffer& buffer, std::size_t bytes, const char* name);

    Renderer&           m_Renderer;
    const AssetManager& m_Assets;
    VkFormat            m_ColorFormat, m_DepthFormat;
    std::array<Pipeline, 2> m_Sprite, m_Ribbon, m_Mesh; // [alpha, additive]
    std::array<Buffer, kFramesInFlight> m_Particles, m_Ribbons;
    const ParticleRenderData* m_Data = nullptr;
    std::vector<BatchDraw>     m_Draws;
    std::vector<std::uint32_t> m_Order; // scratch
    ParticleRenderStats        m_Stats;
};

} // namespace Engine
