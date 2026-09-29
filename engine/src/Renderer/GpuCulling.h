#pragma once
#include "GpuScene.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/Buffer.h"
#include "Engine/Renderer/Vulkan/Pipeline.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Engine {

// Mirrors CullView in gpu_cull.comp.
struct GpuCullView {
    glm::mat4                viewProj{1.0f};
    std::array<glm::vec4, 6> planes{}; // dot(xyz, p) + w >= 0 inside
    glm::vec4                sphere{0.0f};
    std::uint32_t            listBase    = 0;
    std::uint32_t            counterBase = 0;
    std::uint32_t            commandBase = 0;
    std::uint32_t            flags       = 0;
};
static_assert(sizeof(GpuCullView) == 192);

inline constexpr std::uint32_t kCullViewShadow      = 1;
inline constexpr std::uint32_t kCullViewCameraEarly = 2;
inline constexpr std::uint32_t kCullViewCameraLate  = 4;
inline constexpr std::uint32_t kCullViewOcclusion   = 8;
inline constexpr std::uint32_t kCullViewSphere      = 16;

// View slots of a frame: camera early + late first, then the shadow views.
inline constexpr std::uint32_t kCameraEarlyView = 0;
inline constexpr std::uint32_t kCameraLateView  = 1;

// Counters of one frame, read back kFramesInFlight frames later (mirrors STAT_* in gpu_cull.comp).
struct GpuCullStats {
    std::uint32_t tested    = 0; // camera: draw records (instance x submesh) tested
    std::uint32_t frustum   = 0; // camera: outside the frustum
    std::uint32_t occluded  = 0; // camera: behind the Hi-Z
    std::uint32_t early     = 0; // camera: drawn by the early pass (visible last frame)
    std::uint32_t late      = 0; // camera: newly visible, drawn by the late pass
    std::uint32_t shadow    = 0; // shadow views: draws emitted
    std::uint32_t commands  = 0; // indirect commands, all views
    std::uint32_t triangles = 0; // camera
};

// GPU-driven culling of a GpuScene: per-view visible lists, instance batching and indirect
// commands (vkCmdDrawIndexedIndirectCount per bucket), two-phase Hi-Z occlusion for the camera.
// All buffers are shared by the frames in flight (ordered by barriers). Main thread only.
class GpuCulling {
public:
    explicit GpuCulling(Renderer& renderer);
    ~GpuCulling();

    GpuCulling(const GpuCulling&)            = delete;
    GpuCulling& operator=(const GpuCulling&) = delete;

    // Frame start (after this slot's fence): counters of the frame that last used the slot.
    void ReadStats(std::uint32_t frameIndex);
    [[nodiscard]] const GpuCullStats& Stats() const { return m_Stats; }

    // views[0] = camera early, views[1] = camera late (only culled by CullLate), then shadow views.
    // Culls every view but the late one and builds their commands. The scene must be uploaded.
    void CullEarly(VkCommandBuffer cmd, const GpuScene& scene, std::span<const GpuCullView> views,
                   VkExtent2D depthExtent);
    // Hi-Z pyramid of the depth target (bindless slot, DEPTH_READ_ONLY_OPTIMAL). Kept (not
    // rebuilt) while `keep` is set: frozen culling.
    void BuildHiZ(VkCommandBuffer cmd, std::uint32_t depthSlot, VkExtent2D extent, bool keep);
    void CullLate(VkCommandBuffer cmd);
    void CopyStats(VkCommandBuffer cmd, std::uint32_t frameIndex);

    // One indirect multi-draw per bucket; setBucket(bucket) binds state before each.
    template <class F>
    void Draw(VkCommandBuffer cmd, std::uint32_t view, std::uint32_t bucketCount, F&& setBucket);

    [[nodiscard]] VkDeviceAddress VisibleAddress() const { return m_Visible.Address(); }
    [[nodiscard]] bool            Active() const { return m_ViewCount > 0; } // culled this frame
    [[nodiscard]] std::uint32_t   IndirectCalls() const { return m_IndirectCalls; }
    // Hi-Z (debug view, frame uniforms): level 0 size, level count (0: none yet).
    [[nodiscard]] VkDeviceAddress HiZAddress() const { return m_HiZ ? m_HiZ.Address() : 0; }
    [[nodiscard]] glm::uvec2      HiZSize() const { return m_HiZSize; }
    [[nodiscard]] std::uint32_t   HiZLevels() const { return m_HiZLevels; }

private:
    struct CullData { // mirrors CullData in gpu_cull.comp
        VkDeviceAddress views = 0, draws = 0, instances = 0, submeshes = 0, batches = 0, counters = 0, visible = 0,
                        visibility = 0, drawCounts = 0, commands = 0, hiz = 0, stats = 0;
        std::uint32_t drawCount = 0, batchCount = 0, batchCapacity = 0, pad = 0;
        glm::uvec4    hizInfo{0};
        glm::uvec4    depthSize{0};
    };
    static_assert(sizeof(CullData) == 144);

    void RecordDraw(VkCommandBuffer cmd, std::uint32_t view, std::uint32_t bucket);
    void Dispatch(VkCommandBuffer cmd, std::uint32_t phase, std::uint32_t firstView, std::uint32_t viewCount,
                  std::uint32_t threads);

    Renderer& m_Renderer;
    Pipeline  m_Cull, m_HiZBuild;

    Buffer m_Visible, m_Counters, m_Commands, m_DrawCounts, m_StatsBuffer, m_HiZ;
    std::array<Buffer, kFramesInFlight> m_Readback;
    std::array<bool, kFramesInFlight>   m_ReadbackValid{};
    GpuCullStats                        m_Stats;

    // This frame
    CullData                   m_Data;
    VkDeviceAddress            m_CullData = 0;
    std::vector<GpuCullView>   m_Views;
    std::uint32_t              m_ViewCount     = 0;
    std::uint32_t              m_BatchCapacity = 0;
    std::uint32_t              m_DrawCapacity  = 0;
    std::uint32_t              m_IndirectCalls = 0;
    glm::uvec2                 m_HiZSize{0};
    std::uint32_t              m_HiZLevels = 0;
    VkExtent2D                 m_HiZExtent{};
};

template <class F>
void GpuCulling::Draw(VkCommandBuffer cmd, std::uint32_t view, std::uint32_t bucketCount, F&& setBucket)
{
    if (view >= m_ViewCount || m_BatchCapacity == 0)
        return;
    for (std::uint32_t bucket = 0; bucket < bucketCount; ++bucket) {
        setBucket(bucket);
        RecordDraw(cmd, view, bucket);
    }
}

} // namespace Engine
