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

    GeometryPool& pool = renderer.Geometry();
    out.name     = data.name;
    out.vertices = pool.Upload(GeometryKind::Vertices, std::span{data.vertices}, ticket);
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
    out.nodes     = data.nodes;
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
                          model.nodes.size() * sizeof(ModelNode) + model.materialFlags.size() * sizeof(std::uint32_t) +
                          model.gpuSubmeshes.size() * sizeof(GpuSubmesh);
    for (const Mesh& mesh : model.meshes)
        bytes += sizeof(Mesh) + mesh.submeshes.size() * sizeof(Submesh);
    return bytes;
}

Entity InstantiateModel(Scene& scene, ModelHandle handle, const Model& model, Entity parent)
{
    Registry&           registry = scene.GetRegistry();
    const Entity        root     = scene.CreateEntity(model.name, parent);
    std::vector<Entity> entities(model.nodes.size(), NullEntity);

    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const ModelNode& node       = model.nodes[i];
        const Entity     nodeParent = node.parent >= 0 ? entities[static_cast<std::size_t>(node.parent)] : root;
        const Entity     e          = scene.CreateEntity(node.name, nodeParent);
        scene.SetTransform(e, node.local);
        if (node.mesh >= 0)
            registry.Emplace<MeshRenderer>(e, handle, static_cast<std::uint32_t>(node.mesh));
        if (node.light)
            registry.Emplace<Light>(e, *node.light);
        entities[i] = e;
    }
    return root;
}

} // namespace Engine
