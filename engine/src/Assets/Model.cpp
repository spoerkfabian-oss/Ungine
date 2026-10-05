#include "Engine/Assets/Model.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <span>
#include <stdexcept>

namespace Engine {

std::uint64_t TextureContentHash(std::span<const std::byte> bytes)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (std::byte b : bytes)
        hash = (hash ^ static_cast<std::uint8_t>(b)) * 1099511628211ull;
    return hash;
}

void BuildModelGeometry(Renderer& renderer, const ModelData& data, Model& out, UploadTicket& ticket)
{
    if (data.vertices.empty() || data.indices.empty())
        throw std::runtime_error("BuildModel: '" + data.name + "' contains no geometry");
    if (!data.skinInfluences.empty() && data.skinInfluences.size() != data.vertices.size())
        throw std::runtime_error("BuildModel: '" + data.name + "' has mismatched skin influences");

    GeometryPool& pool = renderer.Geometry();
    out.name     = data.name;
    std::vector<Vertex> vertices = data.vertices;
    if (!data.skinInfluences.empty())
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            vertices[i].joints = glm::uvec4(data.skinInfluences[i].joints);
            vertices[i].weights = data.skinInfluences[i].weights;
        }
    out.vertices = pool.Upload(GeometryKind::Vertices, std::span<const Vertex>{vertices}, ticket);
    out.indices  = pool.Upload(GeometryKind::Indices, std::span{data.indices}, ticket);

    out.meshes = data.meshes;
    for (Mesh& mesh : out.meshes)
        for (Submesh& sm : mesh.submeshes) {
            if (sm.lodCount <= 1) {
                sm.lodCount = 1;
                sm.lods[0]  = {.firstIndex = sm.firstIndex, .indexCount = sm.indexCount, .error = 0.0f};
            }
            sm.lodCount = std::min(sm.lodCount, kMaxLods);
            for (std::uint32_t l = 0; l < sm.lodCount; ++l)
                if (std::uint64_t{sm.lods[l].firstIndex} + sm.lods[l].indexCount > data.indices.size())
                    throw std::runtime_error("BuildModel: '" + data.name + "' has an index range out of bounds");
        }
    out.nodes      = data.nodes;
    out.skinInfluences = data.skinInfluences;
    out.skins      = data.skins;
    out.animations = data.animations;
    out.boundsMin = data.boundsMin;
    out.boundsMax = data.boundsMax;

    out.collisionPositions.reserve(data.vertices.size());
    for (const Vertex& v : data.vertices)
        out.collisionPositions.push_back(v.position);
    out.collisionIndices = data.indices; // LOD ranges included but never referenced by colliders
}

void BuildModelMaterials(Renderer& renderer, std::span<const MaterialData> materials,
                         std::span<const std::uint32_t> textureEntries, Model& out, UploadTicket& ticket)
{
    out.previewMaterials.assign(materials.begin(), materials.end());
    if (materials.empty())
        throw std::runtime_error("BuildModel: '" + out.name + "' has no materials");

    const auto entry = [&](std::int32_t texture, DefaultTexture fallback) {
        if (texture >= 0) {
            if (static_cast<std::size_t>(texture) >= textureEntries.size())
                throw std::runtime_error("BuildModel: '" + out.name + "' references a missing texture");
            return textureEntries[static_cast<std::size_t>(texture)];
        }
        // Default textures occupy the first table entries (entry == enum value).
        return static_cast<std::uint32_t>(texture == kErrorTexture ? DefaultTexture::Error : fallback);
    };

    std::vector<GpuMaterial> gpu;
    gpu.reserve(materials.size());
    out.materialFlags.clear();
    for (const MaterialData& m : materials) {
        const std::uint32_t flags = (m.alphaMask ? kMaterialAlphaMask : 0u) |
                                    (m.doubleSided ? kMaterialDoubleSided : 0u) |
                                    (m.alphaBlend ? kMaterialAlphaBlend : 0u);
        gpu.push_back({.baseColorFactor          = m.baseColorFactor,
                       .emissiveFactor           = glm::vec4(m.emissiveFactor, 0.0f),
                       .metallic                 = m.metallic,
                       .roughness                = m.roughness,
                       .alphaCutoff              = m.alphaCutoff,
                       .flags                    = flags,
                       .baseColorTexture         = entry(m.baseColorTexture, DefaultTexture::White),
                       .normalTexture            = entry(m.normalTexture, DefaultTexture::FlatNormal),
                       .metallicRoughnessTexture = entry(m.metallicRoughnessTexture, DefaultTexture::White),
                       .emissiveTexture          = entry(m.emissiveTexture, DefaultTexture::White),
                       .occlusionTexture         = entry(m.occlusionTexture, DefaultTexture::White),
                       .samplerIndex             = static_cast<std::uint32_t>(DefaultSampler::LinearRepeat),
                       .normalScale              = m.normalScale,
                       .occlusionStrength        = m.occlusionStrength});
        out.materialFlags.push_back(flags);
    }
    GeometryPool& pool = renderer.Geometry();
    out.materials      = pool.Upload(GeometryKind::Materials, std::span<const GpuMaterial>{gpu}, ticket);

    // Submesh records with absolute pool offsets (GPU culling, indirect draws).
    std::vector<GpuSubmesh> records;
    for (Mesh& mesh : out.meshes) {
        mesh.firstGpuSubmesh = static_cast<std::uint32_t>(records.size()); // made absolute below
        for (const Submesh& sm : mesh.submeshes) {
            if (sm.material >= materials.size())
                throw std::runtime_error("BuildModel: '" + out.name + "' references a missing material");
            GpuSubmesh record{.firstIndex   = out.indices.offset + sm.firstIndex,
                              .indexCount   = sm.indexCount,
                              .vertexOffset = static_cast<std::int32_t>(out.vertices.offset) + sm.vertexOffset,
                              .material     = out.materials.offset + sm.material,
                              .boundsMin    = sm.boundsMin,
                              .flags        = out.materialFlags[sm.material],
                              .boundsMax    = sm.boundsMax,
                              .lodCount     = sm.lodCount};
            for (std::uint32_t l = 0; l < sm.lodCount; ++l) {
                const auto i = static_cast<glm::length_t>(l);
                record.lodFirstIndex[i] = out.indices.offset + sm.lods[l].firstIndex;
                record.lodIndexCount[i] = sm.lods[l].indexCount;
                record.lodError[i]      = sm.lods[l].error;
            }
            records.push_back(record);
        }
    }
    if (!records.empty()) {
        out.submeshes = pool.Upload(GeometryKind::Submeshes, std::span<const GpuSubmesh>{records}, ticket);
        for (Mesh& mesh : out.meshes)
            mesh.firstGpuSubmesh += out.submeshes.offset;
    }
    out.gpuSubmeshes = std::move(records);
}

