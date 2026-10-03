#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/Model.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Renderer/RangeAllocator.h"
#include "Engine/Renderer/Vulkan/Buffer.h"
#include "Engine/Renderer/Vulkan/Pipeline.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Engine {

class AssetManager;
class Renderer;
class Scene;
class SpatialIndex;

// Mirrors GpuInstance in scene_common.glsl.
struct GpuInstance {
    glm::mat4     model{1.0f};
    glm::vec4     normal[3]{}; // inverse-transpose of the upper 3x3 (columns)
    std::uint32_t entityId  = 0; // entity slot index + 1
    std::uint32_t flags     = 0; // kInstanceMirrored
    std::uint32_t firstDraw = 0;
    std::uint32_t drawCount = 0;
    std::uint32_t jointOffset = ~0u;
    std::uint32_t jointCount = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
};
static_assert(sizeof(GpuInstance) == 144);
inline constexpr std::uint32_t kInstanceMirrored = 1;
inline constexpr std::uint32_t kInstanceSkinned = 2;

// Mirrors GpuDraw: one submesh of one instance, the unit of culling.
struct GpuDraw {
    std::uint32_t instance = ~0u; // ~0u: free slot
    std::uint32_t submesh  = 0;   // geometry pool record
    std::uint32_t batch    = 0;
    std::uint32_t pad      = 0;
};
static_assert(sizeof(GpuDraw) == 16);

// Mirrors GpuBatch: all draws of one submesh LOD with the same winding -> one indirect command.
// A submesh with n LODs owns n consecutive batches (GpuDraw::batch = LOD 0, + lod per view).
struct GpuBatch {
    std::uint32_t indexCount   = 0; // 0: unused id
    std::uint32_t firstIndex   = 0;
    std::int32_t  vertexOffset = 0;
    std::uint32_t instanceBase = 0; // slice of a view's visible list (prefix sum of draw counts)
    std::uint32_t cameraBucket = 0; // bit 0: double-sided, bit 1: mirrored; kCameraBucketBlend: alpha-blended
    std::uint32_t shadowBucket = 0; // 1: alpha-masked
    std::uint32_t pad0 = 0, pad1 = 0;
};
static_assert(sizeof(GpuBatch) == 32);

inline constexpr std::uint32_t kCameraBuckets     = 4;
inline constexpr std::uint32_t kCameraBucketBlend = 4; // alpha-blended: no indirect command, sorted CPU pass
inline constexpr std::uint32_t kShadowBuckets     = 2;

// Visible list entry (mirrors VISIBLE_* in scene_common.glsl): draw record (22 bits) | LOD << 22 |
// cross-fade << 24 | fading in << 31.
inline constexpr std::uint32_t kVisibleRecordMask = 0x003FFFFFu;
inline constexpr std::uint32_t kVisibleLodShift   = 22;
inline constexpr std::uint32_t kVisibleFadeShift  = 24;
inline constexpr std::uint32_t kVisibleFadeIn     = 0x80000000u;
inline constexpr std::uint32_t kMaxDrawRecords    = kVisibleRecordMask + 1;

struct LodChoice {
    std::uint32_t lod  = 0;
    std::uint32_t fade = 0; // 0..127 towards lod + 1 (camera views draw both, dithered)
};

// Mirrors SelectLod in scene_common.glsl (the CPU path must pick the same geometry).
[[nodiscard]] LodChoice SelectLod(const GpuSubmesh& submesh, const glm::mat4& model, const glm::vec4& lodCamera,
                                  std::uint32_t forced);

// Persistent GPU copy of the scene's mesh instances for GPU-driven rendering (and the CPU path,
// which draws the same records directly). Update() applies the spatial index's mesh changes on
// the CPU; Upload() records this frame's changes (compute scatter) into the command buffer.
class GpuScene {
public:
    struct Instance {
        Entity        entity = NullEntity;
        ModelHandle   model;
        std::uint32_t meshIndex = 0;
        std::uint32_t revision  = 0; // AssetManager::Revision of `model` when the draws were built
        bool          mirrored  = false;
    };

    explicit GpuScene(Renderer& renderer);
    ~GpuScene(); // buffers go through the deferred queue

    GpuScene(const GpuScene&)            = delete;
    GpuScene& operator=(const GpuScene&) = delete;

    void Update(const Scene& scene, const SpatialIndex& spatial, const AssetManager& assets);
    void Upload(VkCommandBuffer cmd);
    void RebuildPipelines(); // shader hot reload

