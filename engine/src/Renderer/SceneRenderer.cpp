#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Model.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Frustum.h"
#include "Engine/Scene/Scene.h"

namespace Engine {

namespace {
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct FrameUniforms { // mirrors FrameData in frame.glsl
    glm::mat4  viewProj;
    glm::mat4  view;
    glm::mat4  proj;
    glm::mat4  invViewProj;
    glm::vec4  cameraPosition;
    glm::vec4  sunDirection;
    glm::vec4  sunRadiance;
    glm::vec4  sky;
    glm::uvec4 ibl;
};

struct DrawData { // mirrors DrawData in mesh_common.glsl
    glm::mat4 model;
    glm::mat4 normalMatrix;
};

struct MeshPush { // mirrors MeshPush in mesh_common.glsl
    VkDeviceAddress frame;
    VkDeviceAddress vertices;
    VkDeviceAddress materials;
    VkDeviceAddress draw;
    std::uint32_t   materialIndex;
};
static_assert(sizeof(MeshPush) <= kPushConstantSize);

struct TonemapPush { // mirrors TonemapPush in tonemap.frag
    std::uint32_t hdrTexture;
    std::uint32_t tonemapper;
    float         exposure;
};

VkRenderingAttachmentInfo Attachment(VkImageView view, VkImageLayout layout, VkAttachmentLoadOp load)
{
    VkRenderingAttachmentInfo a{};
    a.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a.imageView   = view;
    a.imageLayout = layout;
    a.loadOp      = load;
    a.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    return a;
}

void BeginRendering(VkCommandBuffer cmd, VkExtent2D extent, const VkRenderingAttachmentInfo& color,
                    const VkRenderingAttachmentInfo* depth)
{
    VkRenderingInfo info{};
    info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea           = {{0, 0}, extent};
    info.layerCount           = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments    = &color;
    info.pDepthAttachment     = depth;
    vkCmdBeginRendering(cmd, &info);
}
} // namespace

const char* ToString(Tonemapper t)
{
    switch (t) {
    case Tonemapper::PbrNeutral: return "PBR Neutral";
    case Tonemapper::Aces:       return "ACES";
    case Tonemapper::None:       return "None";
    default:                     return "?";
    }
}

SceneRenderer::SceneRenderer(Renderer& renderer, const VulkanContext& ctx, const AssetManager& assets)
    : m_Renderer(renderer), m_Assets(assets), m_Environment(renderer)
{
    const VkDevice         device = ctx.Device();
    const VkPipelineLayout layout = renderer.GetBindless().PipelineLayout();

    m_Mesh = GraphicsPipelineBuilder{}
                 .SetShaders(ShaderPath("mesh.vert.spv"), ShaderPath("mesh.frag.spv"))
                 .AddColorAttachment(kHdrFormat)
                 .SetDepthFormat(kDepthFormat)
                 .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL) // reverse-Z
                 .SetDynamicCulling(true)
                 .SetDebugName("MeshPbr")
                 .Build(device, layout);

    // Depth 0 = infinity: passes only where no geometry was drawn. No depth writes.
    m_Sky = GraphicsPipelineBuilder{}
                .SetShaders(ShaderPath("fullscreen.vert.spv"), ShaderPath("sky.frag.spv"))
                .AddColorAttachment(kHdrFormat)
                .SetDepthFormat(kDepthFormat)
                .SetDepth(true, false, VK_COMPARE_OP_GREATER_OR_EQUAL)
                .SetDebugName("Sky")
                .Build(device, layout);

    m_Tonemap = GraphicsPipelineBuilder{}
                    .SetShaders(ShaderPath("fullscreen.vert.spv"), ShaderPath("tonemap.frag.spv"))
                    .AddColorAttachment(renderer.GetSwapchain().Format())
                    .SetDebugName("Tonemap")
                    .Build(device, layout);
}

SceneRenderer::~SceneRenderer()
{
    if (m_Hdr) {
        Renderer* r = &m_Renderer;
        r->DeferCall([r, slot = m_HdrSlot] { r->GetBindless().RemoveSampledImage(slot); });
        r->DeferRelease(std::move(m_Hdr));
    }
    m_Renderer.DeferRelease(std::move(m_Mesh));
    m_Renderer.DeferRelease(std::move(m_Sky));
    m_Renderer.DeferRelease(std::move(m_Tonemap));
}

void SceneRenderer::EnsureHdrTarget(VkExtent2D extent)
{
    if (m_Hdr && m_Hdr.Extent().width == extent.width && m_Hdr.Extent().height == extent.height)
        return;
    if (m_Hdr) { // resized: frames in flight may still sample the old one
        Renderer* r = &m_Renderer;
        r->DeferCall([r, slot = m_HdrSlot] { r->GetBindless().RemoveSampledImage(slot); });
        r->DeferRelease(std::move(m_Hdr));
    }
    m_Hdr     = Image(m_Renderer.GetContext(), {.extent    = {extent.width, extent.height, 1},
                                                .format    = kHdrFormat,
                                                .usage     = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                             VK_IMAGE_USAGE_SAMPLED_BIT,
                                                .debugName = "HdrColor"});
    m_HdrSlot = m_Renderer.GetBindless().AddSampledImage(m_Hdr.View());
}