void BuildModel(Renderer& renderer, const ModelData& data, Model& out, UploadTicket& ticket)
{
    if (!data.textures.empty())
        throw std::runtime_error("BuildModel: '" + data.name + "' has textures - load it through the AssetManager");
    BuildModelGeometry(renderer, data, out, ticket);
    BuildModelMaterials(renderer, data.materials, {}, out, ticket);
}

void ReleaseModel(Renderer& renderer, Model&& model)
{
    Renderer* r = &renderer;
    renderer.DeferCall([r, vertices = model.vertices, indices = model.indices, materials = model.materials,
                        submeshes = model.submeshes] {
        GeometryPool& pool = r->Geometry();
        pool.Free(GeometryKind::Vertices, vertices); // empty ranges (failed builds) are ignored
        pool.Free(GeometryKind::Indices, indices);
        pool.Free(GeometryKind::Materials, materials);
        pool.Free(GeometryKind::Submeshes, submeshes);
    });
    renderer.DeferRelease(std::move(model));
}

std::uint64_t ModelGpuBytes(const Model& model)
{
    return std::uint64_t{model.vertices.count} * GeometryPool::Stride(GeometryKind::Vertices) +
           std::uint64_t{model.indices.count} * GeometryPool::Stride(GeometryKind::Indices) +
           std::uint64_t{model.materials.count} * GeometryPool::Stride(GeometryKind::Materials) +
           std::uint64_t{model.submeshes.count} * GeometryPool::Stride(GeometryKind::Submeshes);
}

std::uint64_t ModelCpuBytes(const Model& model)
{
    std::uint64_t bytes = model.collisionPositions.size() * sizeof(glm::vec3) +
                          model.collisionIndices.size() * sizeof(std::uint32_t) +
                          model.skinInfluences.size() * sizeof(VertexSkinInfluence) +
                          model.nodes.size() * sizeof(ModelNode) + model.materialFlags.size() * sizeof(std::uint32_t) +
                          model.gpuSubmeshes.size() * sizeof(GpuSubmesh);
    for (const Mesh& mesh : model.meshes)
        bytes += sizeof(Mesh) + mesh.submeshes.size() * sizeof(Submesh);
    for (const Skin& skin : model.skins)
        bytes += skin.name.capacity() + skin.joints.capacity() * sizeof(SkinJoint);
    for (const AnimationClip& clip : model.animations) {
        bytes += clip.name.capacity() + clip.tracks.capacity() * sizeof(AnimationTrack);
        for (const AnimationTrack& track : clip.tracks)
            bytes += track.times.capacity() * sizeof(float) +
                     (track.values.capacity() + track.inTangents.capacity() + track.outTangents.capacity()) *
                         sizeof(glm::vec4);
    }
    return bytes;
}

