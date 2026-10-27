#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/Texture.h"
#include "Engine/ECS/Entity.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/Vulkan/Buffer.h"
#include "Engine/Renderer/Vulkan/Image.h"
#include "Engine/Renderer/Vulkan/Pipeline.h"
#include "Engine/Scene/Frustum.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

class AssetManager;
class Scene;
struct TerrainData;
struct TerrainChanges;
struct FoliageLayer;

inline constexpr std::uint32_t kTerrainPatchQuads = 32; // grid quads per node side, mirrored via GpuTerrain.info.w
inline constexpr std::uint32_t kTerrainMaxLods    = 16;

// Mirrors GpuTerrainLayer in terrain.glsl.
struct GpuTerrainLayer {
    glm::uvec4 textures{~0u}; // albedo, normal, orm table entries
    glm::vec4  tint{1.0f};    // w: 1 / tiling
    glm::vec4  material{0.0f};
};
static_assert(sizeof(GpuTerrainLayer) == 48);

// Mirrors TerrainBuffer in terrain.glsl.
struct GpuTerrain {
    glm::vec4       origin{0.0f};
    glm::vec4       params{0.0f};
    glm::uvec4      textures{~0u};
    glm::uvec4      info{0u};
    glm::vec4       camera{0.0f};
    glm::vec4       morph[kTerrainMaxLods]{};
    GpuTerrainLayer layers[8]{};
};
static_assert(sizeof(GpuTerrain) == 80 + 16 * kTerrainMaxLods + 8 * 48);

// Mirrors TerrainNode in terrain.glsl.
struct GpuTerrainNode {
    glm::vec2     origin{0.0f};
    float         size = 0.0f;
    std::uint32_t lod  = 0;
};

// Mirrors FoliageLayerBuffer in foliage.glsl.
struct GpuFoliageLayer {
    glm::vec4  baseColor{0.0f}; // w: wind
    glm::vec4  tipColor{0.0f};  // w: time
    glm::vec4  size{0.0f};
    glm::vec4  fade{0.0f};
    glm::uvec4 info{0u};
};
static_assert(sizeof(GpuFoliageLayer) == 80);

// Mirrors FoliageInstance in foliage.glsl.
struct GpuFoliageInstance {
    glm::vec2     local{0.0f};
    std::uint32_t bits   = 0; // yaw, scale, key, tint (8 bit each)
    std::uint32_t unused = 0;
};
static_assert(sizeof(GpuFoliageInstance) == 16);

inline constexpr float         kFoliageChunkSize      = 16.0f; // meters per scatter chunk side
inline constexpr float         kFoliageMaxDensity     = 64.0f; // instances per square meter
inline constexpr std::uint32_t kFoliageMaxInstances   = 1u << 20; // per frame, all views

struct TerrainRenderStats {
    std::uint32_t terrains  = 0;
    std::uint32_t nodes     = 0; // camera
    std::uint32_t triangles = 0; // camera
    std::uint32_t shadowNodes = 0;
    std::uint32_t foliageInstances = 0; // camera (uploaded; the shader still drops some)
    std::uint32_t foliageShadowInstances = 0;
    std::uint32_t foliageChunksBuilt = 0; // scattered this frame
    std::uint32_t foliageDrawCalls = 0;
};

// Heightmap terrains (Terrain components, TerrainStore data) with CDLOD: a quadtree of grid
// patches selected per view by the distance to the camera (ranges double per level), morphed
// towards the coarser grid in the vertex shader, height read from an R16 texture. Drawn in the
// depth prepass (also feeds the Hi-Z), the sun cascades and the lighting pass (depth EQUAL).
// Foliage layers are scattered per 16 m chunk on the CPU (from the density maps, cached until the
// map / settings change) and drawn instanced with the terrain (blades, cards or pool meshes).
class TerrainRenderer {
public:
    struct Formats {
        VkFormat hdr, velocity, normal, entityId, depth, shadow;
    };
    TerrainRenderer(Renderer& renderer, AssetManager& assets, const Formats& formats);
    ~TerrainRenderer();

    TerrainRenderer(const TerrainRenderer&)            = delete;
    TerrainRenderer& operator=(const TerrainRenderer&) = delete;

