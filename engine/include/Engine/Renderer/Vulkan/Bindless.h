#pragma once
#include "Engine/Renderer/Vulkan/VulkanContext.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Engine {

// Pre-registered samplers, fixed indices (mirrored in bindless.glsl).
enum class DefaultSampler : std::uint32_t { LinearRepeat = 0, LinearClamp = 1, NearestClamp = 2, Count };

inline constexpr std::uint32_t kPushConstantSize = 128; // guaranteed minimum on every device

// One global descriptor set (set 0) shared by every pipeline:
//   binding 0: texture2D[]  binding 1: sampler[]  binding 2: image2D[] (storage)
// Buffers are accessed through buffer device addresses in push constants, not descriptors.
class BindlessRegistry {
public:
    explicit BindlessRegistry(const VulkanContext& ctx);
    ~BindlessRegistry();

    BindlessRegistry(const BindlessRegistry&)            = delete;
    BindlessRegistry& operator=(const BindlessRegistry&) = delete;

    // Add/Remove are thread-safe (asset workers register textures directly).
    [[nodiscard]] std::uint32_t AddSampledImage(VkImageView view,
                                                VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    [[nodiscard]] std::uint32_t AddStorageImage(VkImageView view);
    [[nodiscard]] std::uint32_t AddSampler(VkSampler sampler);

    // Frees the slot for reuse. The GPU may still read it: call through Renderer::DeferCall.
    void RemoveSampledImage(std::uint32_t index) { std::scoped_lock lock{m_Mutex}; m_SampledImages.Release(index); }
    void RemoveStorageImage(std::uint32_t index) { std::scoped_lock lock{m_Mutex}; m_StorageImages.Release(index); }
    void RemoveSampler(std::uint32_t index)      { std::scoped_lock lock{m_Mutex}; m_Samplers.Release(index); }

    void Bind(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint) const;

    [[nodiscard]] VkDescriptorSetLayout SetLayout()      const { return m_SetLayout; }
    [[nodiscard]] VkPipelineLayout      PipelineLayout() const { return m_PipelineLayout; }

private:
    struct SlotAllocator {
        std::uint32_t              capacity = 0;
        std::uint32_t              next     = 0;
        std::vector<std::uint32_t> freeList;
        std::uint32_t Allocate();
        void          Release(std::uint32_t index);
    };

    void WriteImage(std::uint32_t binding, VkDescriptorType type, std::uint32_t index,
                    const VkDescriptorImageInfo& info) const;

    VkDevice              m_Device         = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_SetLayout      = VK_NULL_HANDLE;
    VkDescriptorPool      m_Pool           = VK_NULL_HANDLE;
    VkDescriptorSet       m_Set            = VK_NULL_HANDLE;
    VkPipelineLayout      m_PipelineLayout = VK_NULL_HANDLE;

    // Guards the slot allocators and descriptor writes (the set is externally synchronized).
    // Binding the set while another thread writes unused slots is allowed (UPDATE_AFTER_BIND).
    std::mutex    m_Mutex;
    SlotAllocator m_SampledImages, m_Samplers, m_StorageImages;
    std::array<VkSampler, static_cast<std::size_t>(DefaultSampler::Count)> m_DefaultSamplers{};
};

} // namespace Engine
