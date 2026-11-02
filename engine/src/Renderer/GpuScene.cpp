#include "GpuScene.h"
#include "Engine/Assets/Animation.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SpatialIndex.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <unordered_map>

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

void GpuScene::RebuildPipelines()
{
    Pipeline scatter = CreateComputePipeline(m_Renderer.GetContext().Device(), m_Renderer.GetBindless().PipelineLayout(),
                                             ShaderPath("scatter.comp.spv"), "Scatter");
    m_Renderer.DeferRelease(std::move(m_Scatter));
    m_Scatter = std::move(scatter);
}

GpuScene::~GpuScene()
{
    m_Renderer.DeferRelease(std::move(m_Scatter));
    for (Buffer* b : {&m_InstanceBuffer, &m_DrawBuffer, &m_BatchBuffer, &m_Visibility, &m_JointMatrices[0], &m_JointMatrices[1],
                      &m_ParamBuffer, &m_BucketBuffer})
        if (*b)
            m_Renderer.DeferRelease(std::move(*b));
}

// --- CPU side -----------------------------------------------------------------------------------

LodChoice SelectLod(const GpuSubmesh& submesh, const glm::mat4& model, const glm::vec4& lodCamera, std::uint32_t forced)
{
    constexpr float kFadeBand = 0.2f; // LOD_FADE_BAND
    if (submesh.lodCount <= 1)
        return {};
    if (forced > 0)
        return {.lod = std::min(forced - 1, submesh.lodCount - 1), .fade = 0};
    if (lodCamera.w <= 0.0f)
        return {};
    const glm::vec3 c      = (submesh.boundsMin + submesh.boundsMax) * 0.5f;
    const glm::vec3 e      = (submesh.boundsMax - submesh.boundsMin) * 0.5f;
    const glm::vec3 center = glm::vec3(model * glm::vec4(c, 1.0f));
    const glm::vec3 extent = glm::mat3(glm::abs(glm::vec3(model[0])), glm::abs(glm::vec3(model[1])),
                                       glm::abs(glm::vec3(model[2]))) * e;
    const float scale    = std::max({glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])),
                                     glm::length(glm::vec3(model[2]))});
    const float distance = glm::length(glm::vec3(lodCamera) - center) - glm::length(extent);
    if (distance <= 0.0f)
        return {};
    LodChoice choice;
    for (std::uint32_t l = 1; l < submesh.lodCount; ++l)
        if (submesh.lodError[static_cast<glm::length_t>(l)] * scale * lodCamera.w <= distance)
            choice.lod = l;
    if (choice.lod + 1 < submesh.lodCount) {
        const float next = submesh.lodError[static_cast<glm::length_t>(choice.lod + 1)] * scale * lodCamera.w;
        const float t    = (distance / next - (1.0f - kFadeBand)) / kFadeBand;
        choice.fade      = t > 0.0f ? std::min(static_cast<std::uint32_t>(t * 128.0f), 127u) : 0u;
    }
    return choice;
}