    void CreatePipelines(); // also on shader hot reload
    // Outside a rendering scope: syncs GPU copies with the store (uploads changed regions), selects
    // the nodes of the camera and every cascade (LODs by the camera distance in all views).
    void Prepare(VkCommandBuffer cmd, Scene& scene, const glm::vec3& camera, const Frustum& cameraFrustum,
                 const std::vector<Frustum>& cascades, bool shadows, std::uint32_t frameIndex);
    // Inside the passes (bindless bound, viewport set).
    void DrawPrepass(VkCommandBuffer cmd, VkDeviceAddress frameAddress, bool picking);
    void DrawLighting(VkCommandBuffer cmd, VkDeviceAddress frameAddress);
    void DrawShadow(VkCommandBuffer cmd, VkDeviceAddress frameAddress, std::uint32_t cascade);
    [[nodiscard]] bool                      Empty() const { return m_Instances.empty(); }
    [[nodiscard]] const TerrainRenderStats& Stats() const { return m_Stats; }

private:
    struct Texture {
        Image         image;
        std::uint32_t slot = 0;
        bool          initialized = false;
    };
    struct FoliageChunk {
        std::vector<GpuFoliageInstance> instances; // shuffled: any prefix is an even thinning
        bool                            built = false;
    };
    struct FoliageView {
        std::uint32_t first = 0, count = 0; // in this frame's instance buffer
    };
    struct FoliageState {
        std::uint64_t             scatterKey = 0; // settings that place instances
        std::uint32_t             chunksPerSide = 0;
        std::vector<FoliageChunk> chunks;
        VkDeviceAddress           gpu = 0; // this frame's GpuFoliageLayer
        std::uint32_t             kind = 0;
        ModelHandle               mesh;     // mesh foliage (shared per path, see m_FoliageModels)
        std::uint32_t             meshIndex = 0;
        FoliageView               camera;
        std::array<FoliageView, 4> cascades{};
    };
    struct Instance {
        Entity                       entity = NullEntity;
        std::string                  path;
        std::shared_ptr<TerrainData> data;
        std::uint64_t                revision = 0;
        std::uint32_t                resolution = 0, splatResolution = 0;
        bool                         hasHoles = false;
        Texture                      height, splat0, splat1, holes;
        std::vector<glm::vec2>       minMax;     // per leaf patch (patches^2): min / max height
        std::uint32_t                patches = 0; // leaf patches per side
        std::uint32_t                lods    = 0;
        float                        lodDistance = 24.0f;
        bool                         castShadows = true;
        glm::vec3                    origin{0.0f};
        float                        size = 0.0f;
        VkDeviceAddress              gpu = 0;   // this frame's GpuTerrain
        std::vector<GpuTerrainNode>  cameraNodes;
        std::array<std::vector<GpuTerrainNode>, 4> cascadeNodes;
        VkDeviceAddress              cameraAddress = 0;
        std::array<VkDeviceAddress, 4> cascadeAddress{};
        std::uint64_t                seen = 0;
        float                        foliageDensity = 1.0f, foliageDistance = 1.0f;
        std::vector<FoliageState>    foliage;
    };
    struct LayerTexture {
        TextureHandle handle;
        std::uint64_t lastUsed = 0;
    };
    struct FoliageModel {
        ModelHandle   handle;
        std::uint64_t lastUsed = 0;
    };

    void Sync(VkCommandBuffer cmd, Instance& instance);
    void CreateTextures(Instance& instance);
    void ReleaseTextures(Instance& instance);
    void Upload(VkCommandBuffer cmd, Texture& texture, std::uint32_t texelBytes, std::uint32_t width,
                const void* rows, std::uint32_t rowPitch, glm::uvec4 rect);
    void UpdateMinMax(Instance& instance, glm::uvec4 heightRect);
    [[nodiscard]] glm::vec2 NodeMinMax(const Instance& instance, glm::uvec2 firstPatch, std::uint32_t patches) const;
    void Select(const Instance& instance, const glm::vec3& camera, const Frustum& frustum, std::vector<GpuTerrainNode>& out) const;
    [[nodiscard]] std::uint32_t LayerTextureEntry(const std::string& path, TextureKind kind);
    void Draw(VkCommandBuffer cmd, VkDeviceAddress frameAddress, const Instance& instance, VkDeviceAddress nodes,
              std::uint32_t count, std::uint32_t cascade);
    void PrepareFoliage(Instance& instance, const glm::vec3& camera, const Frustum& cameraFrustum,
                        const std::vector<Frustum>& cascades, bool shadows);
    void MarkFoliageDirty(Instance& instance, const TerrainChanges& changes);
    void BuildChunk(const Instance& instance, const FoliageLayer& layer, std::uint32_t cx, std::uint32_t cz,
                    FoliageChunk& out) const;
    [[nodiscard]] glm::vec2 RectMinMax(const Instance& instance, glm::vec2 lo, glm::vec2 hi) const;
    [[nodiscard]] ModelHandle FoliageMesh(const std::string& path);
    void DrawFoliage(VkCommandBuffer cmd, VkDeviceAddress frameAddress, std::uint32_t pass /* 0 prepass, 1 lighting, 2 shadow */,
                     std::uint32_t cascade);

    Renderer&     m_Renderer;
    AssetManager& m_Assets;
    Formats       m_Formats;
    Pipeline      m_Prepass, m_PrepassPicking, m_Lighting, m_Shadow;
    Pipeline      m_FoliagePrepass, m_FoliagePrepassPicking, m_FoliageLighting, m_FoliageShadow;
    Buffer        m_Indices;
    std::uint32_t m_IndexCount = 0;
    std::vector<std::unique_ptr<Instance>>        m_Instances;
    std::unordered_map<std::string, LayerTexture> m_LayerTextures;
    std::unordered_map<std::string, FoliageModel> m_FoliageModels;
    std::array<Buffer, kFramesInFlight>           m_FoliageBuffers; // instances of all views, per frame slot
    std::vector<GpuFoliageInstance>               m_FoliageScratch;
    std::uint32_t                                 m_FrameIndex = 0;
    double                                        m_StartTime  = 0.0; // wind clock
    std::uint64_t                                 m_Frame = 0;
    TerrainRenderStats                            m_Stats;
};

} // namespace Engine
