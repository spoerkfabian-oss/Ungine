#include "Engine/Renderer/Vulkan/VulkanContext.h"
#include "Engine/Core/Window.h"
#include "Renderer/Vulkan/VkbUtil.h"

#include <atomic>

namespace Engine {

namespace {

std::atomic<std::uint32_t> g_ValidationErrors{0}; // callback may run on any thread

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        g_ValidationErrors.fetch_add(1, std::memory_order_relaxed);
        ENGINE_ERROR("[Vulkan] {}", data->pMessage);
    }
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        ENGINE_WARN("[Vulkan] {}", data->pMessage);
    else
        ENGINE_TRACE("[Vulkan] {}", data->pMessage);
    return VK_FALSE;
}

} // namespace

VulkanContext::VulkanContext(const Window& window, const VulkanContextDesc& desc)
{
    try {
        VK_CHECK(volkInitialize());
        CreateInstance(desc);
        m_Surface = window.CreateSurface(m_Instance.instance);
        SelectPhysicalDevice();
        CreateDevice();
        CreateAllocator();
    } catch (...) {
        Shutdown(); // destructor won't run if the constructor throws
        throw;
    }

    ENGINE_INFO("Vulkan device: {} (API {}.{}.{})", DeviceName(),
                VK_API_VERSION_MAJOR(Properties().apiVersion),
                VK_API_VERSION_MINOR(Properties().apiVersion),
                VK_API_VERSION_PATCH(Properties().apiVersion));
}

VulkanContext::~VulkanContext() { Shutdown(); }

void VulkanContext::CreateInstance(const VulkanContextDesc& desc)
{
    vkb::InstanceBuilder builder{vkGetInstanceProcAddr};
    builder.set_app_name(desc.appName.c_str())
           .set_engine_name("Engine")
           .require_api_version(1, 3, 0);

    if (desc.enableValidation) {
        builder.request_validation_layers(true)
               .set_debug_callback(DebugCallback)
               .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
    }

    m_Instance = Expect(builder.build(), "Instance creation failed");
    volkLoadInstanceOnly(m_Instance.instance);
}

void VulkanContext::SelectPhysicalDevice()
{
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE; // no VkRenderPass/VkFramebuffer objects
    f13.synchronization2 = VK_TRUE; // vkCmdPipelineBarrier2, vkQueueSubmit2

    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType                                        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.bufferDeviceAddress                          = VK_TRUE;
    f12.descriptorIndexing                           = VK_TRUE; // bindless textures
    f12.runtimeDescriptorArray                       = VK_TRUE;
    f12.descriptorBindingPartiallyBound              = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount     = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending    = VK_TRUE; // rewrite free slots while frames are in flight
    f12.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
    f12.shaderStorageImageArrayNonUniformIndexing    = VK_TRUE;
    f12.timelineSemaphore                            = VK_TRUE; // async transfer sync
    f12.hostQueryReset                               = VK_TRUE; // GPU timers for ImGui stats

    VkPhysicalDeviceFeatures f10{};
    f10.samplerAnisotropy         = VK_TRUE;
    f10.depthClamp                = VK_TRUE; // CSM: avoid near-plane clipping of casters
    f10.fillModeNonSolid          = VK_TRUE; // wireframe debug view
    f10.multiDrawIndirect         = VK_TRUE; // GPU-driven culling later
    f10.drawIndirectFirstInstance = VK_TRUE;

    vkb::PhysicalDeviceSelector selector{m_Instance};
    m_PhysicalDevice = Expect(selector.set_minimum_version(1, 3)
                                  .set_surface(m_Surface)
                                  .set_required_features(f10)
                                  .set_required_features_12(f12)
                                  .set_required_features_13(f13)
                                  .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
                                  .select(),
                              "No suitable GPU found");
}

void VulkanContext::CreateDevice()
{
    m_Device = Expect(vkb::DeviceBuilder{m_PhysicalDevice}.build(), "Device creation failed");
    volkLoadDevice(m_Device.device); // direct device dispatch, skips loader trampoline

    m_Graphics = {Expect(m_Device.get_queue(vkb::QueueType::graphics), "Graphics queue"),
                  Expect(m_Device.get_queue_index(vkb::QueueType::graphics), "Graphics queue index")};
    m_Present  = {Expect(m_Device.get_queue(vkb::QueueType::present), "Present queue"),
                  Expect(m_Device.get_queue_index(vkb::QueueType::present), "Present queue index")};

    if (auto q = m_Device.get_dedicated_queue(vkb::QueueType::transfer)) {
        m_Transfer = {q.value(), m_Device.get_dedicated_queue_index(vkb::QueueType::transfer).value()};
    } else {
        m_Transfer = m_Graphics;
        ENGINE_WARN("No dedicated transfer queue; async uploads share the graphics queue");
    }
}

void VulkanContext::CreateAllocator()
{
    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.instance         = m_Instance.instance;
    info.physicalDevice   = m_PhysicalDevice.physical_device;
    info.device           = m_Device.device;
    info.pVulkanFunctions = &fns;

    VK_CHECK(vmaCreateAllocator(&info, &m_Allocator));
}

std::uint32_t VulkanContext::ValidationErrorCount()
{
    return g_ValidationErrors.load(std::memory_order_relaxed);
}

void VulkanContext::WaitIdle() const
{
    if (m_Device.device)
        VK_CHECK(vkDeviceWaitIdle(m_Device.device));
}

void VulkanContext::Shutdown() noexcept
{
    if (m_Allocator) {
        vmaDestroyAllocator(m_Allocator);
        m_Allocator = VK_NULL_HANDLE;
    }
    if (m_Device.device) {
        vkb::destroy_device(m_Device);
        m_Device = {};
    }
    if (m_Surface) {
        vkDestroySurfaceKHR(m_Instance.instance, m_Surface, nullptr);
        m_Surface = VK_NULL_HANDLE;
    }
    if (m_Instance.instance) {
        vkb::destroy_instance(m_Instance); // also destroys the debug messenger
        m_Instance = {};
    }
}

} // namespace Engine
