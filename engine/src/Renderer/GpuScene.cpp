#include "GpuScene.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SpatialIndex.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace Engine {

namespace {
constexpr std::uint32_t kNone             = ~0u;
constexpr std::size_t   kTransientStaging = 2u << 20; // larger uploads get their own staging buffer

struct ScatterPush { // mirrors ScatterPush in scatter.comp
    VkDeviceAddress src;
    VkDeviceAddress indices;
    VkDeviceAddress dst;
    std::uint32_t   count;
    std::uint32_t   strideWords;
    std::uint32_t   useIndices;
};

void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask  = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask  = dstStage;
    barrier.dstAccessMask = dstAccess;
    VkDependencyInfo dep{};
    dep.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// (Re)creates `buffer` when it holds fewer than `elements`, with room to grow. True: recreated.
bool Ensure(Renderer& renderer, Buffer& buffer, std::size_t elements, std::size_t stride, const char* name)
{
    const VkDeviceSize needed = std::max<VkDeviceSize>(elements, 1) * stride;
    if (buffer && buffer.Size() >= needed)
        return false;
    const VkDeviceSize size = std::max(needed, buffer ? buffer.Size() * 2 : VkDeviceSize{0});
    if (buffer)
        renderer.DeferRelease(std::move(buffer));
    buffer = Buffer(renderer.GetContext(), {.size      = size,
                                            .usage     = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            .debugName = name});
    return true;
}
} // namespace

GpuScene::GpuScene(Renderer& renderer) : m_Renderer(renderer)
{
    m_Scatter = CreateComputePipeline(renderer.GetContext().Device(), renderer.GetBindless().PipelineLayout(),
                                      ShaderPath("scatter.comp.spv"), "Scatter");
}

GpuScene::~GpuScene()
{
    m_Renderer.DeferRelease(std::move(m_Scatter));
    for (Buffer* b : {&m_InstanceBuffer, &m_DrawBuffer, &m_BatchBuffer, &m_Visibility})
        if (*b)
            m_Renderer.DeferRelease(std::move(*b));
}

// --- CPU side -----------------------------------------------------------------------------------

void GpuScene::Update(const Scene& scene, const SpatialIndex& spatial, const AssetManager& assets)
{
    // Models released while instances still use them: their pool ranges are freed a few frames
    // from now, so the instances must go now.
    std::vector<ModelHandle> released;
    for (const auto& [model, count] : m_ModelUse)
        if (!assets.Get(model))
            released.push_back(model);
    if (!released.empty())
        for (std::uint32_t i = 0; i < m_Instances.size(); ++i)
            if (m_Instances[i].entity != NullEntity && std::ranges::find(released, m_Instances[i].model) != released.end())
                RemoveInstance(i);

    if (spatial.LastSync().rebuilt) {
        for (std::uint32_t i = 0; i < m_Instances.size(); ++i)
            if (m_Instances[i].entity != NullEntity)
                RemoveInstance(i);
        for (const SpatialIndex::MeshProxy& proxy : spatial.Meshes())
            Upsert(scene, assets, proxy.entity);
        return;
    }
    for (const Entity e : spatial.MeshUpdates()) {
        if (spatial.FindMesh(e))
            Upsert(scene, assets, e);
        else
            Remove(e);
    }
}

const GpuInstance* GpuScene::FindInstance(Entity entity) const
{
    const std::uint32_t slot = EntityIndex(entity);
    if (slot >= m_InstanceOf.size() || m_InstanceOf[slot] == kNone)
        return nullptr;
    const std::uint32_t index = m_InstanceOf[slot];
    return m_Instances[index].entity == entity ? &m_InstanceData[index] : nullptr;
}