void GpuScene::Update(const Scene& scene, const SpatialIndex& spatial, const AssetManager& assets,
                      const MaterialResolver& materials)
{
    m_Resolve = &materials;
    // Moved last frame: the previous matrix catches up (no motion unless it moves again).
    for (const std::uint32_t i : m_Moved) {
        m_MovedFlag[i] = false;
        GpuInstance& data = m_InstanceData[i];
        if (m_Instances[i].entity != NullEntity && data.prevModel != data.model) {
            data.prevModel = data.model;
            MarkInstance(i);
        }
    }
    m_Moved.clear();

    // Released models: their pool ranges are freed a few frames from now, so the instances must
    // go now. Reloaded / failed / retried models (new revision): rebuild their draws. Both before
    // any other upsert, so a batch of a freed submesh record is never reused by a new model.
    std::vector<ModelHandle> released, changed;
    for (const auto& [model, use] : m_ModelUse) {
        if (assets.State(model) == AssetState::Invalid)
            released.push_back(model);
        else if (assets.Revision(model) != use.revision)
            changed.push_back(model);
    }
    if (!released.empty() || !changed.empty())
        for (std::uint32_t i = 0; i < m_Instances.size(); ++i) {
            const Instance& inst = m_Instances[i];
            if (inst.entity == NullEntity)
                continue;
            if (std::ranges::find(released, inst.model) != released.end())
                RemoveInstance(i);
            else if (std::ranges::find(changed, inst.model) != changed.end())
                Upsert(scene, assets, inst.entity);
        }
    for (ModelHandle model : changed)
        if (const auto it = m_ModelUse.find(model); it != m_ModelUse.end())
            it->second.revision = assets.Revision(model);

    // Custom materials loaded, reloaded, failed or released: instances that use (or ask for) one
    // re-resolve; unchanged ones keep their draws (material key).
    if (assets.MaterialVersion() != m_MaterialVersion) {
        m_MaterialVersion = assets.MaterialVersion();
        const Registry& registry = scene.GetRegistry();
        for (std::uint32_t i = 0; i < m_Instances.size(); ++i) {
            const Instance& inst = m_Instances[i];
            if (inst.entity != NullEntity && (inst.materialKey != 0 || registry.Has<MaterialOverrides>(inst.entity)))
                Upsert(scene, assets, inst.entity);
        }
    }

    if (spatial.LastSync().rebuilt) {
        for (std::uint32_t i = 0; i < m_Instances.size(); ++i)
            if (m_Instances[i].entity != NullEntity)
                RemoveInstance(i);
        for (const SpatialIndex::MeshProxy& proxy : spatial.Meshes())
            Upsert(scene, assets, proxy.entity);
        UpdateJointPalettes(scene, assets);
        return;
    }
    for (const Entity e : spatial.MeshUpdates()) {
        if (spatial.FindMesh(e))
            Upsert(scene, assets, e);
        else
            Remove(e);
    }
    UpdateJointPalettes(scene, assets);
}

