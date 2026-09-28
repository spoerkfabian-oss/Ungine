#pragma once
#include "Engine/Renderer/Vulkan/VkCommon.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

// Compiled SPIR-V lives next to the build (see cmake/Shaders.cmake).
[[nodiscard]] inline std::filesystem::path ShaderPath(std::string_view spvName)
{
    return std::filesystem::path{ENGINE_SHADER_DIR} / spvName;
}

[[nodiscard]] VkShaderModule LoadShaderModule(VkDevice device, const std::filesystem::path& path);

class Pipeline {
public:
    Pipeline() = default;
    Pipeline(VkDevice device, VkPipeline pipeline) : m_Device(device), m_Pipeline(pipeline) {}
    ~Pipeline() { Release(); }

    Pipeline(Pipeline&& o) noexcept
        : m_Device(o.m_Device), m_Pipeline(std::exchange(o.m_Pipeline, VK_NULL_HANDLE)) {}
    Pipeline& operator=(Pipeline&& o) noexcept
    {
        if (this != &o) {
            Release();
            m_Device   = o.m_Device;
            m_Pipeline = std::exchange(o.m_Pipeline, VK_NULL_HANDLE);
        }
        return *this;
    }
    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] VkPipeline Handle() const { return m_Pipeline; }
    [[nodiscard]] explicit operator bool() const { return m_Pipeline != VK_NULL_HANDLE; }

private:
    void Release() noexcept
    {
        if (m_Pipeline)
            vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
        m_Pipeline = VK_NULL_HANDLE;
    }

    VkDevice   m_Device   = VK_NULL_HANDLE;
    VkPipeline m_Pipeline = VK_NULL_HANDLE;
};

// Compute pipeline on the shared bindless layout.
[[nodiscard]] Pipeline CreateComputePipeline(VkDevice device, VkPipelineLayout layout,
                                             const std::filesystem::path& shader, const char* debugName = nullptr);

enum class BlendMode { Opaque, Alpha, Additive };

// Dynamic-rendering graphics pipeline. No vertex input state: vertices are pulled via BDA.
// Viewport/scissor are always dynamic. Defaults: triangle list, no culling, no depth, opaque.
class GraphicsPipelineBuilder {
public:
    GraphicsPipelineBuilder& SetShaders(std::filesystem::path vertex, std::filesystem::path fragment = {});
    GraphicsPipelineBuilder& SetTopology(VkPrimitiveTopology topology);
    GraphicsPipelineBuilder& SetPolygonMode(VkPolygonMode mode);
    GraphicsPipelineBuilder& SetCulling(VkCullModeFlags cull, VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE);
    // Cull mode + front face set per draw (vkCmdSetCullMode/FrontFace, core 1.3).
    GraphicsPipelineBuilder& SetDynamicCulling(bool enable);
    // Reverse-Z is the engine convention: clear depth to 0, compare GREATER_OR_EQUAL.
    GraphicsPipelineBuilder& SetDepth(bool test, bool write, VkCompareOp op = VK_COMPARE_OP_GREATER_OR_EQUAL);
    GraphicsPipelineBuilder& SetDepthClamp(bool enable);       // shadow maps
    GraphicsPipelineBuilder& SetDynamicDepthBias(bool enable); // shadow maps
    GraphicsPipelineBuilder& SetBlend(BlendMode mode);
    GraphicsPipelineBuilder& AddColorAttachment(VkFormat format);
    GraphicsPipelineBuilder& SetDepthFormat(VkFormat format);
    GraphicsPipelineBuilder& SetDebugName(std::string name);

    [[nodiscard]] Pipeline Build(VkDevice device, VkPipelineLayout layout) const;

private:
    std::filesystem::path m_VertexShader, m_FragmentShader;
    VkPrimitiveTopology   m_Topology         = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode         m_PolygonMode      = VK_POLYGON_MODE_FILL;
    VkCullModeFlags       m_CullMode         = VK_CULL_MODE_NONE;
    VkFrontFace           m_FrontFace        = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool                  m_DepthTest        = false;
    bool                  m_DepthWrite       = false;
    VkCompareOp           m_DepthCompare     = VK_COMPARE_OP_GREATER_OR_EQUAL;
    bool                  m_DepthClamp       = false;
    bool                  m_DynamicDepthBias = false;
    bool                  m_DynamicCulling   = false;
    BlendMode             m_Blend            = BlendMode::Opaque;
    std::vector<VkFormat> m_ColorFormats;
    VkFormat              m_DepthFormat      = VK_FORMAT_UNDEFINED;
    std::string           m_DebugName;
};

} // namespace Engine
