#include "Renderer/ParticleRenderer.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/Platform.h"
#include "Engine/Particles/ParticleWorld.h"
#include "Engine/Renderer/Vulkan/Bindless.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Frustum.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace Engine {

namespace {

struct ParticlePush { // mirrors ParticlePush in particle.glsl
    VkDeviceAddress frame;
    VkDeviceAddress data;
    std::uint32_t   first;
    std::uint32_t   texture;
    std::uint32_t   columns;
    std::uint32_t   rows;
    std::uint32_t   facing;
    std::uint32_t   flags;
    float           stretch;
    float           softness;
    glm::vec4       axis;
    std::uint32_t   depthTexture;
    std::uint32_t   material;
    std::uint32_t   count;
    float           pad0;
};
static_assert(sizeof(ParticlePush) == 80);

constexpr std::uint32_t kSoft     = 1u;
constexpr std::uint32_t kTextured = 2u;
constexpr std::uint32_t kAdditive = 4u;
constexpr std::uint32_t kShade    = 8u;
constexpr std::uint32_t kAlign    = 16u;
constexpr std::uint32_t kRepeat   = 32u;

} // namespace

ParticleRenderer::ParticleRenderer(Renderer& renderer, const AssetManager& assets, VkFormat colorFormat, VkFormat depthFormat)
    : m_Renderer(renderer), m_Assets(assets), m_ColorFormat(colorFormat), m_DepthFormat(depthFormat)
{
    CreatePipelines();
}

ParticleRenderer::~ParticleRenderer()
{
    for (auto* set : {&m_Sprite, &m_Ribbon, &m_Mesh})
        for (Pipeline& p : *set)
            if (p)
                m_Renderer.DeferRelease(std::move(p));
    for (auto* set : {&m_Particles, &m_Ribbons})
        for (Buffer& b : *set)
            if (b)
                m_Renderer.DeferRelease(std::move(b));
}

void ParticleRenderer::CreatePipelines()
{
    const VkDevice         device = m_Renderer.GetContext().Device();
    const VkPipelineLayout layout = m_Renderer.GetBindless().PipelineLayout();
    std::array<Pipeline, 2> sprite, ribbon, mesh;
    for (int i = 0; i < 2; ++i) {
        const BlendMode blend = i == 0 ? BlendMode::Alpha : BlendMode::Additive;
        // Depth tested against the scene (reverse-Z), never written: particles overlap freely.
        sprite[i] = GraphicsPipelineBuilder{}
                        .SetShaders(ShaderPath("particle.vert.spv"), ShaderPath("particle.frag.spv"))
                        .AddColorAttachment(m_ColorFormat)
                        .SetDepthFormat(m_DepthFormat)
                        .SetDepth(true, false, VK_COMPARE_OP_GREATER_OR_EQUAL)
                        .SetBlend(blend)
                        .SetDebugName(i == 0 ? "ParticleSprite" : "ParticleSpriteAdd")
                        .Build(device, layout);
        ribbon[i] = GraphicsPipelineBuilder{}
                        .SetShaders(ShaderPath("particle_ribbon.vert.spv"), ShaderPath("particle.frag.spv"))
                        .AddColorAttachment(m_ColorFormat)
                        .SetDepthFormat(m_DepthFormat)
                        .SetDepth(true, false, VK_COMPARE_OP_GREATER_OR_EQUAL)
                        .SetBlend(blend)
                        .SetDebugName(i == 0 ? "ParticleRibbon" : "ParticleRibbonAdd")
                        .Build(device, layout);
        mesh[i] = GraphicsPipelineBuilder{}
                      .SetShaders(ShaderPath("particle_mesh.vert.spv"), ShaderPath("particle_mesh.frag.spv"))
                      .AddColorAttachment(m_ColorFormat)
                      .SetDepthFormat(m_DepthFormat)
                      .SetDepth(true, false, VK_COMPARE_OP_GREATER_OR_EQUAL)
                      .SetCulling(VK_CULL_MODE_BACK_BIT)
                      .SetBlend(blend)
                      .SetDebugName(i == 0 ? "ParticleMesh" : "ParticleMeshAdd")
                      .Build(device, layout);
    }
    const auto replace = [&](std::array<Pipeline, 2>& current, std::array<Pipeline, 2>& fresh) {
        for (int i = 0; i < 2; ++i) {
            if (current[i])
                m_Renderer.DeferRelease(std::move(current[i]));
            current[i] = std::move(fresh[i]);
        }
    };
    replace(m_Sprite, sprite);
    replace(m_Ribbon, ribbon);
    replace(m_Mesh, mesh);
}