Entity InstantiateModel(Scene& scene, ModelHandle handle, const Model& model, Entity parent)
{
    Registry&           registry = scene.GetRegistry();
    const Entity        root     = scene.CreateEntity(model.name, parent);
    std::vector<Entity> entities(model.nodes.size(), NullEntity);
    registry.Emplace<ModelInstance>(root, ModelInstance{.model = handle});
    if (!model.animations.empty())
        registry.Emplace<Animator>(root);

    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const ModelNode& node       = model.nodes[i];
        const Entity     nodeParent = node.parent >= 0 ? entities[static_cast<std::size_t>(node.parent)] : root;
        const Entity     e          = scene.CreateEntity(node.name, nodeParent);
        scene.SetTransform(e, node.local);
        registry.Emplace<ModelNodeRef>(e, ModelNodeRef{.node = static_cast<std::uint32_t>(i), .instanceRoot = root});
        if (node.mesh >= 0)
            registry.Emplace<MeshRenderer>(e, handle, static_cast<std::uint32_t>(node.mesh));
        if (node.light)
            registry.Emplace<Light>(e, *node.light);
        entities[i] = e;
    }
    return root;
}

std::size_t RefreshModelInstances(Scene& scene, ModelHandle handle, const Model& model)
{
    Registry&           registry = scene.GetRegistry();
    std::vector<Entity> roots;
    registry.ViewOf<ModelInstance>().Each([&](Entity e, ModelInstance& instance) {
        if (instance.model == handle)
            roots.push_back(e);
    });

    for (const Entity root : roots) {
        if (!model.animations.empty()) {
            if (!registry.Has<Animator>(root))
                registry.Emplace<Animator>(root);
        } else {
            registry.Remove<Animator>(root);
        }
        // This instance's node entities (nested instances are left alone).
        std::vector<Entity> existing;
        std::vector<Entity> stack(registry.Get<Hierarchy>(root).children);
        while (!stack.empty()) {
            const Entity e = stack.back();
            stack.pop_back();
            if (registry.Has<ModelInstance>(e))
                continue;
            if (registry.Has<ModelNodeRef>(e))
                existing.push_back(e);
            const auto& children = registry.Get<Hierarchy>(e).children;
            stack.insert(stack.end(), children.begin(), children.end());
        }
        // Match by name (exports keep names when nodes are added or removed), same index first.
        std::vector<Entity> byNode(model.nodes.size(), NullEntity);
        std::vector<bool>   used(existing.size(), false);
        for (int pass = 0; pass < 2; ++pass)
            for (std::size_t i = 0; i < model.nodes.size(); ++i)
                for (std::size_t k = 0; k < existing.size() && byNode[i] == NullEntity; ++k)
                    if (!used[k] && registry.Get<Name>(existing[k]).value == model.nodes[i].name &&
                        (pass == 1 || registry.Get<ModelNodeRef>(existing[k]).node == i)) {
                        byNode[i] = existing[k];
                        used[k]   = true;
                    }
        std::vector<Entity> stale;
        for (std::size_t k = 0; k < existing.size(); ++k)
            if (!used[k])
                stale.push_back(existing[k]);

        for (std::size_t i = 0; i < model.nodes.size(); ++i) { // parents first
            const ModelNode& node   = model.nodes[i];
            const Entity     parent = node.parent >= 0 ? byNode[static_cast<std::size_t>(node.parent)] : root;
            Entity&          e      = byNode[i];
            if (e == NullEntity) {
                e = scene.CreateEntity(node.name, parent);
                registry.Emplace<ModelNodeRef>(e, ModelNodeRef{.node = static_cast<std::uint32_t>(i), .instanceRoot = root});
            } else {
                if (registry.Get<Hierarchy>(e).parent != parent)
                    scene.SetParent(e, parent);
                registry.Get<ModelNodeRef>(e).node = static_cast<std::uint32_t>(i);
                registry.Get<ModelNodeRef>(e).instanceRoot = root;
            }
            scene.SetTransform(e, node.local);
            if (node.mesh >= 0)
                registry.EmplaceOrReplace<MeshRenderer>(e, MeshRenderer{handle, static_cast<std::uint32_t>(node.mesh)});
            else if (const MeshRenderer* mesh = registry.TryGet<MeshRenderer>(e); mesh && mesh->model == handle)
                registry.Remove<MeshRenderer>(e);
            if (node.light)
                registry.EmplaceOrReplace<Light>(e, *node.light);
            else
                registry.Remove<Light>(e);
            scene.MarkChanged(e);
        }

        // Nodes the model no longer has: keep what the user attached to them.
        for (const Entity e : stale) {
            if (!registry.Valid(e))
                continue;
            const std::vector<Entity> children = registry.Get<Hierarchy>(e).children;
            for (const Entity child : children)
                if (std::ranges::find(stale, child) == stale.end())
                    scene.SetParent(child, root);
            scene.DestroyEntity(e);
        }
    }
    return roots.size();
}

} // namespace Engine