    // CPU path / stats
    [[nodiscard]] const GpuInstance* FindInstance(Entity entity) const;
    [[nodiscard]] const GpuDraw&     Draw(std::uint32_t index) const { return m_Draws[index]; }
    [[nodiscard]] const GpuInstance& InstanceData(std::uint32_t index) const { return m_InstanceData[index]; }
    [[nodiscard]] const GpuSubmesh&  DrawSubmesh(std::uint32_t index) const { return m_DrawSubmeshes[index]; }
    [[nodiscard]] const GpuBatch&    Batch(std::uint32_t index) const { return m_Batches[index]; }
    [[nodiscard]] std::uint32_t InstanceCount() const { return m_LiveInstances; }
    [[nodiscard]] std::uint32_t LiveDraws() const { return m_LiveDraws; }
    // Visible list size per view: every LOD batch has room for all draws of its submesh.
    [[nodiscard]] std::uint32_t VisibleCapacity() const { return m_VisibleCapacity; }
    [[nodiscard]] std::uint32_t DrawCapacity() const { return m_DrawRanges.Capacity(); } // incl. free slots
    [[nodiscard]] std::uint32_t BatchCount() const { return static_cast<std::uint32_t>(m_Batches.size()); } // incl. free ids
    [[nodiscard]] std::uint32_t BlendDraws() const { return m_BlendDraws; } // alpha-blended draw records
    [[nodiscard]] std::uint32_t LiveBatches() const { return static_cast<std::uint32_t>(m_BatchOf.size()); } // blocks

    // GPU buffers (valid after Upload).
    [[nodiscard]] VkDeviceAddress InstanceAddress() const { return m_InstanceBuffer.Address(); }
    [[nodiscard]] VkDeviceAddress JointMatrixAddress() const { return m_JointMatrices.Address(); }
    [[nodiscard]] VkDeviceAddress DrawAddress() const { return m_DrawBuffer.Address(); }
    [[nodiscard]] VkDeviceAddress BatchAddress() const { return m_BatchBuffer.Address(); }
    [[nodiscard]] VkDeviceAddress VisibilityAddress() const { return m_Visibility.Address(); }

private:
    void Upsert(const Scene& scene, const AssetManager& assets, Entity entity);
    void UpdateJointPalettes(const Scene& scene, const AssetManager& assets);
    void UseModel(const AssetManager& assets, ModelHandle model);
    void UnuseModel(ModelHandle model);
    void Remove(Entity entity);
    void RemoveInstance(std::uint32_t index);
    void ReleaseDraws(std::uint32_t instance);
    [[nodiscard]] std::uint32_t AcquireBatch(const GpuSubmesh& submesh, std::uint32_t submeshIndex, bool mirrored);
    void ReleaseBatch(std::uint32_t batch);
    void MarkInstance(std::uint32_t index);
    void MarkDraw(std::uint32_t index);
    // Host memory the scatter shader reads: this frame's transient memory, or a one-off staging
    // buffer for large uploads.
    [[nodiscard]] VkDeviceAddress Stage(const void* data, std::size_t bytes);
    void Scatter(VkCommandBuffer cmd, VkDeviceAddress dst, const void* data, std::size_t elementBytes,
                 std::span<const std::uint32_t> indices, std::uint32_t count);

    Renderer& m_Renderer;
    Pipeline  m_Scatter;

    // CPU state
    std::vector<Instance>      m_Instances;     // slot -> instance (entity == NullEntity: free)
    std::vector<GpuInstance>   m_InstanceData;  // mirror of the GPU buffer
    std::vector<std::uint32_t> m_FreeInstances;
    std::vector<std::uint32_t> m_InstanceOf;    // entity slot index -> instance, ~0u
    std::uint32_t              m_LiveInstances = 0;

    RangeAllocator             m_DrawRanges;
    std::vector<GpuDraw>       m_Draws;         // mirror
    std::vector<GpuSubmesh>    m_DrawSubmeshes; // CPU path: geometry of each draw
    std::uint32_t              m_LiveDraws  = 0;
    std::uint32_t              m_BlendDraws = 0;
    bool                       m_ReportedFull = false;

    struct ModelUse {
        std::uint32_t instances = 0;
        std::uint32_t revision  = 0;
    };
    std::vector<GpuBatch>                            m_Batches;   // mirror
    std::vector<std::uint32_t>                       m_BatchRefs; // per batch: draws of its block
    std::vector<std::uint64_t>                       m_BatchKeys; // per block (first batch)
    std::vector<std::uint32_t>                       m_BatchSizes; // per block: LOD count
    RangeAllocator                                   m_BatchRanges;
    std::unordered_map<std::uint64_t, std::uint32_t> m_BatchOf; // (submesh << 1) | mirrored -> first batch
    std::unordered_map<ModelHandle, ModelUse>        m_ModelUse; // release / reload detection
    std::uint32_t                                    m_VisibleCapacity = 0;

    // Pending uploads
    std::vector<std::uint32_t> m_DirtyInstances, m_DirtyDraws;
    std::vector<bool>          m_InstanceDirty, m_DrawDirty;
    bool                       m_BatchesDirty = false;

    // GPU buffers (capacity in elements)
    Buffer m_InstanceBuffer, m_DrawBuffer, m_BatchBuffer, m_Visibility, m_JointMatrices;
    std::vector<glm::mat4> m_JointMatrixData;
};

} // namespace Engine