void ParticleRenderer::Ensure(Buffer& buffer, std::size_t bytes, const char* name)
{
    if (buffer && buffer.Size() >= bytes)
        return;
    if (buffer)
        m_Renderer.DeferRelease(std::move(buffer));
    const std::size_t size = std::max<std::size_t>(bytes + bytes / 2, 64 * 1024);
    buffer = Buffer(m_Renderer.GetContext(), {.size      = size,
                                              .usage     = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                              .memory    = MemoryUsage::Upload,
                                              .debugName = name});
}

void ParticleRenderer::Prepare(const ParticleRenderData& data, const Frustum& frustum, const glm::vec3& camera,
                               std::uint32_t frameIndex)
{
    m_Stats = {};
    m_Data  = &data;
    m_Draws.clear();
    // Visible batches, far to near (alpha blending composes back to front; additive does not care).
    std::vector<std::pair<float, std::uint32_t>> visible;
    for (std::uint32_t i = 0; i < data.batches.size(); ++i) {
        const ParticleBatch& b = data.batches[i];
        if (b.count == 0 || !frustum.Intersects(Aabb{b.boundsMin, b.boundsMax}))
            continue;
        const glm::vec3 center = (b.boundsMin + b.boundsMax) * 0.5f;
        const glm::vec3 d      = center - camera;
        visible.emplace_back(glm::dot(d, d), i);
    }
    std::ranges::sort(visible, std::greater{}, &std::pair<float, std::uint32_t>::first);

    std::size_t particles = 0, points = 0;
    for (const auto& [distance, i] : visible) {
        const ParticleBatch& b = data.batches[i];
        (b.render.type == ParticleRenderType::Ribbon ? points : particles) += b.count;
    }
    Buffer& particleBuffer = m_Particles[frameIndex];
    Buffer& ribbonBuffer   = m_Ribbons[frameIndex];
    if (particles > 0)
        Ensure(particleBuffer, particles * sizeof(GpuParticle), "ParticleInstances");
    if (points > 0)
        Ensure(ribbonBuffer, points * sizeof(GpuRibbonPoint), "ParticleRibbons");
    auto*         particleOut = particles > 0 ? static_cast<GpuParticle*>(particleBuffer.Mapped()) : nullptr;
    auto*         ribbonOut   = points > 0 ? static_cast<GpuRibbonPoint*>(ribbonBuffer.Mapped()) : nullptr;
    std::uint32_t particleAt = 0, ribbonAt = 0;
    for (const auto& [distance, i] : visible) {
        const ParticleBatch& b = data.batches[i];
        BatchDraw            draw{.batch = i, .count = b.count};
        if (b.render.type == ParticleRenderType::Ribbon) {
            draw.ribbon = true;
            draw.first  = ribbonAt;
            std::memcpy(ribbonOut + ribbonAt, data.ribbonPoints.data() + b.first, b.count * sizeof(GpuRibbonPoint));
            ribbonAt += b.count;
            m_Stats.ribbonPoints += b.count;
        } else {
            draw.first = particleAt;
            const GpuParticle* source = data.particles.data() + b.first;
            if (b.render.sort && b.render.blend == ParticleBlend::Alpha && b.count > 1) {
                m_Order.resize(b.count);
                std::iota(m_Order.begin(), m_Order.end(), 0u);
                std::vector<float> keys(b.count);
                for (std::uint32_t k = 0; k < b.count; ++k) {
                    const glm::vec3 d = source[k].position - camera;
                    keys[k]           = glm::dot(d, d);
                }
                std::ranges::sort(m_Order, [&](std::uint32_t x, std::uint32_t y) { return keys[x] > keys[y]; });
                for (std::uint32_t k = 0; k < b.count; ++k)
                    particleOut[particleAt + k] = source[m_Order[k]];
            } else {
                std::memcpy(particleOut + particleAt, source, b.count * sizeof(GpuParticle));
            }
            particleAt += b.count;
            m_Stats.particles += b.count;
        }
        m_Draws.push_back(draw);
    }
    if (particleAt > 0)
        particleBuffer.Flush(0, particleAt * sizeof(GpuParticle));
    if (ribbonAt > 0)
        ribbonBuffer.Flush(0, ribbonAt * sizeof(GpuRibbonPoint));
    m_Stats.batches = static_cast<std::uint32_t>(m_Draws.size());
}