void GpuScene::UpdateJointPalettes(const Scene& scene, const AssetManager& assets)
{
    const Registry& registry = scene.GetRegistry();
    std::unordered_map<Entity, std::vector<Entity>> nodeEntitiesByRoot;
    std::unordered_map<Entity, Entity> rootOf; // node entity -> its model instance
    registry.ViewOf<ModelNodeRef>().Each([&](Entity entity, const ModelNodeRef& reference) {
        const Entity root = FindModelInstanceRoot(registry, entity);
        if (root == NullEntity)
            return;
        rootOf.emplace(entity, root);
        auto& nodes = nodeEntitiesByRoot[root];
        if (nodes.size() <= reference.node)
            nodes.resize(static_cast<std::size_t>(reference.node) + 1, NullEntity);
        nodes[reference.node] = entity;
    });

    m_JointMatrixData.clear();
    for (std::uint32_t i = 0; i < m_Instances.size(); ++i) {
        const Entity entity = m_Instances[i].entity;
        GpuInstance& gpu = m_InstanceData[i];
        const std::uint32_t oldFlags = gpu.flags;
        const std::uint32_t oldOffset = gpu.jointOffset;
        const std::uint32_t oldCount = gpu.jointCount;
        const std::uint32_t oldPrevOffset = gpu.prevJointOffset;
        gpu.prevJointOffset = ~0u;
        const glm::vec3 oldBoundsMin = glm::vec3(gpu.skinnedBoundsMin);
        const glm::vec3 oldBoundsMax = glm::vec3(gpu.skinnedBoundsMax);
        const auto markIfChanged = [&] {
            if (gpu.flags != oldFlags || gpu.jointOffset != oldOffset || gpu.jointCount != oldCount ||
                gpu.prevJointOffset != oldPrevOffset ||
                glm::length(glm::vec3(gpu.skinnedBoundsMin) - oldBoundsMin) > 1.0e-5f ||
                glm::length(glm::vec3(gpu.skinnedBoundsMax) - oldBoundsMax) > 1.0e-5f)
                MarkInstance(i);
        };
        gpu.jointOffset = ~0u;
        gpu.jointCount = 0;
        gpu.flags &= ~(kInstanceSkinned | kInstanceSkinBoundsValid);
        if (entity == NullEntity || !registry.Valid(entity)) {
            markIfChanged();
            continue;
        }
        const MeshRenderer* meshRenderer = registry.TryGet<MeshRenderer>(entity);
        const ModelNodeRef* nodeRef = registry.TryGet<ModelNodeRef>(entity);
        if (!meshRenderer || !nodeRef) {
            markIfChanged();
            continue;
        }
        const Model* model = assets.Get(meshRenderer->model);
        if (!model || nodeRef->node >= model->nodes.size()) {
            markIfChanged();
            continue;
        }
        const std::int32_t skinIndex = model->nodes[nodeRef->node].skin;
        if (skinIndex < 0 || static_cast<std::size_t>(skinIndex) >= model->skins.size()) {
            markIfChanged();
            continue;
        }
        const Skin& skin = model->skins[static_cast<std::size_t>(skinIndex)];
        const auto owner  = rootOf.find(entity);
        const auto rootIt = owner == rootOf.end() ? nodeEntitiesByRoot.end() : nodeEntitiesByRoot.find(owner->second);
        if (skin.joints.empty() || rootIt == nodeEntitiesByRoot.end()) {
            markIfChanged();
            continue;
        }
        const glm::mat4& meshWorld = registry.Get<WorldTransform>(entity).matrix;
        const float determinant = glm::determinant(meshWorld);
        if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-8f) {
            markIfChanged();
            continue;
        }
        const glm::mat4 inverseMesh = glm::inverse(meshWorld);
        const std::uint32_t offset = static_cast<std::uint32_t>(m_JointMatrixData.size());
        bool valid = true;
        for (const SkinJoint& joint : skin.joints) {
            if (joint.node < 0 || static_cast<std::size_t>(joint.node) >= rootIt->second.size()) {
                valid = false;
                break;
            }
            const Entity jointEntity = rootIt->second[static_cast<std::size_t>(joint.node)];
            if (jointEntity == NullEntity || !registry.Valid(jointEntity)) {
                valid = false;
                break;
            }
            const glm::mat4 matrix = inverseMesh * registry.Get<WorldTransform>(jointEntity).matrix * joint.inverseBindMatrix;
            m_JointMatrixData.push_back(matrix);
        }
        if (!valid) {
            m_JointMatrixData.resize(offset);
            markIfChanged();
            continue;
        }
        gpu.jointOffset = offset;
        gpu.jointCount = static_cast<std::uint32_t>(skin.joints.size());
        gpu.flags |= kInstanceSkinned;
        // Last frame's palette of this instance sits in the other buffer at its old offset.
        if (oldOffset != ~0u && oldCount == gpu.jointCount)
            gpu.prevJointOffset = oldOffset;

        if (const auto bounds = ComputeSkinnedBounds(*model, meshRenderer->meshIndex,
                                                     static_cast<std::uint32_t>(skinIndex),
                                                     std::span<const glm::mat4>{m_JointMatrixData.data() + offset,
                                                                                skin.joints.size()})) {
            gpu.skinnedBoundsMin = glm::vec4(bounds->first, 0.0f);
            gpu.skinnedBoundsMax = glm::vec4(bounds->second, 0.0f);
            gpu.flags |= kInstanceSkinBoundsValid;
        }
        markIfChanged();
    }
}

void GpuScene::UseModel(const AssetManager& assets, ModelHandle model)
{
    ModelUse& use = m_ModelUse[model];
    ++use.instances;
    use.revision = assets.Revision(model); // every instance of it is (re)built with this revision
}

