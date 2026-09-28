#include "Engine/Assets/Model.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Scene/Scene.h"

#include <span>
#include <stdexcept>

namespace Engine {

std::shared_ptr<Model> UploadModel(Renderer& renderer, const ModelData& data)
{
    if (data.vertices.empty() || data.indices.empty())
        throw std::runtime_error("UploadModel: '" + data.name + "' contains no geometry");

    auto& uploader = renderer.GetUploader();
    auto& bindless = renderer.GetBindless();
    auto  model    = std::make_unique<Model>();

    model->name         = data.name;
    model->vertexBuffer = uploader.CreateBuffer(std::as_bytes(std::span{data.vertices}),
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "ModelVertices");
    model->indexBuffer  = uploader.CreateBuffer(std::as_bytes(std::span{data.indices}),
                                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "ModelIndices");

    model->textures.reserve(data.textures.size());
    for (const TextureData& tex : data.textures) {
        model->textures.push_back(uploader.CreateTexture2D(
            {.pixels    = tex.pixels.data(),
             .width     = tex.width,
             .height    = tex.height,
             .format    = tex.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
             .debugName = tex.name.c_str()}));
        model->bindlessTextures.push_back(bindless.AddSampledImage(model->textures.back().View()));
    }

    const auto slot = [&](std::int32_t texture, DefaultTexture fallback) {
        return texture >= 0 ? model->bindlessTextures[static_cast<std::size_t>(texture)]
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
                             .samplerIndex = static_cast<std::uint32_t>(DefaultSampler::LinearRepeat),
                             .padding      = {}});
        model->materialFlags.push_back(flags);
    }
    if (materials.empty())
        throw std::runtime_error("UploadModel: '" + data.name + "' has no materials");
    model->materialBuffer = uploader.CreateBuffer(std::as_bytes(std::span{materials}),
                                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "ModelMaterials");

    model->meshes    = data.meshes;
    model->nodes     = data.nodes;
    model->boundsMin = data.boundsMin;
    model->boundsMax = data.boundsMax;

    // Custom deleter: GPU memory and bindless slots are released only after in-flight frames finish.
    Renderer* r = &renderer;
    return std::shared_ptr<Model>(model.release(), [r](Model* m) {
        r->DeferCall([r, slots = std::move(m->bindlessTextures)] {
            for (std::uint32_t s : slots)
                r->GetBindless().RemoveSampledImage(s);
        });
        r->DeferRelease(std::move(*m));
        delete m;
    });
}

Entity InstantiateModel(Scene& scene, const std::shared_ptr<const Model>& model, Entity parent)
{
    Registry&           registry = scene.GetRegistry();
    const Entity        root     = scene.CreateEntity(model->name, parent);
    std::vector<Entity> entities(model->nodes.size(), NullEntity);

    for (std::size_t i = 0; i < model->nodes.size(); ++i) {
        const ModelNode& node = model->nodes[i];
        const Entity nodeParent = node.parent >= 0 ? entities[static_cast<std::size_t>(node.parent)] : root;
        const Entity e          = scene.CreateEntity(node.name, nodeParent);
        registry.Get<Transform>(e) = node.local;
        if (node.mesh >= 0)
            registry.Emplace<MeshRenderer>(e, model, static_cast<std::uint32_t>(node.mesh));
        entities[i] = e;
    }
    return root;
}

} // namespace Engine
