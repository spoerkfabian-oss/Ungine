#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/Texture.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Renderer/GeometryPool.h"
#include "Engine/Renderer/Vulkan/Buffer.h"
#include "Engine/Renderer/Vulkan/Image.h"
#include "Engine/Renderer/Vulkan/Upload.h"
#include "Engine/Scene/Components.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
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

struct VertexSkinInfluence {
    glm::u16vec4 joints{0};
    glm::vec4    weights{0.0f};
};

inline constexpr std::uint32_t kMaxLods = 4;

struct SubmeshLod {
    std::uint32_t firstIndex = 0; // like Submesh::firstIndex (relative to the model's indices)
    std::uint32_t indexCount = 0;
    float         error      = 0.0f; // object-space deviation from LOD 0 (0 for LOD 0)
};

struct Submesh {
    std::uint32_t firstIndex   = 0; // LOD 0
    std::uint32_t indexCount   = 0;
    std::int32_t  vertexOffset = 0;
    std::uint32_t material     = 0;
    glm::vec3     boundsMin{0.0f}; // object space, for frustum culling
    glm::vec3     boundsMax{0.0f};
    // Simplified index ranges over the same vertices (MeshOptimizer.h). lods[0] mirrors
    // firstIndex / indexCount; BuildModelGeometry fills it when lodCount is 1.
    std::uint32_t                    lodCount = 1;
    std::array<SubmeshLod, kMaxLods> lods{};
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
    std::int32_t         skin   = -1;
    std::optional<Light> light; // KHR_lights_punctual (point / spot)
};

struct SkinJoint {
    std::int32_t node = -1; // index into Model::nodes
    glm::mat4    inverseBindMatrix{1.0f};
};

struct Skin {
    std::string          name;
    std::int32_t         skeletonRoot = -1; // index into Model::nodes, when present
    std::vector<SkinJoint> joints;
};

enum class AnimationPath : std::uint8_t { Translation, Rotation, Scale };
enum class AnimationInterpolation : std::uint8_t { Linear, Step, CubicSpline };

struct AnimationTrack {
    std::uint32_t          node = 0; // index into Model::nodes
    AnimationPath          path = AnimationPath::Translation;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
    std::vector<float>     times;
    // Values use vec4 for a common representation: translation/scale use xyz, rotation uses xyzw.
    // Cubic spline tracks keep the per-key tangents in separate arrays.
    std::vector<glm::vec4> values;
    std::vector<glm::vec4> inTangents;
    std::vector<glm::vec4> outTangents;
};

struct AnimationClip {
    std::string               name;
    float                     duration = 0.0f;
    std::vector<AnimationTrack> tracks;
};

inline constexpr std::int32_t kNoTexture    = -1; // the slot's default texture
inline constexpr std::int32_t kErrorTexture = -2; // DefaultTexture::Error (placeholders)

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
    std::int32_t baseColorTexture         = kNoTexture; // indices into ModelData::textures (or kNo/kErrorTexture)
    std::int32_t normalTexture            = kNoTexture;
    std::int32_t metallicRoughnessTexture = kNoTexture;
    std::int32_t emissiveTexture          = kNoTexture;
    std::int32_t occlusionTexture         = kNoTexture;
};

// Where a material texture comes from. The AssetManager turns each into a texture asset: an
// external file is shared by path (LoadTexture), embedded bytes by content hash.
struct TextureData {
    std::string            name;
    TextureKind            kind = TextureKind::Color;
    std::filesystem::path  file;    // external image (PNG / JPEG / KTX2), or
    std::vector<std::byte> encoded; // embedded image bytes (PNG / JPEG / KTX2)
    std::uint64_t          hash = 0; // of `encoded` (TextureContentHash)
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
    std::vector<VertexSkinInfluence> skinInfluences;
    std::vector<Skin>          skins;
    std::vector<AnimationClip> animations;
    glm::vec3                  boundsMin{0.0f}; // world space of the default scene
    glm::vec3                  boundsMax{0.0f};
    // Files read besides the source itself (external buffers): hot reload watches them too.
    std::vector<std::filesystem::path> dependencies;
};

