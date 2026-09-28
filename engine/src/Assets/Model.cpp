#include "Engine/Assets/Model.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Scene/Scene.h"

#include <span>
#include <stdexcept>

namespace Engine {

void BuildModel(Renderer& renderer, const ModelData& data, Model& out, UploadTicket& ticket)
{
    if (data.vertices.empty() || data.indices.empty())
        throw std::runtime_error("BuildModel: '" + data.name + "' contains no geometry");
    if (data.materials.empty())
        throw std::runtime_error("BuildModel: '" + data.name + "' has no materials");

    UploadQueue&      uploader = renderer.GetUploader();
    BindlessRegistry& bindless = renderer.GetBindless();

    out.name         = data.name;
    out.vertexBuffer = uploader.CreateBuffer(std::as_bytes(std::span{data.vertices}),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ticket, "ModelVertices");
    out.indexBuffer  = uploader.CreateBuffer(std::as_bytes(std::span{data.indices}),
                                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT, ticket, "ModelIndices");

    // The slot may be written now: nothing samples it until the material buffer is resident.
    out.textures.reserve(data.textures.size());
    out.bindlessTextures.reserve(data.textures.size());
    for (const TextureData& tex : data.textures) {
        out.textures.push_back(uploader.CreateTexture2D(
            {.pixels    = tex.pixels.data(),
             .width     = tex.width,
             .height    = tex.height,
             .format    = tex.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
             .debugName = tex.name.c_str()},
            ticket));
        out.bindlessTextures.push_back(bindless.AddSampledImage(out.textures.back().View()));
    }

    const auto slot = [&](std::int32_t texture, DefaultTexture fallback) {
        return texture >= 0 ? out.bindlessTextures[static_cast<std::size_t>(texture)]
                            : renderer.DefaultTextureIndex(fallback);
    };

    std::vector<GpuMaterial> materials;
    materials.reserve(data.materials.size());
    for (const MaterialData& m : data.materials) {
        const std::uint32_t flags = (m.alphaMask ? kMaterialAlphaMask : 0u) |
                                    (m.doubleSided ? kMaterialDoubleSided : 0u) |
                                    (m.alphaBlend ? kMaterialAlphaBlend : 0u);
        materials.push_back({.baseColorFactor          = m.baseColorFactor,
                             .emissiveFactor           = glm::vec4(m.emissiveFactor, 0.0f),
                             .metallic                 = m.metallic,
                             .roughness                = m.roughness,
                             .alphaCutoff              = m.alphaCutoff,
                             .flags                    = flags,
                             .baseColorTexture         = slot(m.baseColorTexture, DefaultTexture::White),
                             .normalTexture            = slot(m.normalTexture, DefaultTexture::FlatNormal),
                             .metallicRoughnessTexture = slot(m.metallicRoughnessTexture, DefaultTexture::White),
                             .emissiveTexture          = slot(m.emissiveTexture, DefaultTexture::White),
                             .occlusionTexture         = slot(m.occlusionTexture, DefaultTexture::White),
                             .samplerIndex             = static_cast<std::uint32_t>(DefaultSampler::LinearRepeat),
                             .normalScale              = m.normalScale,
                             .occlusionStrength        = m.occlusionStrength});
        out.materialFlags.push_back(flags);
    }
    out.materialBuffer = uploader.CreateBuffer(std::as_bytes(std::span{materials}),
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ticket, "ModelMaterials");

    out.meshes    = data.meshes;
    out.nodes     = data.nodes;
    out.boundsMin = data.boundsMin;
    out.boundsMax = data.boundsMax;
}

void ReleaseModel(Renderer& renderer, Model&& model)
{
    Renderer* r = &renderer;
    renderer.DeferCall([r, slots = std::move(model.bindlessTextures)] {
        for (std::uint32_t s : slots)
            r->GetBindless().RemoveSampledImage(s);
    });
    renderer.DeferRelease(std::move(model));
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
        registry.Get<Transform>(e) = node.local;
        if (node.mesh >= 0)
            registry.Emplace<MeshRenderer>(e, handle, static_cast<std::uint32_t>(node.mesh));
        if (node.light)
            registry.Emplace<Light>(e, *node.light);
        entities[i] = e;
    }
    return root;
}

} // namespace Engine
