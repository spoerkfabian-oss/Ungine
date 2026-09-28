#include "Engine/Renderer/Vulkan/Bindless.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace Engine {

namespace {
constexpr std::uint32_t kBindingSampledImages = 0;
constexpr std::uint32_t kBindingSamplers      = 1;
constexpr std::uint32_t kBindingStorageImages = 2;

// Desired sizes; clamped to device limits at runtime.
constexpr std::uint32_t kMaxSampledImages = 16384;
constexpr std::uint32_t kMaxSamplers      = 64;
constexpr std::uint32_t kMaxStorageImages = 4096;

VkSampler CreateSampler(VkDevice device, VkFilter filter, VkSamplerAddressMode address, float maxAnisotropy)
{
    VkSamplerCreateInfo info{};
    info.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter        = filter;
    info.minFilter        = filter;
    info.mipmapMode       = filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU     = address;
    info.addressModeV     = address;
    info.addressModeW     = address;
    info.anisotropyEnable = maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy    = maxAnisotropy;
    info.maxLod           = VK_LOD_CLAMP_NONE;
    VkSampler sampler     = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(device, &info, nullptr, &sampler));
    return sampler;
}
} // namespace

std::uint32_t BindlessRegistry::SlotAllocator::Allocate()
{
    if (!freeList.empty()) {
        const std::uint32_t index = freeList.back();
        freeList.pop_back();
        return index;
    }
    if (next >= capacity)
        throw std::runtime_error("Bindless descriptor table is full");
    return next++;
}

void BindlessRegistry::SlotAllocator::Release(std::uint32_t index)
{
    assert(index < next && "Releasing a slot that was never allocated");
    freeList.push_back(index);
}

BindlessRegistry::BindlessRegistry(const VulkanContext& ctx)
    : m_Device(ctx.Device())
{
    VkPhysicalDeviceVulkan12Properties p12{};
    p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &p12;
    vkGetPhysicalDeviceProperties2(ctx.PhysicalDevice(), &props);

    m_SampledImages.capacity = std::min({kMaxSampledImages, p12.maxDescriptorSetUpdateAfterBindSampledImages,
                                         p12.maxPerStageDescriptorUpdateAfterBindSampledImages});
    m_Samplers.capacity      = std::min({kMaxSamplers, p12.maxDescriptorSetUpdateAfterBindSamplers,
                                         p12.maxPerStageDescriptorUpdateAfterBindSamplers});
    m_StorageImages.capacity = std::min({kMaxStorageImages, p12.maxDescriptorSetUpdateAfterBindStorageImages,
                                         p12.maxPerStageDescriptorUpdateAfterBindStorageImages});

    // --- Layout ---
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
        {kBindingSampledImages, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, m_SampledImages.capacity, VK_SHADER_STAGE_ALL, nullptr},
        {kBindingSamplers, VK_DESCRIPTOR_TYPE_SAMPLER, m_Samplers.capacity, VK_SHADER_STAGE_ALL, nullptr},
        {kBindingStorageImages, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, m_StorageImages.capacity, VK_SHADER_STAGE_ALL, nullptr},
    }};
    constexpr VkDescriptorBindingFlags kFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                                VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    const std::array<VkDescriptorBindingFlags, 3> bindingFlags{kFlags, kFlags, kFlags};

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount  = static_cast<std::uint32_t>(bindingFlags.size());
    flagsInfo.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext        = &flagsInfo;
    layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_SetLayout));

    // --- Pool + the one set ---
    const std::array<VkDescriptorPoolSize, 3> poolSizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, m_SampledImages.capacity},
        {VK_DESCRIPTOR_TYPE_SAMPLER, m_Samplers.capacity},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, m_StorageImages.capacity},
    }};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_Pool));

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_Pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_SetLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_Device, &allocInfo, &m_Set));

    // --- Global pipeline layout: set 0 + 128 B push constants for all stages ---
    const VkPushConstantRange pushRange{VK_SHADER_STAGE_ALL, 0, kPushConstantSize};
    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &m_SetLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(m_Device, &plInfo, nullptr, &m_PipelineLayout));

    // --- Default samplers at fixed indices ---
    const float aniso = std::min(16.0f, ctx.Properties().limits.maxSamplerAnisotropy);
    m_DefaultSamplers = {CreateSampler(m_Device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, aniso),
                         CreateSampler(m_Device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, aniso),
                         CreateSampler(m_Device, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f)};
    for (std::uint32_t i = 0; i < m_DefaultSamplers.size(); ++i) {
        [[maybe_unused]] const std::uint32_t index = AddSampler(m_DefaultSamplers[i]);
        assert(index == i && "Default samplers must occupy the first slots");
    }

    ENGINE_INFO("Bindless table: {} textures, {} samplers, {} storage images", m_SampledImages.capacity,
                m_Samplers.capacity, m_StorageImages.capacity);
}

BindlessRegistry::~BindlessRegistry()
{
    for (VkSampler s : m_DefaultSamplers)
        vkDestroySampler(m_Device, s, nullptr);
    vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
    vkDestroyDescriptorPool(m_Device, m_Pool, nullptr); // frees m_Set
    vkDestroyDescriptorSetLayout(m_Device, m_SetLayout, nullptr);
}

std::uint32_t BindlessRegistry::AddSampledImage(VkImageView view, VkImageLayout layout)
{
    const std::uint32_t index = m_SampledImages.Allocate();
    WriteImage(kBindingSampledImages, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, index, {VK_NULL_HANDLE, view, layout});
    return index;
}

std::uint32_t BindlessRegistry::AddStorageImage(VkImageView view)
{
    const std::uint32_t index = m_StorageImages.Allocate();
    WriteImage(kBindingStorageImages, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, index,
               {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL});
    return index;
}

std::uint32_t BindlessRegistry::AddSampler(VkSampler sampler)
{
    const std::uint32_t index = m_Samplers.Allocate();
    WriteImage(kBindingSamplers, VK_DESCRIPTOR_TYPE_SAMPLER, index,
               {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    return index;
}

void BindlessRegistry::WriteImage(std::uint32_t binding, VkDescriptorType type, std::uint32_t index,
                                  const VkDescriptorImageInfo& info) const
{
    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_Set;
    write.dstBinding      = binding;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType  = type;
    write.pImageInfo      = &info;
    vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

void BindlessRegistry::Bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) const
{
    vkCmdBindDescriptorSets(cmd, bindPoint, m_PipelineLayout, 0, 1, &m_Set, 0, nullptr);
}

} // namespace Engine