[[nodiscard]] std::uint64_t TextureContentHash(std::span<const std::byte> bytes); // FNV-1a 64

// GPU material, mirrors `Material` in mesh_common.glsl.
struct GpuMaterial {
    glm::vec4     baseColorFactor;
    glm::vec4     emissiveFactor;
    float         metallic;
    float         roughness;
    float         alphaCutoff;
    std::uint32_t flags;
    std::uint32_t baseColorTexture; // texture table entries (Renderer::AllocateTextureEntry)
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
    std::uint32_t firstIndex   = 0; // LOD 0
    std::uint32_t indexCount   = 0;
    std::int32_t  vertexOffset = 0;
    std::uint32_t material     = 0;
    glm::vec3     boundsMin{0.0f}; // object space
    std::uint32_t flags = 0;       // material flags (kMaterial*)
    glm::vec3     boundsMax{0.0f};
    std::uint32_t lodCount = 1;
    glm::uvec4    lodFirstIndex{0}; // per LOD, absolute
    glm::uvec4    lodIndexCount{0};
    glm::vec4     lodError{0.0f};   // object space
};
static_assert(sizeof(GpuSubmesh) == 96);

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
    std::vector<GpuSubmesh>    gpuSubmeshes; // CPU copy of those records (index: firstGpuSubmesh - submeshes.offset)
    std::vector<TextureHandle> textures;  // per ModelData texture; owned (released) by the AssetManager
    std::vector<std::uint32_t> materialFlags; // CPU copy for pipeline selection
    std::vector<Mesh>          meshes;
    std::vector<ModelNode>     nodes;
    std::vector<VertexSkinInfluence> skinInfluences; // parallel to GPU model vertices; empty means unskinned
    std::vector<Skin>          skins;
    std::vector<AnimationClip> animations;
    glm::vec3                  boundsMin{0.0f};
    glm::vec3                  boundsMax{0.0f};
    // CPU copy of the geometry for mesh colliders (submesh ranges as on the GPU).
    std::vector<glm::vec3>     collisionPositions;
    std::vector<std::uint32_t> collisionIndices;
};

// Two steps, `ticket` covers every upload of both. On exception `out` holds whatever was created
// so far and must be released like a finished model.
// 1) Geometry (vertices, indices, meshes, nodes, collision copy). Thread-safe: asset workers.
void BuildModelGeometry(Renderer& renderer, const ModelData& data, Model& out, UploadTicket& ticket);
// 2) Materials and submesh records, once the textures have table entries (`textureEntries[i]`
//    for ModelData texture i). Thread-safe too, but texture entries are allocated on the main thread.
void BuildModelMaterials(Renderer& renderer, std::span<const MaterialData> materials,
                         std::span<const std::uint32_t> textureEntries, Model& out, UploadTicket& ticket);
// Both steps (models without textures, tools, tests).
void BuildModel(Renderer& renderer, const ModelData& data, Model& out, UploadTicket& ticket);

// Main thread. Frees the pool ranges once in-flight frames are done (textures are separate
// assets). Precondition: the build's ticket is ready (UploadQueue::IsReady).
void ReleaseModel(Renderer& renderer, Model&& model);

// Geometry memory of a model in the pool (bytes) and its CPU-side copies.
[[nodiscard]] std::uint64_t ModelGpuBytes(const Model& model);
[[nodiscard]] std::uint64_t ModelCpuBytes(const Model& model);

// Creates one entity per node under a new root entity (ModelInstance; nodes get ModelNodeRef);
// returns the root. The entities reference the model by handle only: releasing it makes them
// render nothing.
Entity InstantiateModel(Scene& scene, ModelHandle handle, const Model& model, Entity parent = NullEntity);

// Re-syncs every instance of `handle` with the model's current nodes (after a reload): node
// entities are matched by name (same index first), created, re-parented, updated (local
// transform, mesh, light) or removed (their other children move to the instance root). The
// roots keep their own transforms. Returns the number of instances.
std::size_t RefreshModelInstances(Scene& scene, ModelHandle handle, const Model& model);

} // namespace Engine