void GpuScene::Upsert(const Scene& scene, const AssetManager& assets, Entity entity)
{
    const Registry&     registry = scene.GetRegistry();
    const MeshRenderer* renderer = registry.Valid(entity) ? registry.TryGet<MeshRenderer>(entity) : nullptr;
    const Model*        model    = renderer ? assets.Get(renderer->model) : nullptr;
    if (!model || renderer->meshIndex >= model->meshes.size() || model->meshes[renderer->meshIndex].submeshes.empty()) {
        Remove(entity);
        return;
    }
    const glm::mat4& world    = registry.Get<WorldTransform>(entity).matrix;
    const glm::mat3  linear   = glm::mat3(world);
    const bool       mirrored = glm::determinant(linear) < 0.0f; // glTF: flips the winding

    const std::uint32_t slot  = EntityIndex(entity);
    std::uint32_t       index = slot < m_InstanceOf.size() ? m_InstanceOf[slot] : kNone;
    if (index != kNone && m_Instances[index].entity != entity) { // slot reused before the removal arrived
        RemoveInstance(index);
        index = kNone;
    }

    bool newGeometry = true;
    if (index == kNone) {
        if (!m_FreeInstances.empty()) {
            index = m_FreeInstances.back();
            m_FreeInstances.pop_back();
        } else {
            index = static_cast<std::uint32_t>(m_Instances.size());
            m_Instances.emplace_back();
            m_InstanceData.emplace_back();
            m_InstanceDirty.push_back(false);
        }
        if (slot >= m_InstanceOf.size())
            m_InstanceOf.resize(std::max<std::size_t>(slot + 1, m_InstanceOf.size() * 2), kNone);
        m_InstanceOf[slot] = index;
        ++m_LiveInstances;
        ++m_ModelUse[renderer->model];
    } else {
        const Instance& old = m_Instances[index];
        newGeometry = old.model != renderer->model || old.meshIndex != renderer->meshIndex || old.mirrored != mirrored;
        if (newGeometry) {
            ReleaseDraws(index);
            if (--m_ModelUse[old.model] == 0)
                m_ModelUse.erase(old.model);
            ++m_ModelUse[renderer->model];
        }
    }
    m_Instances[index] = {.entity = entity, .model = renderer->model, .meshIndex = renderer->meshIndex, .mirrored = mirrored};

    GpuInstance&    data   = m_InstanceData[index];
    const glm::mat3 normal = glm::transpose(glm::inverse(linear));
    data.model     = world;
    data.normal[0] = glm::vec4(normal[0], 0.0f);
    data.normal[1] = glm::vec4(normal[1], 0.0f);
    data.normal[2] = glm::vec4(normal[2], 0.0f);
    data.entityId  = slot + 1;
    data.flags     = mirrored ? kInstanceMirrored : 0u;

    if (newGeometry) {
        const Mesh&         mesh  = model->meshes[renderer->meshIndex];
        const auto          count = static_cast<std::uint32_t>(mesh.submeshes.size());
        std::optional<std::uint32_t> first = m_DrawRanges.Allocate(count);
        if (!first) {
            const std::uint32_t capacity = std::max({m_DrawRanges.Capacity() * 2, m_DrawRanges.Capacity() + count, 1024u});
            m_DrawRanges.Grow(capacity);
            m_Draws.resize(capacity);
            m_DrawSubmeshes.resize(capacity);
            m_DrawDirty.resize(capacity, false);
            first = m_DrawRanges.Allocate(count);
        }
        data.firstDraw = *first;
        data.drawCount = count;
        m_LiveDraws += count;
        for (std::uint32_t k = 0; k < count; ++k) {
            const Submesh&      sm        = mesh.submeshes[k];
            const std::uint32_t submeshId = mesh.firstGpuSubmesh + k;
            const std::uint32_t d         = *first + k;
            m_DrawSubmeshes[d] = {.firstIndex   = model->indices.offset + sm.firstIndex,
                                  .indexCount   = sm.indexCount,
                                  .vertexOffset = static_cast<std::int32_t>(model->vertices.offset) + sm.vertexOffset,
                                  .material     = model->materials.offset + sm.material,
                                  .boundsMin    = sm.boundsMin,
                                  .flags        = model->materialFlags[sm.material],
                                  .boundsMax    = sm.boundsMax,
                                  .pad          = 0};
            m_Draws[d] = {.instance = index,
                          .submesh  = submeshId,
                          .batch    = AcquireBatch(m_DrawSubmeshes[d], submeshId, mirrored),
                          .pad      = 0};
            MarkDraw(d);
        }
    }
    MarkInstance(index);
}

void GpuScene::Remove(Entity entity)
{
    const std::uint32_t slot = EntityIndex(entity);
    if (slot < m_InstanceOf.size() && m_InstanceOf[slot] != kNone && m_Instances[m_InstanceOf[slot]].entity == entity)
        RemoveInstance(m_InstanceOf[slot]);
}