void SceneRenderer::Render(const FrameContext& frame, Scene& scene, const CameraData& camera)
{
    m_Stats = {};
    const VkCommandBuffer cmd      = frame.cmd;
    const auto&           bindless = m_Renderer.GetBindless();
    const SkySettings&    sky      = lighting.sky;

    EnsureHdrTarget(frame.extent);
    m_Environment.Update(cmd, sky); // compute, only when the sky changed

    const glm::mat4     viewProj = camera.projection * camera.view;
    const FrameUniforms uniforms{
        .viewProj       = viewProj,
        .view           = camera.view,
        .proj           = camera.projection,
        .invViewProj    = glm::inverse(viewProj),
        .cameraPosition = glm::vec4(camera.position, 1.0f),
        .sunDirection   = glm::vec4(glm::normalize(sky.sunDirection), 0.0f),
        .sunRadiance    = glm::vec4(sky.sunColor * sky.sunIntensity, 0.0f),
        .sky            = glm::vec4(sky.skyIntensity, lighting.iblIntensity, 0.0f, 0.0f),
        .ibl            = glm::uvec4(m_Environment.IrradianceCube(), m_Environment.PrefilteredCube(),
                                     m_Environment.BrdfLut(), m_Environment.PrefilteredMipCount())};
    const VkDeviceAddress frameAddress = m_Renderer.PushTransient(uniforms);

    // --- Scene -> HDR. The target is shared by all frames in flight: wait for the previous
    //     frame's tone mapping read before overwriting (WAR, execution dependency only). ---
    CmdImageBarrier(cmd, {.image     = m_Hdr.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .dstAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});

    // No color clear: the sky covers every pixel without geometry.
    const VkRenderingAttachmentInfo hdr =
        Attachment(m_Hdr.View(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    VkRenderingAttachmentInfo depth =
        Attachment(frame.depthView, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR);
    depth.clearValue.depthStencil = {0.0f, 0}; // reverse-Z: far = 0

    BeginRendering(cmd, frame.extent, hdr, &depth);
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    SetViewportScissor(cmd, frame.extent);

    DrawMeshes(cmd, scene, camera, frameAddress);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Sky.Handle());
    vkCmdPushConstants(cmd, bindless.PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(frameAddress), &frameAddress);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    // --- HDR -> swapchain (tone mapping) ---
    CmdImageBarrier(cmd, {.image     = m_Hdr.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          .srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          .dstStage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});

    BeginRendering(cmd, frame.extent,
                   Attachment(frame.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE),
                   nullptr);
    const TonemapPush tonemap{.hdrTexture = m_HdrSlot,
                              .tonemapper = static_cast<std::uint32_t>(post.tonemapper),
                              .exposure   = post.exposure};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Tonemap.Handle());
    vkCmdPushConstants(cmd, bindless.PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(tonemap), &tonemap);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void SceneRenderer::DrawMeshes(VkCommandBuffer cmd, Scene& scene, const CameraData& camera,
                               VkDeviceAddress frameAddress)
{
    const VkPipelineLayout layout  = m_Renderer.GetBindless().PipelineLayout();
    const Frustum          frustum = Frustum::FromViewProjection(camera.projection * camera.view);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Mesh.Handle());

    const Model*    boundModel = nullptr;
    ModelHandle     lastHandle;
    const Model*    lastModel = nullptr; // entities of one model are usually contiguous
    VkCullModeFlags cullMode  = VK_CULL_MODE_FLAG_BITS_MAX_ENUM;
    VkFrontFace     frontFace = VK_FRONT_FACE_MAX_ENUM;

    scene.GetRegistry().ViewOf<WorldTransform, MeshRenderer>().Each(
        [&](Entity, const WorldTransform& world, const MeshRenderer& renderer) {
            if (renderer.model != lastHandle) {
                lastHandle = renderer.model;
                lastModel  = m_Assets.Get(renderer.model); // nullptr while loading or after release
            }
            const Model* model = lastModel;
            if (!model || renderer.meshIndex >= model->meshes.size())
                return;

            VkDeviceAddress drawAddress = 0; // allocated lazily: fully culled nodes cost nothing
            // Mirrored transforms flip the winding (glTF: negative determinant -> clockwise front faces).
            const VkFrontFace nodeFront = glm::determinant(glm::mat3(world.matrix)) < 0.0f
                                              ? VK_FRONT_FACE_CLOCKWISE
                                              : VK_FRONT_FACE_COUNTER_CLOCKWISE;

            for (const Submesh& sm : model->meshes[renderer.meshIndex].submeshes) {
                if (!frustum.Intersects(TransformAabb({sm.boundsMin, sm.boundsMax}, world.matrix))) {
                    ++m_Stats.culled;
                    continue;
                }
                if (drawAddress == 0) {
                    drawAddress = m_Renderer.PushTransient(
                        DrawData{.model        = world.matrix,
                                 .normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(world.matrix))))},
                        16);
                }
                if (model != boundModel) {
                    vkCmdBindIndexBuffer(cmd, model->indexBuffer.Handle(), 0, VK_INDEX_TYPE_UINT32);
                    boundModel = model;
                }

                const bool            doubleSided = (model->materialFlags[sm.material] & kMaterialDoubleSided) != 0;
                const VkCullModeFlags cull        = doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
                if (cull != cullMode) {
                    vkCmdSetCullMode(cmd, cull);
                    cullMode = cull;
                }
                if (nodeFront != frontFace) {
                    vkCmdSetFrontFace(cmd, nodeFront);
                    frontFace = nodeFront;
                }

                const MeshPush push{.frame         = frameAddress,
                                    .vertices      = model->vertexBuffer.Address(),
                                    .materials     = model->materialBuffer.Address(),
                                    .draw          = drawAddress,
                                    .materialIndex = sm.material};
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.firstIndex, sm.vertexOffset, 0);

                ++m_Stats.drawCalls;
                m_Stats.triangles += sm.indexCount / 3;
            }
        });
}

} // namespace Engine
