#pragma once
#include "Engine/Renderer/Vulkan/VkCommon.h"

#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace Engine {

class Window;

struct VulkanContextDesc {
    std::string           appName          = "Engine";
    bool                  enableValidation = false;
    std::filesystem::path pipelineCache{}; // loaded at startup, saved at shutdown; empty: memory only
};

struct Queue {
    VkQueue       handle = VK_NULL_HANDLE;
    std::uint32_t family = 0;
};

// Owns instance, surface, device, queues and the VMA allocator.
class VulkanContext {
public:
    VulkanContext(const Window& window, const VulkanContextDesc& desc);
    ~VulkanContext();

    VulkanContext(const VulkanContext&)            = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    void WaitIdle() const;

    // Validation-layer errors reported so far (process-wide). Smoke tests fail on > 0.
    [[nodiscard]] static std::uint32_t ValidationErrorCount();

    [[nodiscard]] VkInstance       Instance()       const { return m_Instance.instance; }
    [[nodiscard]] VkPhysicalDevice PhysicalDevice() const { return m_PhysicalDevice.physical_device; }
    [[nodiscard]] VkDevice         Device()         const { return m_Device.device; }
    [[nodiscard]] VkSurfaceKHR     Surface()        const { return m_Surface; }
    [[nodiscard]] VmaAllocator     Allocator()      const { return m_Allocator; }

    [[nodiscard]] const Queue& GraphicsQueue() const { return m_Graphics; }
    [[nodiscard]] const Queue& PresentQueue()  const { return m_Present; }
    [[nodiscard]] const Queue& TransferQueue() const { return m_Transfer; } // falls back to graphics

    [[nodiscard]] const std::string& DeviceName() const { return m_PhysicalDevice.name; }
    [[nodiscard]] const VkPhysicalDeviceProperties& Properties() const { return m_PhysicalDevice.properties; }
    // Optional features, enabled when present.
    [[nodiscard]] bool SupportsBC() const { return m_TextureCompressionBC; } // BC1-7 sampling
    // VK_EXT_swapchain_maintenance1: present fences -> swapchain recreation without a device wait.
    [[nodiscard]] bool SupportsSwapchainMaintenance() const { return m_SwapchainMaintenance; }

    // Shared by every pipeline creation (see Pipeline.h), persisted in VulkanContextDesc::pipelineCache.
    [[nodiscard]] VkPipelineCache PipelineCache() const { return m_PipelineCache; }

    // Keeps the original vkb objects accessible for the swapchain builder (Phase 2).
    [[nodiscard]] const vkb::Device& VkbDevice() const { return m_Device; }

private:
    void CreateInstance(const VulkanContextDesc& desc);
    void SelectPhysicalDevice();
    void CreateDevice();
    void CreateAllocator();
    void CreatePipelineCache();
    void SavePipelineCache() const;
    void Shutdown() noexcept;

    vkb::Instance       m_Instance{};
    vkb::PhysicalDevice m_PhysicalDevice{};
    vkb::Device         m_Device{};
    VkSurfaceKHR        m_Surface   = VK_NULL_HANDLE;
    VmaAllocator        m_Allocator = VK_NULL_HANDLE;
    VkPipelineCache     m_PipelineCache = VK_NULL_HANDLE;
    std::filesystem::path m_PipelineCachePath;

    Queue m_Graphics{}, m_Present{}, m_Transfer{};
    bool  m_TextureCompressionBC = false;
    bool  m_SurfaceMaintenance   = false; // instance: VK_EXT_surface_maintenance1
    bool  m_SwapchainMaintenance = false; // device: VK_EXT_swapchain_maintenance1 (feature enabled)
};

} // namespace Engine