void ParticleRenderer::Draw(VkCommandBuffer cmd, VkDeviceAddress frameAddress, std::uint32_t depthSlot, std::uint32_t frameIndex)
{
    if (!m_Data || m_Draws.empty())
        return;
    const VkPipelineLayout layout       = m_Renderer.GetBindless().PipelineLayout();
    VkPipeline             bound        = VK_NULL_HANDLE;
    bool                   indexBound   = false;
    const auto             bind         = [&](const Pipeline& pipeline) {
        if (pipeline.Handle() != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Handle());
            bound = pipeline.Handle();
        }
    };
    for (const BatchDraw& draw : m_Draws) {
        const ParticleBatch&  b      = m_Data->batches[draw.batch];
        const ParticleRender& r      = b.render;
        const int             blend  = r.blend == ParticleBlend::Additive ? 1 : 0;
        ParticlePush          push{};
        push.frame        = frameAddress;
        push.first        = draw.first;
        push.count        = draw.count;
        push.columns      = std::max(r.columns, 1u);
        push.rows         = std::max(r.rows, 1u);
        push.facing       = static_cast<std::uint32_t>(r.facing);
        push.stretch      = r.stretch;
        push.softness     = r.softness;
        push.axis         = glm::vec4(glm::length(r.axis) > 1e-6f ? glm::normalize(r.axis) : glm::vec3(0, 1, 0), r.cameraFade);
        push.depthTexture = depthSlot;
        push.flags        = (r.softness > 0.0f ? kSoft : 0u) | (blend ? kAdditive : 0u);
        if (b.texture) {
            push.texture = m_Assets.TableEntry(b.texture);
            push.flags |= kTextured;
        }
        if (draw.ribbon) {
            if (draw.count < 2)
                continue;
            push.data   = m_Ribbons[frameIndex].Address();
            push.facing = 3; // FACING_RIBBON
            push.flags |= r.ribbonUv == RibbonUv::Tile ? kRepeat : 0u;
            bind(m_Ribbon[blend]);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 6 * (draw.count - 1), 1, 0, 0);
            ++m_Stats.drawCalls;
            continue;
        }
        push.data = m_Particles[frameIndex].Address();
        if (r.type == ParticleRenderType::Sprite) {
            bind(m_Sprite[blend]);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 6, draw.count, 0, 0);
            ++m_Stats.drawCalls;
            continue;
        }
        // Mesh particles: every submesh of the mesh, instanced over the batch.
        const ResolvedMesh resolved = m_Assets.ResolveMesh(b.mesh, r.meshIndex);
        if (!resolved.model || resolved.placeholder || resolved.meshIndex >= resolved.model->meshes.size())
            continue;
        const Model& model = *resolved.model;
        const Mesh&  mesh  = model.meshes[resolved.meshIndex];
        push.flags |= (r.shade ? kShade : 0u) | (r.alignToVelocity ? kAlign : 0u);
        bind(m_Mesh[blend]);
        if (!indexBound) {
            vkCmdBindIndexBuffer(cmd, m_Renderer.Geometry().IndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
            indexBound = true;
        }
        for (std::size_t k = 0; k < mesh.submeshes.size(); ++k) {
            const std::size_t record = mesh.firstGpuSubmesh - model.submeshes.offset + k;
            if (record >= model.gpuSubmeshes.size())
                break;
            const GpuSubmesh& s = model.gpuSubmeshes[record];
            push.material       = s.material;
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
            vkCmdDrawIndexed(cmd, s.indexCount, draw.count, s.firstIndex, s.vertexOffset, 0);
            ++m_Stats.drawCalls;
        }
    }
}

} // namespace Engine