void GpuScene::RemoveInstance(std::uint32_t index)
{
    Instance& inst = m_Instances[index];
    ReleaseDraws(index);
    if (--m_ModelUse[inst.model] == 0)
        m_ModelUse.erase(inst.model);
    m_InstanceOf[EntityIndex(inst.entity)] = kNone;
    inst = {};
    m_FreeInstances.push_back(index);
    --m_LiveInstances;
}

void GpuScene::ReleaseDraws(std::uint32_t instance)
{
    GpuInstance& data = m_InstanceData[instance];
    for (std::uint32_t d = data.firstDraw; d < data.firstDraw + data.drawCount; ++d) {
        ReleaseBatch(m_Draws[d].batch);
        m_Draws[d] = {};
        MarkDraw(d);
    }
    m_DrawRanges.Free(data.firstDraw, data.drawCount);
    m_LiveDraws -= data.drawCount;
    data.drawCount = 0;
}

std::uint32_t GpuScene::AcquireBatch(const GpuSubmesh& submesh, std::uint32_t submeshIndex, bool mirrored)
{
    const std::uint64_t key = (std::uint64_t{submeshIndex} << 1) | (mirrored ? 1u : 0u);
    m_BatchesDirty          = true; // capacities (instance bases) change
    if (const auto it = m_BatchOf.find(key); it != m_BatchOf.end()) {
        ++m_BatchRefs[it->second];
        return it->second;
    }
    std::uint32_t id;
    if (!m_FreeBatches.empty()) {
        id = m_FreeBatches.back();
        m_FreeBatches.pop_back();
    } else {
        id = static_cast<std::uint32_t>(m_Batches.size());
        m_Batches.emplace_back();
        m_BatchRefs.push_back(0);
        m_BatchKeys.push_back(0);
    }
    m_BatchKeys[id] = key;
    m_Batches[id] = {.indexCount   = submesh.indexCount,
                     .firstIndex   = submesh.firstIndex,
                     .vertexOffset = submesh.vertexOffset,
                     .instanceBase = 0,
                     .cameraBucket = ((submesh.flags & kMaterialDoubleSided) != 0 ? 1u : 0u) | (mirrored ? 2u : 0u),
                     .shadowBucket = (submesh.flags & kMaterialAlphaMask) != 0 ? 1u : 0u,
                     .pad0         = 0,
                     .pad1         = 0};
    m_BatchRefs[id] = 1;
    m_BatchOf.emplace(key, id);
    return id;
}

void GpuScene::ReleaseBatch(std::uint32_t batch)
{
    m_BatchesDirty = true;
    if (--m_BatchRefs[batch] > 0)
        return;
    m_BatchOf.erase(m_BatchKeys[batch]);
    m_Batches[batch] = {}; // indexCount 0: never produces a command
    m_FreeBatches.push_back(batch);
}

void GpuScene::MarkInstance(std::uint32_t index)
{
    if (!m_InstanceDirty[index]) {
        m_InstanceDirty[index] = true;
        m_DirtyInstances.push_back(index);
    }
}

void GpuScene::MarkDraw(std::uint32_t index)
{
    if (!m_DrawDirty[index]) {
        m_DrawDirty[index] = true;
        m_DirtyDraws.push_back(index);
    }
}

// --- GPU side -----------------------------------------------------------------------------------

VkDeviceAddress GpuScene::Stage(const void* data, std::size_t bytes)
{
    if (bytes <= kTransientStaging) {
        const TransientAllocation a = m_Renderer.AllocateTransient(bytes, 16);
        std::memcpy(a.cpu, data, bytes);
        return a.gpu;
    }
    Buffer staging(m_Renderer.GetContext(), {.size      = bytes,
                                             .usage     = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                             .memory    = MemoryUsage::Upload,
                                             .debugName = "GpuSceneStaging"});
    staging.Write(data, bytes);
    const VkDeviceAddress address = staging.Address();
    m_Renderer.DeferRelease(std::move(staging)); // freed once this frame is done
    return address;
}

void GpuScene::Scatter(VkCommandBuffer cmd, VkDeviceAddress dst, const void* data, std::size_t elementBytes,
                       std::span<const std::uint32_t> indices, std::uint32_t count)
{
    if (count == 0)
        return;
    const ScatterPush push{.src         = Stage(data, elementBytes * count),
                           .indices     = indices.empty() ? 0 : Stage(indices.data(), indices.size_bytes()),
                           .dst         = dst,
                           .count       = count,
                           .strideWords = static_cast<std::uint32_t>(elementBytes / 4),
                           .useIndices  = indices.empty() ? 0u : 1u};
    const std::uint64_t threads = std::uint64_t{count} * push.strideWords;
    const auto          groups  = static_cast<std::uint32_t>((threads + 63) / 64);
    const std::uint32_t x       = std::min(groups, 65535u);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.Handle());
    vkCmdPushConstants(cmd, m_Renderer.GetBindless().PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, x, (groups + x - 1) / x, 1);
}

