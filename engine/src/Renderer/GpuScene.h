#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/Model.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Material/MaterialAsset.h"
#include "Engine/Renderer/RangeAllocator.h"
#include "Engine/Renderer/Vulkan/Buffer.h"
#include "Engine/Renderer/Vulkan/Pipeline.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
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
    std::uint32_t prevJointOffset = ~0u; // last frame's palette (PrevJointMatrixAddress), ~0u: none
    std::uint32_t pad1 = 0;
    glm::vec4 skinnedBoundsMin{0.0f};
    glm::vec4 skinnedBoundsMax{0.0f};
    glm::mat4 prevModel{1.0f}; // last frame's model matrix (motion vectors)
};
static_assert(sizeof(GpuInstance) == 240);
inline constexpr std::uint32_t kInstanceMirrored = 1;
inline constexpr std::uint32_t kInstanceSkinned = 2;
inline constexpr std::uint32_t kInstanceSkinBoundsValid = 4;

// Mirrors GpuDraw: one submesh of one instance, the unit of culling.
struct GpuDraw {
    std::uint32_t instance = ~0u; // ~0u: free slot
    std::uint32_t submesh  = 0;   // geometry pool record
    std::uint32_t batch    = 0;
    std::uint32_t params   = 0;   // custom material: parameter block (vec4 index)
};
static_assert(sizeof(GpuDraw) == 16);

// Mirrors GpuBatch: all draws of one submesh LOD with the same winding -> one indirect command.
// A submesh with n LODs owns n consecutive batches (GpuDraw::batch = LOD 0, + lod per view).
struct GpuBatch {
    std::uint32_t indexCount   = 0; // 0: unused id
    std::uint32_t firstIndex   = 0;
    std::int32_t  vertexOffset = 0;
    std::uint32_t instanceBase = 0; // slice of a view's visible list (prefix sum of draw counts)
    std::uint32_t cameraBucket = 0; // bit 0: double-sided, bit 1: mirrored; kCameraBucketBlend: alpha-blended;
                                    // custom material: kCustomCameraBucketBase + 2 * slot + mirrored
    std::uint32_t shadowBucket = 0; // 1: alpha-masked; custom material: kShadowBuckets + slot; kBucketNone
    float         displacement = 0.0f; // custom material: world position offset bound (culling)
    std::uint32_t material     = 0;    // custom material slot + 1 (0: the model's material)
};
static_assert(sizeof(GpuBatch) == 32);

inline constexpr std::uint32_t kCameraBuckets          = 4; // glTF materials (indirect)
inline constexpr std::uint32_t kCameraBucketBlend      = 4; // alpha-blended: no indirect command, sorted CPU pass
inline constexpr std::uint32_t kCustomCameraBucketBase = 5; // + 2 * material slot + mirrored
inline constexpr std::uint32_t kShadowBuckets          = 2; // glTF materials; custom: + material slot
inline constexpr std::uint32_t kBucketNone             = ~0u; // casts no shadow

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
        std::uint64_t materialKey = 0; // custom materials (handles + revisions): draws rebuilt on change
        std::uint64_t paramKey    = 0; // per-entity parameter values: blocks rewritten on change
    };

    // A custom material in use (index = slot): its buckets and pipelines (SceneRenderer).
    struct MaterialSlotInfo {
        MaterialHandle                         handle;
        std::uint32_t                          draws = 0; // 0: free slot
        MaterialSettings                       settings{};
        std::shared_ptr<const MaterialShaders> shaders;
    };
    // Custom material of a MaterialOverrides path (SceneRenderer: holds a reference per path).
    using MaterialResolver = std::function<MaterialHandle(const std::string& path)>;

    explicit GpuScene(Renderer& renderer);
    ~GpuScene(); // buffers go through the deferred queue

    GpuScene(const GpuScene&)            = delete;
    GpuScene& operator=(const GpuScene&) = delete;

    void Update(const Scene& scene, const SpatialIndex& spatial, const AssetManager& assets,
                const MaterialResolver& materials = {});
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
    [[nodiscard]] bool IsBlendDraw(std::uint32_t draw) const { return Batch(Draw(draw).batch).cameraBucket == kCameraBucketBlend; }

    // Custom materials and the bucket layout (valid after Upload): commands of a view are grouped
    // by bucket, bucket b at BucketOffset (its capacity = batches in it).
    [[nodiscard]] const std::vector<MaterialSlotInfo>& MaterialSlots() const { return m_MaterialSlots; }
    [[nodiscard]] std::uint32_t CameraBucketCount() const { return kCustomCameraBucketBase + 2 * static_cast<std::uint32_t>(m_MaterialSlots.size()); }
    [[nodiscard]] std::uint32_t ShadowBucketCount() const { return kShadowBuckets + static_cast<std::uint32_t>(m_MaterialSlots.size()); }
    [[nodiscard]] std::uint32_t BucketOffset(bool shadow, std::uint32_t bucket) const;
    [[nodiscard]] std::uint32_t BucketCapacity(bool shadow, std::uint32_t bucket) const;
    [[nodiscard]] std::uint32_t ShadowBucketBase() const { return m_BucketCameraCount; } // in the bucket buffer
    [[nodiscard]] VkDeviceAddress BucketAddress() const { return m_BucketBuffer.Address(); }
    [[nodiscard]] std::uint32_t MaterialParamCount() const { return static_cast<std::uint32_t>(m_ParamData.size()); }
    // GPU buffers (valid after Upload).
    [[nodiscard]] VkDeviceAddress InstanceAddress() const { return m_InstanceBuffer.Address(); }
    [[nodiscard]] VkDeviceAddress JointMatrixAddress() const { return m_JointMatrices[m_JointBuffer].Address(); }
    // Last frame's skin palettes (motion vectors; 0 before the second frame).
    [[nodiscard]] VkDeviceAddress PrevJointMatrixAddress() const
    {
        const Buffer& b = m_JointMatrices[m_JointBuffer ^ 1u];
        return b ? b.Address() : 0;
    }
    [[nodiscard]] VkDeviceAddress DrawAddress() const { return m_DrawBuffer.Address(); }
    [[nodiscard]] VkDeviceAddress BatchAddress() const { return m_BatchBuffer.Address(); }
    [[nodiscard]] VkDeviceAddress VisibilityAddress() const { return m_Visibility.Address(); }
    // Custom material parameter blocks (vec4 per slot; GpuDraw::params indexes them).
    [[nodiscard]] VkDeviceAddress MaterialParamAddress() const { return m_ParamBuffer ? m_ParamBuffer.Address() : 0; }

