#pragma once
#include "Engine/Renderer/Vulkan/VulkanContext.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Engine {

struct GpuTiming {
    const char*   name         = nullptr; // string literal passed to Begin
    double        milliseconds = 0.0;
    std::uint32_t depth        = 0;       // nesting level (0 = top)
};

// Timestamp queries per frame slot. Results of a slot are read when the Renderer comes back to
// it (its fence has been waited on), so they are kFramesInFlight frames old and never stall.
// Main thread only.
class GpuProfiler {
public:
    static constexpr std::uint32_t kMaxScopes = 48;

    GpuProfiler(const VulkanContext& ctx, std::uint32_t framesInFlight);
    ~GpuProfiler();

    GpuProfiler(const GpuProfiler&)            = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    // Renderer::BeginFrame, after the slot's fence wait and before recording: collect + reset.
    void BeginFrame(std::uint32_t frameIndex);

    // Returns a scope id for End(); ~0u when timestamps are unsupported or the frame is full.
    [[nodiscard]] std::uint32_t Begin(VkCommandBuffer cmd, const char* name);
    void End(VkCommandBuffer cmd, std::uint32_t scope);

    [[nodiscard]] std::span<const GpuTiming> Results() const { return m_Results; } // in Begin order
    [[nodiscard]] bool Supported() const { return m_Supported; }

private:
    struct Slot {
        VkQueryPool                 pool = VK_NULL_HANDLE;
        std::vector<GpuTiming>      scopes; // name + depth; timings filled on collect
        std::vector<std::uint32_t>  open;   // stack of open scope ids
    };

    VkDevice          m_Device     = VK_NULL_HANDLE;
    bool              m_Supported  = false;
    double            m_Period     = 1.0; // nanoseconds per tick
    std::uint64_t     m_ValidMask  = ~0ull;
    std::vector<Slot> m_Slots;
    Slot*             m_Current    = nullptr;
    std::vector<GpuTiming> m_Results;
};

// RAII scope; tolerates a null profiler.
class GpuScope {
public:
    GpuScope(GpuProfiler* profiler, VkCommandBuffer cmd, const char* name)
        : m_Profiler(profiler), m_Cmd(cmd), m_Id(profiler ? profiler->Begin(cmd, name) : ~0u)
    {
    }
    ~GpuScope()
    {
        if (m_Profiler)
            m_Profiler->End(m_Cmd, m_Id);
    }
    GpuScope(const GpuScope&)            = delete;
    GpuScope& operator=(const GpuScope&) = delete;

private:
    GpuProfiler*    m_Profiler;
    VkCommandBuffer m_Cmd;
    std::uint32_t   m_Id;
};

} // namespace Engine
