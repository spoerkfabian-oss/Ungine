#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Assets/Model.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Scene.h"

namespace Engine {

namespace {
struct FrameUniforms { // mirrors FrameData in mesh_common.glsl
    glm::mat4 viewProj;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec4 cameraPosition;
    glm::vec4 sunDirection;
    glm::vec4 sunColor;
    glm::vec4 ambient;
};

struct MeshPush { // mirrors MeshPush in mesh_common.glsl
    glm::mat4       model;
    VkDeviceAddress frame;
    VkDeviceAddress vertices;
    VkDeviceAddress materials;
    std::uint32_t   materialIndex;
};
static_assert(sizeof(MeshPush) <= kPushConstantSize);
} // namespace

SceneRenderer::SceneRenderer(Renderer& renderer, const VulkanContext& ctx)
    : m_Renderer(renderer)
{
    GraphicsPipelineBuilder builder;
    builder.SetShaders(ShaderPath("mesh.vert.spv"), ShaderPath("mesh.frag.spv"))
           .AddColorAttachment(renderer.GetSwapchain().Format())
           .SetDepthFormat(kDepthFormat)
           .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL); // reverse-Z

    // glTF is CCW; the flipped viewport preserves GL-style winding.
    m_Opaque = builder.SetCulling(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                      .SetDebugName("MeshOpaque")
                      .Build(ctx.Device(), renderer.GetBindless().PipelineLayout());
    m_DoubleSided = builder.SetCulling(VK_CULL_MODE_NONE)
                           .SetDebugName("MeshDoubleSided")
                           .Build(ctx.Device(), renderer.GetBindless().PipelineLayout());
}

void SceneRenderer::Render(const FrameContext& frame, Scene& scene, const CameraData& camera)
{
    m_Stats = {};
    const VkCommandBuffer cmd      = frame.cmd;
    const auto&           bindless = m_Renderer.GetBindless();

    const FrameUniforms uniforms{
        .viewProj       = camera.projection * camera.view,
        .view           = camera.view,
        .proj           = camera.projection,
        .cameraPosition = glm::vec4(camera.position, 1.0f),
        .sunDirection   = glm::vec4(glm::normalize(lighting.sunDirection), 0.0f),
        .sunColor       = glm::vec4(lighting.sunColor * lighting.sunIntensity, 0.0f),
        .ambient        = glm::vec4(lighting.ambient, 0.0f)};
    const VkDeviceAddress frameAddress = m_Renderer.PushTransient(uniforms);

    VkRenderingAttachmentInfo color{};
    color.sType            = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView        = frame.view;
    color.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{lighting.clearColor.r, lighting.clearColor.g, lighting.clearColor.b, 1.0f}};

    VkRenderingAttachmentInfo depth{};
    depth.sType                   = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView               = frame.depthView;
    depth.imageLayout             = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp                  = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp                 = VK_ATTACHMENT_STORE_OP_STORE; // kept for SSAO later
    depth.clearValue.depthStencil = {0.0f, 0}; // reverse-Z: far = 0

    VkRenderingInfo info{};
    info.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea           = {{0, 0}, frame.extent};
    info.layerCount           = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments    = &color;
    info.pDepthAttachment     = &depth;

    vkCmdBeginRendering(cmd, &info);
    bindless.Bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    SetViewportScissor(cmd, frame.extent);

    VkPipeline   boundPipeline = VK_NULL_HANDLE;
    const Model* boundModel    = nullptr;

    scene.GetRegistry().ViewOf<WorldTransform, MeshRenderer>().Each(
        [&](Entity, const WorldTransform& world, const MeshRenderer& renderer) {
            const Model* model = renderer.model.get();
            if (!model || renderer.meshIndex >= model->meshes.size())
                return;
            if (model != boundModel) {
                vkCmdBindIndexBuffer(cmd, model->indexBuffer.Handle(), 0, VK_INDEX_TYPE_UINT32);
                boundModel = model;
            }

            for (const Submesh& sm : model->meshes[renderer.meshIndex].submeshes) {
                const bool       doubleSided = (model->materialFlags[sm.material] & kMaterialDoubleSided) != 0;
                const VkPipeline pipeline    = doubleSided ? m_DoubleSided.Handle() : m_Opaque.Handle();
                if (pipeline != boundPipeline) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    boundPipeline = pipeline;
                }

                const MeshPush push{.model         = world.matrix,
                                    .frame         = frameAddress,
                                    .vertices      = model->vertexBuffer.Address(),
                                    .materials     = model->materialBuffer.Address(),
                                    .materialIndex = sm.material};
                vkCmdPushConstants(cmd, bindless.PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.firstIndex, sm.vertexOffset, 0);

                ++m_Stats.drawCalls;
                m_Stats.triangles += sm.indexCount / 3;
            }
        });

    vkCmdEndRendering(cmd);
}

} // namespace Engine