private:
    void Upsert(const Scene& scene, const AssetManager& assets, Entity entity);
    // Custom material per submesh of the entity's mesh (null: the model's) and their keys.
    void ResolveMaterials(const Scene& scene, const AssetManager& assets, Entity entity, std::uint32_t submeshes,
                          std::vector<MaterialHandle>& out, std::uint64_t& materialKey, std::uint64_t& paramKey) const;
    void WriteParams(const Scene& scene, const AssetManager& assets, Entity entity, std::uint32_t draw,
                     std::uint32_t submesh, const MaterialAsset& material);
    [[nodiscard]] std::uint32_t AcquireMaterialSlot(MaterialHandle handle, const MaterialAsset& material);
    void ReleaseMaterialSlot(std::uint32_t slot);
    void UpdateJointPalettes(const Scene& scene, const AssetManager& assets);
    void UseModel(const AssetManager& assets, ModelHandle model);
    void UnuseModel(ModelHandle model);
    void Remove(Entity entity);
    void RemoveInstance(std::uint32_t index);
    void ReleaseDraws(std::uint32_t instance);
    [[nodiscard]] std::uint32_t AcquireBatch(const GpuSubmesh& submesh, std::uint32_t submeshIndex, bool mirrored,
                                             std::uint32_t materialSlot = ~0u, std::uint32_t materialRevision = 0);
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
    const MaterialResolver* m_Resolve = nullptr; // during Update

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
    std::unordered_map<std::uint64_t, std::uint32_t> m_BatchOf; // mirrored | submesh << 1 | material << 25 -> first batch
    std::unordered_map<ModelHandle, ModelUse>        m_ModelUse; // release / reload detection
    std::uint32_t                                    m_VisibleCapacity = 0;

    // Custom materials
    std::vector<MaterialSlotInfo>                      m_MaterialSlots;
    std::vector<std::uint32_t>                         m_FreeMaterialSlots;
    std::unordered_map<MaterialHandle, std::uint32_t>  m_MaterialSlotOf;
    std::uint64_t                                      m_MaterialVersion = ~0ull;
    RangeAllocator                                     m_ParamRanges;
    std::vector<glm::vec4>                             m_ParamData;     // mirror of m_ParamBuffer
    std::vector<std::uint32_t>                         m_DrawParamCount; // per draw record
    bool                                               m_ParamsDirty = true;
    std::vector<std::uint32_t>                         m_BucketData;    // camera offsets, shadow offsets
    std::vector<std::uint32_t>                         m_BucketCapacity;
    std::uint32_t                                      m_BucketCameraCount = 0;
    Buffer                                             m_BucketBuffer;

    // Instances whose model matrix changed last frame: prevModel catches up this frame.
    std::vector<std::uint32_t> m_Moved;
    std::vector<bool>          m_MovedFlag;

    // Pending uploads
    std::vector<std::uint32_t> m_DirtyInstances, m_DirtyDraws;
    std::vector<bool>          m_InstanceDirty, m_DrawDirty;
    bool                       m_BatchesDirty = false;

    // GPU buffers (capacity in elements)
    Buffer m_InstanceBuffer, m_DrawBuffer, m_BatchBuffer, m_Visibility, m_ParamBuffer;
    // Skin palettes, ping-pong: this frame's are written into m_JointMatrices[m_JointBuffer], the
    // other one still holds last frame's (prevJointOffset indexes it).
    std::array<Buffer, 2>  m_JointMatrices;
    std::uint32_t          m_JointBuffer = 0;
    std::vector<glm::mat4> m_JointMatrixData;
};

} // namespace Engine
