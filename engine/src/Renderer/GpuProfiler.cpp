#include "Engine/Renderer/GpuProfiler.h"

#include <array>

namespace Engine {

GpuProfiler::GpuProfiler(const VulkanContext& ctx, std::uint32_t framesInFlight)
    : m_Device(ctx.Device())
{
    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.PhysicalDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.PhysicalDevice(), &familyCount, families.data());

    const std::uint32_t validBits = families[ctx.GraphicsQueue().family].timestampValidBits;
    m_Supported = validBits > 0 && ctx.Properties().limits.timestampPeriod > 0.0f;
    if (!m_Supported) {
        ENGINE_WARN("GPU timestamps not supported on the graphics queue; profiler disabled");
        return;
    }
    m_Period    = ctx.Properties().limits.timestampPeriod;
    m_ValidMask = validBits >= 64 ? ~0ull : (1ull << validBits) - 1;

    m_Slots.resize(framesInFlight);
    for (Slot& slot : m_Slots) {
        VkQueryPoolCreateInfo info{};
        info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = 2 * kMaxScopes;
        VK_CHECK(vkCreateQueryPool(m_Device, &info, nullptr, &slot.pool));
        vkResetQueryPool(m_Device, slot.pool, 0, info.queryCount); // hostQueryReset (Vulkan 1.2)
    }
}

GpuProfiler::~GpuProfiler()
{
    for (Slot& slot : m_Slots)
        vkDestroyQueryPool(m_Device, slot.pool, nullptr);
}

void GpuProfiler::BeginFrame(std::uint32_t frameIndex)
{
    if (!m_Supported)
        return;
    Slot& slot = m_Slots[frameIndex];

    if (!slot.scopes.empty()) {
        // The fence of this slot was waited on: every query is available.
        std::array<std::uint64_t, 2 * kMaxScopes> ticks{};
        const auto count  = static_cast<std::uint32_t>(2 * slot.scopes.size());
        const VkResult result =
            vkGetQueryPoolResults(m_Device, slot.pool, 0, count, sizeof(ticks), ticks.data(), sizeof(std::uint64_t),
                                  VK_QUERY_RESULT_64_BIT);
        if (result == VK_SUCCESS) {
            for (std::size_t i = 0; i < slot.scopes.size(); ++i) {
                const std::uint64_t begin = ticks[2 * i] & m_ValidMask;
                const std::uint64_t end   = ticks[2 * i + 1] & m_ValidMask;
                slot.scopes[i].milliseconds = end >= begin ? static_cast<double>(end - begin) * m_Period * 1e-6 : 0.0;
            }
            m_Results = slot.scopes;
        }
        vkResetQueryPool(m_Device, slot.pool, 0, count);
    }
    slot.scopes.clear();
    slot.open.clear();
    m_Current = &slot;
}

std::uint32_t GpuProfiler::Begin(VkCommandBuffer cmd, const char* name)
{
    if (!m_Current || m_Current->scopes.size() >= kMaxScopes)
        return ~0u;
    const auto id = static_cast<std::uint32_t>(m_Current->scopes.size());
    m_Current->scopes.push_back({.name = name, .milliseconds = 0.0,
                                 .depth = static_cast<std::uint32_t>(m_Current->open.size())});
    m_Current->open.push_back(id);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_Current->pool, 2 * id);
    return id;
}

void GpuProfiler::End(VkCommandBuffer cmd, std::uint32_t scope)
{
    if (!m_Current || scope == ~0u)
        return;
    // After everything recorded before this point has finished.
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_Current->pool, 2 * scope + 1);
    if (!m_Current->open.empty() && m_Current->open.back() == scope)
        m_Current->open.pop_back();
}

} // namespace Engine
