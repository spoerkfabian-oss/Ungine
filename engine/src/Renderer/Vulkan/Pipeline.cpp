#include "Engine/Renderer/Vulkan/Pipeline.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"

#include <cstdint>
#include <format>
#include <fstream>
#include <stdexcept>

namespace Engine {

VkShaderModule LoadShaderModule(VkDevice device, const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error(std::format("Cannot open shader '{}'", path.string()));

    const auto size = static_cast<std::size_t>(file.tellg());
    if (size == 0 || size % sizeof(std::uint32_t) != 0)
        throw std::runtime_error(std::format("Invalid SPIR-V file '{}'", path.string()));

    std::vector<std::uint32_t> code(size / sizeof(std::uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode    = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::SetShaders(std::filesystem::path vertex, std::filesystem::path fragment)
{
    m_VertexShader   = std::move(vertex);
    m_FragmentShader = std::move(fragment);
    return *this;
}
GraphicsPipelineBuilder& GraphicsPipelineBuilder::SetTopology(VkPrimitiveTopology t) { m_Topology = t; return *this; }
GraphicsPipelineBuilder& GraphicsPipelineBuilder::SetPolygonMode(VkPolygonMode m) { m_PolygonMode = m; return *this; }
GraphicsPipelineBuilder& GraphicsPipelineBuilder::SetCulling(VkCullModeFlags cull, VkFrontFace front)
{
    m_CullMode  = cull;
    m_FrontFace = front;
    return *this;
}
GraphicsPipelineBuilder& GraphicsPipelineBuilder::SetDepth(bool test, bool write, VkCompareOp op)
{
    m_DepthTest    = test;
    m_DepthWrite   = write;
    m_DepthCompare = op;
    return *this;
}
GraphicsPipelineBuilder& GraphicsPipelineBuilder::SetDepthClamp(bool e) { m_DepthClamp = e; return *this; }
GraphicsPipelineBuilder& GraphicsPipelineBuilder::SetDynamicDepthBias(bool e) { m_DynamicDepthBias = e; return *this; }
GraphicsPipelineBuilder& GraphicsPipelineBuilder::SetDynamicCulling(bool e) { m_DynamicCulling = e; return *this; }
GraphicsPipelineBuilder& GraphicsPipelineBuilder::SetBlend(BlendMode m) { m_Blend = m; return *this; }
GraphicsPipelineBuilder& GraphicsPipelineBuilder::AddColorAttachment(VkFormat f) { m_ColorFormats.push_back(f); return *this; }
GraphicsPipelineBuilder& GraphicsPipelineBuilder::SetDepthFormat(VkFormat f) { m_DepthFormat = f; return *this; }
GraphicsPipelineBuilder& GraphicsPipelineBuilder::SetDebugName(std::string n) { m_DebugName = std::move(n); return *this; }

namespace {
VkPipelineColorBlendAttachmentState MakeBlendState(BlendMode mode)
{
    VkPipelineColorBlendAttachmentState s{};
    s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                       VK_COLOR_COMPONENT_A_BIT;
    if (mode == BlendMode::Opaque)
        return s;

    s.blendEnable         = VK_TRUE;
    s.colorBlendOp        = VK_BLEND_OP_ADD;
    s.alphaBlendOp        = VK_BLEND_OP_ADD;
    s.srcColorBlendFactor = mode == BlendMode::Alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
    s.dstColorBlendFactor = mode == BlendMode::Alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
    s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    s.dstAlphaBlendFactor = mode == BlendMode::Alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
    return s;
}

struct ShaderModuleGuard {
    VkDevice       device = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    ~ShaderModuleGuard()
    {
        if (module)
            vkDestroyShaderModule(device, module, nullptr);
    }
};
} // namespace

Pipeline CreateComputePipeline(VkDevice device, VkPipelineLayout layout, const std::filesystem::path& shader,
                               const char* debugName)
{
    const ShaderModuleGuard cs{device, LoadShaderModule(device, shader)};

    VkComputePipelineCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = cs.module;
    info.stage.pName  = "main";
    info.layout       = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
    if (debugName)
        Engine::SetDebugName(device, VK_OBJECT_TYPE_PIPELINE, pipeline, debugName);
    return Pipeline{device, pipeline};
}

Pipeline GraphicsPipelineBuilder::Build(VkDevice device, VkPipelineLayout layout) const
{
    if (m_VertexShader.empty())
        throw std::runtime_error("GraphicsPipelineBuilder: no vertex shader set");

    // Modules are only needed until pipeline creation.
    const ShaderModuleGuard vs{device, LoadShaderModule(device, m_VertexShader)};
    const ShaderModuleGuard fs{device, m_FragmentShader.empty() ? VK_NULL_HANDLE
                                                                : LoadShaderModule(device, m_FragmentShader)};

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.pName  = "main";
    stage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vs.module;
    stages.push_back(stage);
    if (fs.module) { // depth-only passes (shadows) have no fragment shader
        stage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stage.module = fs.module;
        stages.push_back(stage);
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = m_Topology;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType            = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable = m_DepthClamp ? VK_TRUE : VK_FALSE;
    raster.polygonMode      = m_PolygonMode;
    raster.cullMode         = m_CullMode;
    raster.frontFace        = m_FrontFace;
    raster.depthBiasEnable  = m_DynamicDepthBias ? VK_TRUE : VK_FALSE;
    raster.lineWidth        = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = m_DepthTest ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = m_DepthWrite ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp   = m_DepthTest ? m_DepthCompare : VK_COMPARE_OP_ALWAYS;
    depth.maxDepthBounds   = 1.0f;

    const std::vector<VkPipelineColorBlendAttachmentState> blendStates(m_ColorFormats.size(), MakeBlendState(m_Blend));
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = static_cast<std::uint32_t>(blendStates.size());
    blend.pAttachments    = blendStates.data();

    std::vector<VkDynamicState> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    if (m_DynamicDepthBias)
        dynamicStates.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
    if (m_DynamicCulling) {
        dynamicStates.push_back(VK_DYNAMIC_STATE_CULL_MODE);
        dynamicStates.push_back(VK_DYNAMIC_STATE_FRONT_FACE);
    }
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates    = dynamicStates.data();

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount    = static_cast<std::uint32_t>(m_ColorFormats.size());
    rendering.pColorAttachmentFormats = m_ColorFormats.data();
    rendering.depthAttachmentFormat   = m_DepthFormat;
    rendering.stencilAttachmentFormat = HasStencilComponent(m_DepthFormat) ? m_DepthFormat : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &rendering;
    info.stageCount          = static_cast<std::uint32_t>(stages.size());
    info.pStages             = stages.data();
    info.pVertexInputState   = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState      = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &multisample;
    info.pDepthStencilState  = &depth;
    info.pColorBlendState    = &blend;
    info.pDynamicState       = &dynamic;
    info.layout              = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
    if (!m_DebugName.empty())
        Engine::SetDebugName(device, VK_OBJECT_TYPE_PIPELINE, pipeline, m_DebugName.c_str());
    return Pipeline{device, pipeline};
}

} // namespace Engine
