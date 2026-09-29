#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Renderer/GeometryPool.h"
#include "Engine/Renderer/Vulkan/Buffer.h"
#include "Engine/Renderer/Vulkan/Image.h"
#include "Engine/Renderer/Vulkan/Upload.h"
#include "Engine/Scene/Components.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

class Renderer;
class Scene;

// 48 bytes, std430-compatible (uv split into the vec3 padding slots). Mirrors mesh_common.glsl.
struct Vertex {
    glm::vec3 position{0.0f};
    float     uvX = 0.0f;
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float     uvY = 0.0f;
    glm::vec4 tangent{0.0f}; // w = bitangent sign; w == 0: none, the shader derives a frame
};
static_assert(sizeof(Vertex) == 48);

struct Submesh {
    std::uint32_t firstIndex   = 0;
    std::uint32_t indexCount   = 0;
    std::int32_t  vertexOffset = 0;
    std::uint32_t material     = 0;
    glm::vec3     boundsMin{0.0f}; // object space, for frustum culling
    glm::vec3     boundsMax{0.0f};
};

struct Mesh {
    std::string          name;
    std::vector<Submesh> submeshes;
    std::uint32_t        firstGpuSubmesh = 0; // Model: index of submeshes[0] in the geometry pool's submesh records
};

// Nodes are stored parents-before-children (topological order).
struct ModelNode {
    std::string          name;
    Transform            local;
    std::int32_t         mesh   = -1;
    std::int32_t         parent = -1;
    std::optional<Light> light; // KHR_lights_punctual (point / spot)
};

inline constexpr std::int32_t kNoTexture = -1;

struct MaterialData {
    std::string  name;
    glm::vec4    baseColorFactor{1.0f};
    glm::vec3    emissiveFactor{0.0f};
    float        metallic          = 1.0f;
    float        roughness         = 1.0f;
    float        alphaCutoff       = 0.5f;
    float        normalScale       = 1.0f; // normalTexture.scale
    float        occlusionStrength = 1.0f; // occlusionTexture.strength
    bool         alphaMask         = false;
    bool         alphaBlend        = false;
    bool         doubleSided       = false;
    std::int32_t baseColorTexture         = kNoTexture; // indices into ModelData::textures
    std::int32_t normalTexture            = kNoTexture;
    std::int32_t metallicRoughnessTexture = kNoTexture;
    std::int32_t emissiveTexture          = kNoTexture;
    std::int32_t occlusionTexture         = kNoTexture;
};

struct TextureData {
    std::string               name;
    std::vector<std::uint8_t> pixels; // RGBA8
    std::uint32_t             width  = 0;
    std::uint32_t             height = 0;
    bool                      srgb   = true;
};

// CPU-side parse result. No GPU access -> can be produced on a worker thread.
struct ModelData {
    std::string                name;
    std::vector<Vertex>        vertices;
    std::vector<std::uint32_t> indices;
    std::vector<TextureData>   textures;
    std::vector<MaterialData>  materials;
    std::vector<Mesh>          meshes;
    std::vector<ModelNode>     nodes;
    glm::vec3                  boundsMin{0.0f}; // world space of the default scene
    glm::vec3                  boundsMax{0.0f};
};

// GPU material, mirrors `Material` in mesh_common.glsl.
struct GpuMaterial {
    glm::vec4     baseColorFactor;
    glm::vec4     emissiveFactor;
    float         metallic;
    float         roughness;
    float         alphaCutoff;
    std::uint32_t flags;
    std::uint32_t baseColorTexture; // bindless indices
    std::uint32_t normalTexture;
    std::uint32_t metallicRoughnessTexture;
    std::uint32_t emissiveTexture;
    std::uint32_t occlusionTexture;
    std::uint32_t samplerIndex;
    float         normalScale;
    float         occlusionStrength;
};
static_assert(sizeof(GpuMaterial) == 80);

// Submesh record in the geometry pool (GPU culling and indirect draws), mirrors GpuSubmesh in
// scene_common.glsl. Offsets are absolute pool indices.
struct GpuSubmesh {
    std::uint32_t firstIndex   = 0;
    std::uint32_t indexCount   = 0;
    std::int32_t  vertexOffset = 0;
    std::uint32_t material     = 0;
    glm::vec3     boundsMin{0.0f}; // object space
    std::uint32_t flags = 0;       // material flags (kMaterial*)
    glm::vec3     boundsMax{0.0f};
    std::uint32_t pad = 0;
};
static_assert(sizeof(GpuSubmesh) == 48);

inline constexpr std::uint32_t kMaterialAlphaMask   = 1u << 0;
inline constexpr std::uint32_t kMaterialDoubleSided = 1u << 1;
inline constexpr std::uint32_t kMaterialAlphaBlend  = 1u << 2;

// GPU-resident model: its geometry lives in ranges of the renderer's GeometryPool.
struct Model {
    std::string                name;
    PoolRange                  vertices;  // Submesh::vertexOffset is relative to vertices.offset
    PoolRange                  indices;   // Submesh::firstIndex is relative to indices.offset
    PoolRange                  materials; // Submesh::material is relative to materials.offset
    PoolRange                  submeshes; // GpuSubmesh records of all meshes (Mesh::firstGpuSubmesh)
    std::vector<Image>         textures;
    std::vector<std::uint32_t> bindlessTextures;
    std::vector<std::uint32_t> materialFlags; // CPU copy for pipeline selection
    std::vector<Mesh>          meshes;
    std::vector<ModelNode>     nodes;
    glm::vec3                  boundsMin{0.0f};
    glm::vec3                  boundsMax{0.0f};
    // CPU copy of the geometry for mesh colliders (submesh ranges as on the GPU).
    std::vector<glm::vec3>     collisionPositions;
    std::vector<std::uint32_t> collisionIndices;
};

// Creates the GPU resources and records their upload on the UploadQueue; `ticket` covers all
// of them. Thread-safe (asset worker threads). On exception `out` holds whatever was created
// so far and must be released like a finished model.
void BuildModel(Renderer& renderer, const ModelData& data, Model& out, UploadTicket& ticket);

// Main thread. Frees GPU memory and bindless slots once in-flight frames are done.
// Precondition: the build's ticket is ready (UploadQueue::IsReady).
void ReleaseModel(Renderer& renderer, Model&& model);

// Creates one entity per node under a new root entity; returns the root. The entities
// reference the model by handle only: releasing it makes them render nothing.
Entity InstantiateModel(Scene& scene, ModelHandle handle, const Model& model, Entity parent = NullEntity);

} // namespace Engine