void GpuScene::Upload(VkCommandBuffer cmd)
{
    if (m_BatchesDirty) { // each batch owns a slice of every view's visible list
        std::uint32_t base = 0;
        for (std::size_t b = 0; b < m_Batches.size(); ++b) {
            m_Batches[b].instanceBase = base;
            base += m_BatchRefs[b];
        }
    }

    const bool newInstances = Ensure(m_Renderer, m_InstanceBuffer, m_InstanceData.size(), sizeof(GpuInstance), "GpuInstances");
    const bool newDraws     = Ensure(m_Renderer, m_DrawBuffer, m_Draws.size(), sizeof(GpuDraw), "GpuDraws");
    const bool newBatches   = Ensure(m_Renderer, m_BatchBuffer, m_Batches.size(), sizeof(GpuBatch), "GpuBatches");
    const bool newVisible   = Ensure(m_Renderer, m_Visibility, m_Draws.size(), sizeof(std::uint32_t), "GpuDrawVisibility");
    m_BatchesDirty |= newBatches;

    const bool fullInstances = newInstances && !m_InstanceData.empty();
    const bool fullDraws     = newDraws && !m_Draws.empty();
    const bool work = newVisible || fullInstances || fullDraws || !m_DirtyInstances.empty() || !m_DirtyDraws.empty() ||
                      (m_BatchesDirty && !m_Batches.empty());
    if (!work)
        return;

    // Earlier frames' culling and vertex shaders read these buffers (WAR).
    Barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, // + WAW with earlier uploads
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (newVisible) // stale visibility only costs one frame of extra early draws: start from "not visible"
        vkCmdFillBuffer(cmd, m_Visibility.Handle(), 0, VK_WHOLE_SIZE, 0);

    m_Renderer.GetBindless().Bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    const auto gather = [](auto& mirror, std::vector<std::uint32_t>& dirty, std::vector<bool>& flags) {
        std::vector<std::remove_cvref_t<decltype(mirror[0])>> values;
        values.reserve(dirty.size());
        for (std::uint32_t i : dirty) {
            values.push_back(mirror[i]);
            flags[i] = false;
        }
        return values;
    };

    if (fullInstances) { // new buffer: everything, contiguous
        Scatter(cmd, m_InstanceBuffer.Address(), m_InstanceData.data(), sizeof(GpuInstance), {},
                static_cast<std::uint32_t>(m_InstanceData.size()));
        for (std::uint32_t i : m_DirtyInstances)
            m_InstanceDirty[i] = false;
    } else if (!m_DirtyInstances.empty()) {
        const auto values = gather(m_InstanceData, m_DirtyInstances, m_InstanceDirty);
        Scatter(cmd, m_InstanceBuffer.Address(), values.data(), sizeof(GpuInstance), m_DirtyInstances,
                static_cast<std::uint32_t>(values.size()));
    }
    m_DirtyInstances.clear();

    if (fullDraws) {
        Scatter(cmd, m_DrawBuffer.Address(), m_Draws.data(), sizeof(GpuDraw), {}, static_cast<std::uint32_t>(m_Draws.size()));
        for (std::uint32_t i : m_DirtyDraws)
            m_DrawDirty[i] = false;
    } else if (!m_DirtyDraws.empty()) {
        const auto values = gather(m_Draws, m_DirtyDraws, m_DrawDirty);
        Scatter(cmd, m_DrawBuffer.Address(), values.data(), sizeof(GpuDraw), m_DirtyDraws,
                static_cast<std::uint32_t>(values.size()));
    }
    m_DirtyDraws.clear();

    if (m_BatchesDirty && !m_Batches.empty())
        Scatter(cmd, m_BatchBuffer.Address(), m_Batches.data(), sizeof(GpuBatch), {},
                static_cast<std::uint32_t>(m_Batches.size()));
    m_BatchesDirty = false;

    Barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

} // namespace Engine