void GpuScene::UnuseModel(ModelHandle model)
{
    const auto it = m_ModelUse.find(model);
    if (it != m_ModelUse.end() && --it->second.instances == 0)
        m_ModelUse.erase(it);
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
    const ResolvedMesh  resolved = renderer ? assets.ResolveMesh(renderer->model, renderer->meshIndex) : ResolvedMesh{};
    const Model*        model    = resolved.model; // the placeholder for failed models
    if (!model || model->meshes[resolved.meshIndex].submeshes.empty()) {
        Remove(entity);
        return;
    }
    const std::uint32_t revision = assets.Revision(renderer->model);
    const Mesh&         mesh     = model->meshes[resolved.meshIndex];
    const auto          count    = static_cast<std::uint32_t>(mesh.submeshes.size());
    std::vector<MaterialHandle> materials;
    std::uint64_t               materialKey = 0, paramKey = 0;
    ResolveMaterials(scene, assets, entity, count, materials, materialKey, paramKey);
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
    const bool existed = index != kNone;
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
        UseModel(assets, renderer->model);
    } else {
        const Instance& old = m_Instances[index];
        newGeometry = old.model != renderer->model || old.meshIndex != renderer->meshIndex ||
                      old.mirrored != mirrored || old.revision != revision || old.materialKey != materialKey;
        if (newGeometry) {
            ReleaseDraws(index);
            UnuseModel(old.model);
            UseModel(assets, renderer->model);
        } else if (old.paramKey != paramKey) { // only parameter values: rewrite the blocks in place
            const GpuInstance& d = m_InstanceData[index];
            for (std::uint32_t k = 0; k < d.drawCount; ++k)
                if (m_DrawParamCount[d.firstDraw + k] > 0)
                    if (const MaterialAsset* material = assets.Get(materials[k]))
                        WriteParams(scene, assets, entity, d.firstDraw + k, k, *material);
        }
    }
    m_Instances[index] = {.entity      = entity,
                          .model       = renderer->model,
                          .meshIndex   = renderer->meshIndex,
                          .revision    = revision,
                          .mirrored    = mirrored,
                          .materialKey = materialKey,
                          .paramKey    = paramKey};

    GpuInstance&    data   = m_InstanceData[index];
    const glm::mat3 normal = glm::transpose(glm::inverse(linear));
    if (!existed) { // a reused slot keeps nothing of its previous owner
        data.prevModel       = world;
        data.jointOffset     = ~0u;
        data.jointCount      = 0;
        data.prevJointOffset = ~0u;
    } else if (index >= m_MovedFlag.size() || !m_MovedFlag[index]) {
        data.prevModel = data.model; // where it was drawn last frame
    }
    if (data.prevModel != world) {
        if (index >= m_MovedFlag.size())
            m_MovedFlag.resize(m_InstanceData.size(), false);
        if (!m_MovedFlag[index]) {
            m_MovedFlag[index] = true;
            m_Moved.push_back(index);
        }
    }
    data.model     = world;
    data.normal[0] = glm::vec4(normal[0], 0.0f);
    data.normal[1] = glm::vec4(normal[1], 0.0f);
    data.normal[2] = glm::vec4(normal[2], 0.0f);
    data.entityId  = slot + 1;
    data.flags     = mirrored ? kInstanceMirrored : 0u;

    if (newGeometry) {
        std::optional<std::uint32_t> first = m_DrawRanges.Allocate(count);
        if (!first) {
            // Visible entries address records with 22 bits (and one culling dispatch covers them).
            const std::uint32_t capacity = std::min(
                std::max({m_DrawRanges.Capacity() * 2, m_DrawRanges.Capacity() + count, 1024u}), kMaxDrawRecords);
            if (capacity > m_DrawRanges.Capacity()) {
                m_DrawRanges.Grow(capacity);
                m_Draws.resize(capacity);
                m_DrawSubmeshes.resize(capacity);
                m_DrawDirty.resize(capacity, false);
                m_DrawParamCount.resize(capacity, 0);
            }
            first = m_DrawRanges.Allocate(count);
            if (!first) {
                if (!std::exchange(m_ReportedFull, true))
                    ENGINE_ERROR("GPU scene: more than {} draw records - further meshes are not drawn", kMaxDrawRecords);
                data.drawCount = 0;
                MarkInstance(index);
                return;
            }
        }
        data.firstDraw = *first;
        data.drawCount = count;
        m_LiveDraws += count;
        for (std::uint32_t k = 0; k < count; ++k) {
            const std::uint32_t submeshId = mesh.firstGpuSubmesh + k;
            const std::uint32_t d         = *first + k;
            m_DrawSubmeshes[d] = model->gpuSubmeshes[submeshId - model->submeshes.offset];
            const MaterialAsset* material = assets.Get(materials[k]);
            std::uint32_t        materialSlot = ~0u;
            std::uint32_t        params   = 0;
            if (material) {
                const auto size = static_cast<std::uint32_t>(material->block.size());
                std::optional<std::uint32_t> range = m_ParamRanges.Allocate(size);
                if (!range) {
                    m_ParamRanges.Grow(std::max({m_ParamRanges.Capacity() * 2, m_ParamRanges.Capacity() + size, 256u}));
                    m_ParamData.resize(m_ParamRanges.Capacity());
                    range = m_ParamRanges.Allocate(size);
                }
                materialSlot        = AcquireMaterialSlot(materials[k], *material);
                params              = *range;
                m_DrawParamCount[d] = size;
            }
            m_Draws[d] = {.instance = index,
                          .submesh  = submeshId,
                          .batch    = AcquireBatch(m_DrawSubmeshes[d], submeshId, mirrored, materialSlot,
                                                   material ? assets.Revision(materials[k]) : 0u),
                          .params   = params};
            if (material)
                WriteParams(scene, assets, entity, d, k, *material);
            m_BlendDraws += IsBlendDraw(d) ? 1u : 0u;
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
    UnuseModel(inst.model);
    m_InstanceOf[EntityIndex(inst.entity)] = kNone;
    inst = {};
    m_FreeInstances.push_back(index);
    --m_LiveInstances;
}

void GpuScene::ReleaseDraws(std::uint32_t instance)
{
    GpuInstance& data = m_InstanceData[instance];
    for (std::uint32_t d = data.firstDraw; d < data.firstDraw + data.drawCount; ++d) {
        m_BlendDraws -= IsBlendDraw(d) ? 1u : 0u;
        const std::uint32_t material = m_Batches[m_Draws[d].batch].material;
        ReleaseBatch(m_Draws[d].batch);
        if (material != 0)
            ReleaseMaterialSlot(material - 1);
        if (m_DrawParamCount[d] > 0) {
            m_ParamRanges.Free(m_Draws[d].params, m_DrawParamCount[d]);
            m_DrawParamCount[d] = 0;
        }
        m_Draws[d] = {};
        MarkDraw(d);
    }
    m_DrawRanges.Free(data.firstDraw, data.drawCount);
    m_LiveDraws -= data.drawCount;
    data.drawCount = 0;
}

void GpuScene::ResolveMaterials(const Scene& scene, const AssetManager& assets, Entity entity, std::uint32_t submeshes,
                                std::vector<MaterialHandle>& out, std::uint64_t& materialKey, std::uint64_t& paramKey) const
{
    out.assign(submeshes, MaterialHandle{});
    materialKey = paramKey = 0;
    const MaterialOverrides* overrides = scene.GetRegistry().TryGet<MaterialOverrides>(entity);
    if (!overrides || !m_Resolve || !*m_Resolve)
        return;
    const auto mix = [](std::uint64_t h, std::uint64_t v) { return (h ^ v) * 0x100000001b3ull; };
    std::uint64_t key = 0xcbf29ce484222325ull;
    bool          any = false;
    for (std::uint32_t k = 0; k < submeshes && k < overrides->materials.size(); ++k) {
        if (overrides->materials[k].empty())
            continue;
        const MaterialHandle handle = (*m_Resolve)(overrides->materials[k]);
        if (!assets.Get(handle))
            continue; // loading / failed: the model's material meanwhile
        out[k] = handle;
        any    = true;
        key    = mix(mix(mix(key, k), (std::uint64_t{handle.generation} << 32) | handle.index), assets.Revision(handle));
    }
    if (!any)
        return;
    materialKey = key | 1u;
    std::uint64_t params = 0xcbf29ce484222325ull;
    for (const MaterialParameterOverride& p : overrides->parameters) {
        params = Fnv1a64(std::as_bytes(std::span(p.name.data(), p.name.size())), params);
        params = Fnv1a64(std::as_bytes(std::span(&p.value, 1)), params);
        params = mix(params, static_cast<std::uint32_t>(p.submesh));
    }
    paramKey = params;
}

void GpuScene::WriteParams(const Scene& scene, const AssetManager&, Entity entity, std::uint32_t draw,
                           std::uint32_t submesh, const MaterialAsset& material)
{
    const std::uint32_t base  = m_Draws[draw].params;
    const std::uint32_t count = std::min<std::uint32_t>(m_DrawParamCount[draw], static_cast<std::uint32_t>(material.block.size()));
    std::copy_n(material.block.begin(), count, m_ParamData.begin() + base);
    if (const MaterialOverrides* overrides = scene.GetRegistry().TryGet<MaterialOverrides>(entity))
        for (const MaterialParameterOverride& p : overrides->parameters) {
            if (p.submesh >= 0 && static_cast<std::uint32_t>(p.submesh) != submesh)
                continue;
            const std::uint32_t slot = material.ParameterSlot(p.name);
            if (slot >= count)
                continue;
            m_ParamData[base + slot] = material.slots[slot].type == MaterialParameterType::Scalar
                                           ? glm::vec4(p.value.x, 0.0f, 0.0f, 0.0f)
                                           : p.value;
        }
    m_ParamsDirty = true;
}

std::uint32_t GpuScene::AcquireMaterialSlot(MaterialHandle handle, const MaterialAsset& material)
{
    std::uint32_t slot = 0;
    if (const auto it = m_MaterialSlotOf.find(handle); it != m_MaterialSlotOf.end()) {
        slot = it->second;
    } else {
        if (!m_FreeMaterialSlots.empty()) {
            slot = m_FreeMaterialSlots.back();
            m_FreeMaterialSlots.pop_back();
        } else {
            slot = static_cast<std::uint32_t>(m_MaterialSlots.size());
            m_MaterialSlots.emplace_back();
        }
        m_MaterialSlotOf.emplace(handle, slot);
    }
    MaterialSlotInfo& info = m_MaterialSlots[slot];
    info.handle            = handle;
    info.settings          = material.settings; // latest revision (older batches die in this Update)
    info.shaders           = material.shaders;
    ++info.draws;
    return slot;
}

void GpuScene::ReleaseMaterialSlot(std::uint32_t slot)
{
    MaterialSlotInfo& info = m_MaterialSlots[slot];
    if (--info.draws > 0)
        return;
    m_MaterialSlotOf.erase(info.handle);
    info = {};
    m_FreeMaterialSlots.push_back(slot);
}

std::uint32_t GpuScene::BucketOffset(bool shadow, std::uint32_t bucket) const
{
    const std::size_t i = shadow ? m_BucketCameraCount + bucket : bucket;
    return (shadow ? bucket < m_BucketData.size() - m_BucketCameraCount : bucket < m_BucketCameraCount) && i < m_BucketData.size()
               ? m_BucketData[i]
               : 0u;
}

std::uint32_t GpuScene::BucketCapacity(bool shadow, std::uint32_t bucket) const
{
    const std::size_t i = shadow ? m_BucketCameraCount + bucket : bucket;
    return (shadow ? bucket < m_BucketCapacity.size() - m_BucketCameraCount : bucket < m_BucketCameraCount) &&
                   i < m_BucketCapacity.size()
               ? m_BucketCapacity[i]
               : 0u;
}

std::uint32_t GpuScene::AcquireBatch(const GpuSubmesh& submesh, std::uint32_t submeshIndex, bool mirrored,
                                     std::uint32_t materialSlot, std::uint32_t materialRevision)
{
    // Custom materials: their own batches per revision (a reload may change the buckets; the old
    // batches die with the re-upserts of this Update).
    const std::uint64_t material = materialSlot == ~0u ? 0u : materialSlot + 1u;
    const std::uint64_t key      = (mirrored ? 1u : 0u) | (std::uint64_t{submeshIndex} << 1) | (material << 25) |
                              (std::uint64_t{materialRevision & 0xFFFFFFu} << 40);
    m_BatchesDirty          = true; // capacities (instance bases) change
    if (const auto it = m_BatchOf.find(key); it != m_BatchOf.end()) {
        for (std::uint32_t l = 0; l < m_BatchSizes[it->second]; ++l)
            ++m_BatchRefs[it->second + l];
        return it->second;
    }
    const std::uint32_t lods  = std::clamp(submesh.lodCount, 1u, kMaxLods);
    std::optional<std::uint32_t> first = m_BatchRanges.Allocate(lods);
    if (!first) {
        const std::uint32_t capacity = std::max({m_BatchRanges.Capacity() * 2, m_BatchRanges.Capacity() + lods, 256u});
        m_BatchRanges.Grow(capacity);
        m_Batches.resize(capacity);
        m_BatchRefs.resize(capacity, 0);
        m_BatchKeys.resize(capacity, 0);
        m_BatchSizes.resize(capacity, 0);
        first = m_BatchRanges.Allocate(lods);
    }
    const std::uint32_t base = *first;
    std::uint32_t cameraBucket = (submesh.flags & kMaterialAlphaBlend) != 0
                                     ? kCameraBucketBlend
                                     : ((submesh.flags & kMaterialDoubleSided) != 0 ? 1u : 0u) | (mirrored ? 2u : 0u);
    // Blended surfaces cast alpha-tested shadows (cutoff alphaCutoff).
    std::uint32_t shadowBucket = (submesh.flags & (kMaterialAlphaMask | kMaterialAlphaBlend)) != 0 ? 1u : 0u;
    float         displacement = 0.0f;
    if (materialSlot != ~0u) {
        const MaterialSettings& s = m_MaterialSlots[materialSlot].settings;
        const bool blend = s.blendMode == MaterialBlendMode::Translucent || s.blendMode == MaterialBlendMode::Additive;
        cameraBucket = blend ? kCameraBucketBlend : kCustomCameraBucketBase + 2 * materialSlot + (mirrored ? 1u : 0u);
        shadowBucket = !blend && s.castShadows ? kShadowBuckets + materialSlot : kBucketNone;
        displacement = std::max(s.maxDisplacement, 0.0f);
    }
    for (std::uint32_t l = 0; l < lods; ++l) {
        const auto i = static_cast<glm::length_t>(l);
        m_Batches[base + l] = {.indexCount   = submesh.lodIndexCount[i],
                               .firstIndex   = submesh.lodFirstIndex[i],
                               .vertexOffset = submesh.vertexOffset,
                               .instanceBase = 0,
                               .cameraBucket = cameraBucket,
                               .shadowBucket = shadowBucket,
                               .displacement = displacement,
                               .material     = static_cast<std::uint32_t>(material)};
        m_BatchRefs[base + l] = 1;
    }
    m_BatchKeys[base]  = key;
    m_BatchSizes[base] = lods;
    m_BatchOf.emplace(key, base);
    return base;
}

void GpuScene::ReleaseBatch(std::uint32_t batch)
{
    m_BatchesDirty          = true;
    const std::uint32_t lods = m_BatchSizes[batch];
    for (std::uint32_t l = 0; l < lods; ++l)
        --m_BatchRefs[batch + l];
    if (m_BatchRefs[batch] > 0)
        return;
    m_BatchOf.erase(m_BatchKeys[batch]);
    for (std::uint32_t l = 0; l < lods; ++l)
        m_Batches[batch + l] = {}; // indexCount 0: never produces a command
    m_BatchSizes[batch] = 0;
    m_BatchRanges.Free(batch, lods);
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
        m_VisibleCapacity = base;

        // Bucket layout: per view, the commands of bucket b start at its offset (prefix sum of
        // the batches in the buckets before it).
        const std::uint32_t cameraCount = CameraBucketCount();
        const std::uint32_t shadowCount = ShadowBucketCount();
        m_BucketCapacity.assign(cameraCount + shadowCount, 0u);
        for (std::size_t b = 0; b < m_Batches.size(); ++b) {
            if (m_BatchRefs[b] == 0)
                continue;
            if (m_Batches[b].cameraBucket < cameraCount)
                ++m_BucketCapacity[m_Batches[b].cameraBucket];
            if (m_Batches[b].shadowBucket < shadowCount)
                ++m_BucketCapacity[cameraCount + m_Batches[b].shadowBucket];
        }
        m_BucketData.assign(cameraCount + shadowCount, 0u);
        for (const auto& [first, count] : {std::pair{0u, cameraCount}, std::pair{cameraCount, shadowCount}}) {
            std::uint32_t offset = 0;
            for (std::uint32_t i = first; i < first + count; ++i) {
                m_BucketData[i] = offset;
                offset += m_BucketCapacity[i];
            }
        }
        m_BucketCameraCount = cameraCount;
    }

    const bool newInstances = Ensure(m_Renderer, m_InstanceBuffer, m_InstanceData.size(), sizeof(GpuInstance), "GpuInstances");
    const bool newDraws     = Ensure(m_Renderer, m_DrawBuffer, m_Draws.size(), sizeof(GpuDraw), "GpuDraws");
    const bool newBatches   = Ensure(m_Renderer, m_BatchBuffer, m_Batches.size(), sizeof(GpuBatch), "GpuBatches");
    const bool newVisible   = Ensure(m_Renderer, m_Visibility, m_Draws.size(), sizeof(std::uint32_t), "GpuDrawVisibility");
    m_JointBuffer ^= 1u; // the other buffer keeps last frame's palettes
    const bool newJoints    = Ensure(m_Renderer, m_JointMatrices[m_JointBuffer], m_JointMatrixData.size(), sizeof(glm::mat4), "GpuJointMatrices");
    const bool newBuckets   = Ensure(m_Renderer, m_BucketBuffer, m_BucketData.size(), sizeof(std::uint32_t), "GpuBuckets");
    const bool newParams    = Ensure(m_Renderer, m_ParamBuffer, m_ParamData.size(), sizeof(glm::vec4), "GpuMaterialParams");
    m_BatchesDirty |= newBatches;
    m_ParamsDirty |= newParams;
    const bool bucketsDirty = (m_BatchesDirty || newBuckets) && !m_BucketData.empty();

    const bool fullInstances = newInstances && !m_InstanceData.empty();
    const bool fullDraws     = newDraws && !m_Draws.empty();
    const bool work = newVisible || fullInstances || fullDraws || !m_DirtyInstances.empty() || !m_DirtyDraws.empty() ||
                      (m_BatchesDirty && !m_Batches.empty()) || newJoints || !m_JointMatrixData.empty() || bucketsDirty ||
                      (m_ParamsDirty && !m_ParamData.empty());
    if (!work)
        return;

    // Earlier frames' culling, vertex and (material parameters) fragment shaders read these buffers (WAR).
    Barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
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
    if (bucketsDirty)
        Scatter(cmd, m_BucketBuffer.Address(), m_BucketData.data(), sizeof(std::uint32_t), {},
                static_cast<std::uint32_t>(m_BucketData.size()));
    if (m_ParamsDirty && !m_ParamData.empty())
        Scatter(cmd, m_ParamBuffer.Address(), m_ParamData.data(), sizeof(glm::vec4), {},
                static_cast<std::uint32_t>(m_ParamData.size()));
    m_ParamsDirty = false;
    if (!m_JointMatrixData.empty())
        Scatter(cmd, m_JointMatrices[m_JointBuffer].Address(), m_JointMatrixData.data(), sizeof(glm::mat4), {},
                static_cast<std::uint32_t>(m_JointMatrixData.size()));
    m_BatchesDirty = false;

    Barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

} // namespace Engine
