#include "TerrainRenderer.h"
#include "Engine/Assets/AssetManager.h"
#include "Engine/Assets/Model.h"
#include "Engine/Renderer/GeometryPool.h"
#include "Engine/Core/Platform.h"
#include "Engine/Renderer/ShaderReload.h"
#include "Engine/Renderer/Vulkan/VkUtils.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Terrain/TerrainData.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <numeric>
#include <cmath>
#include <cstring>

namespace Engine {

namespace {

struct TerrainPush { // mirrors TerrainPush in terrain.glsl
    VkDeviceAddress frame;
    VkDeviceAddress terrain;
    VkDeviceAddress nodes;
    std::uint32_t   cascade;
    std::uint32_t   flags;
};
static_assert(sizeof(TerrainPush) <= kPushConstantSize);

struct FoliagePush { // mirrors FoliagePush in foliage.glsl
    VkDeviceAddress frame;
    VkDeviceAddress terrain;
    VkDeviceAddress layer;
    VkDeviceAddress instances;
    std::uint32_t   cascade;
    std::uint32_t   material;
};
static_assert(sizeof(FoliagePush) <= kPushConstantSize);

constexpr std::uint32_t kFoliageBlades = 0, kFoliageCards = 1, kFoliageMesh = 2; // foliage.glsl kinds
constexpr std::uint32_t kFoliageAlignToNormal = 1;

double SteadySeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// PCG-style hash: deterministic scatter per (seed, chunk, cell).
std::uint32_t Hash32(std::uint32_t v)
{
    v = v * 747796405u + 2891336453u;
    v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (v >> 22u) ^ v;
}

float Unit(std::uint32_t& state)
{
    state = Hash32(state);
    return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
}

// Fraction of a chunk's instances the shader may keep at distance d (thinning, see foliage.glsl),
// plus the width of its shrink band.
float FoliageKeep(float d, float range)
{
    const float keep = d < 0.4f * range ? 1.0f : 1.0f + (0.3f - 1.0f) * std::clamp((d - 0.4f * range) / (0.45f * range), 0.0f, 1.0f);
    return std::min(1.0f, keep + 0.08f);
}

constexpr VkPipelineStageFlags2 kShaderStages =
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

bool SphereIntersects(const glm::vec3& center, float radius, const Aabb& box)
{
    const glm::vec3 closest = glm::clamp(center, box.min, box.max);
    const glm::vec3 d       = closest - center;
    return glm::dot(d, d) <= radius * radius;
}

// Sample rectangle (x0, y0, x1, y1; end exclusive) of a normalized edit rectangle, one sample of margin.
glm::uvec4 SampleRect(glm::vec4 rect, std::uint32_t size, bool full)
{
    if (full)
        return {0, 0, size, size};
    const float n  = static_cast<float>(size - 1);
    const auto  lo = [&](float v) { return static_cast<std::uint32_t>(std::max(0.0f, std::floor(v * n) - 1.0f)); };
    const auto  hi = [&](float v) { return std::min(size, static_cast<std::uint32_t>(std::ceil(v * n)) + 2u); };
    glm::uvec4  r{lo(rect.x), lo(rect.y), hi(rect.z), hi(rect.w)};
    r.z = std::max(r.z, r.x + 1);
    r.w = std::max(r.w, r.y + 1);
    return glm::min(r, glm::uvec4(size));
}

} // namespace

TerrainRenderer::TerrainRenderer(Renderer& renderer, AssetManager& assets, const Formats& formats)
    : m_Renderer(renderer), m_Assets(assets), m_Formats(formats)
{
    // One patch: kTerrainPatchQuads^2 quads, vertices from gl_VertexIndex (no vertex buffer).
    std::vector<std::uint32_t> indices;
    const std::uint32_t        q = kTerrainPatchQuads;
    for (std::uint32_t z = 0; z < q; ++z)
        for (std::uint32_t x = 0; x < q; ++x) {
            const std::uint32_t v00 = z * (q + 1) + x, v10 = v00 + 1, v01 = v00 + q + 1, v11 = v01 + 1;
            indices.insert(indices.end(), {v00, v01, v10, v10, v01, v11}); // counter-clockwise seen from +Y
        }
    m_IndexCount = static_cast<std::uint32_t>(indices.size());
    m_Indices    = Buffer(renderer.GetContext(), {.size      = indices.size() * sizeof(std::uint32_t),
                                                  .usage     = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                                  .memory    = MemoryUsage::Upload,
                                                  .debugName = "TerrainPatchIndices"});
    m_Indices.Write(indices.data(), indices.size() * sizeof(std::uint32_t));
    m_StartTime = SteadySeconds();
    CreatePipelines();
}

TerrainRenderer::~TerrainRenderer()
{
    for (auto& instance : m_Instances)
        ReleaseTextures(*instance);
    for (auto& [key, layer] : m_LayerTextures)
        m_Assets.Release(layer.handle);
    for (auto& [key, model] : m_FoliageModels)
        m_Assets.Release(model.handle);
    for (Pipeline* p : {&m_Prepass, &m_PrepassPicking, &m_Lighting, &m_Shadow, &m_FoliagePrepass, &m_FoliagePrepassPicking,
                        &m_FoliageLighting, &m_FoliageShadow})
        if (*p)
            m_Renderer.DeferRelease(std::move(*p));
    for (Buffer& b : m_FoliageBuffers)
        if (b)
            m_Renderer.DeferRelease(std::move(b));
    m_Renderer.DeferRelease(std::move(m_Indices));
}

void TerrainRenderer::CreatePipelines()
{
    const VkDevice         device = m_Renderer.GetContext().Device();
    const VkPipelineLayout layout = m_Renderer.GetBindless().PipelineLayout();
    Pipeline prepass = GraphicsPipelineBuilder{}
                           .SetShaders(ShaderPath("terrain.vert.spv"), ShaderPath("terrain_depth.frag.spv"))
                           .AddColorAttachment(m_Formats.normal)
                           .AddColorAttachment(m_Formats.velocity)
                           .SetDepthFormat(m_Formats.depth)
                           .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL)
                           .SetCulling(VK_CULL_MODE_BACK_BIT)
                           .SetDebugName("TerrainPrepass")
                           .Build(device, layout);
    Pipeline picking = GraphicsPipelineBuilder{}
                           .SetShaders(ShaderPath("terrain.vert.spv"), ShaderPath("terrain_depth_id.frag.spv"))
                           .AddColorAttachment(m_Formats.normal)
                           .AddColorAttachment(m_Formats.velocity)
                           .AddColorAttachment(m_Formats.entityId)
                           .SetDepthFormat(m_Formats.depth)
                           .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL)
                           .SetCulling(VK_CULL_MODE_BACK_BIT)
                           .SetDebugName("TerrainPrepassId")
                           .Build(device, layout);
    Pipeline lighting = GraphicsPipelineBuilder{}
                            .SetShaders(ShaderPath("terrain.vert.spv"), ShaderPath("terrain.frag.spv"))
                            .AddColorAttachment(m_Formats.hdr)
                            .AddColorAttachment(m_Formats.velocity)
                            .SetAttachmentBlend(1, BlendMode::NoWrite)
                            .SetDepthFormat(m_Formats.depth)
                            .SetDepth(true, false, VK_COMPARE_OP_EQUAL)
                            .SetCulling(VK_CULL_MODE_BACK_BIT)
                            .SetDebugName("TerrainLighting")
                            .Build(device, layout);
    Pipeline shadow = GraphicsPipelineBuilder{}
                          .SetShaders(ShaderPath("terrain_shadow.vert.spv"), ShaderPath("terrain_shadow.frag.spv"))
                          .SetDepthFormat(m_Formats.shadow)
                          .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL)
                          .SetDepthClamp(true)
                          .SetDynamicDepthBias(true)
                          .SetDebugName("TerrainShadow")
                          .Build(device, layout);
    // Foliage: two-sided, same targets as the terrain.
    Pipeline foliagePrepass = GraphicsPipelineBuilder{}
                                  .SetShaders(ShaderPath("foliage.vert.spv"), ShaderPath("foliage_depth.frag.spv"))
                                  .AddColorAttachment(m_Formats.normal)
                                  .AddColorAttachment(m_Formats.velocity)
                                  .SetDepthFormat(m_Formats.depth)
                                  .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL)
                                  .SetCulling(VK_CULL_MODE_NONE)
                                  .SetDebugName("FoliagePrepass")
                                  .Build(device, layout);
    Pipeline foliagePicking = GraphicsPipelineBuilder{}
                                  .SetShaders(ShaderPath("foliage.vert.spv"), ShaderPath("foliage_depth_id.frag.spv"))
                                  .AddColorAttachment(m_Formats.normal)
                                  .AddColorAttachment(m_Formats.velocity)
                                  .AddColorAttachment(m_Formats.entityId)
                                  .SetDepthFormat(m_Formats.depth)
                                  .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL)
                                  .SetCulling(VK_CULL_MODE_NONE)
                                  .SetDebugName("FoliagePrepassId")
                                  .Build(device, layout);
    Pipeline foliageLighting = GraphicsPipelineBuilder{}
                                   .SetShaders(ShaderPath("foliage.vert.spv"), ShaderPath("foliage.frag.spv"))
                                   .AddColorAttachment(m_Formats.hdr)
                                   .AddColorAttachment(m_Formats.velocity)
                                   .SetAttachmentBlend(1, BlendMode::NoWrite)
                                   .SetDepthFormat(m_Formats.depth)
                                   .SetDepth(true, false, VK_COMPARE_OP_EQUAL)
                                   .SetCulling(VK_CULL_MODE_NONE)
                                   .SetDebugName("FoliageLighting")
                                   .Build(device, layout);
    Pipeline foliageShadow = GraphicsPipelineBuilder{}
                                 .SetShaders(ShaderPath("foliage_shadow.vert.spv"), ShaderPath("foliage_shadow.frag.spv"))
                                 .SetDepthFormat(m_Formats.shadow)
                                 .SetDepth(true, true, VK_COMPARE_OP_GREATER_OR_EQUAL)
                                 .SetDepthClamp(true)
                                 .SetDynamicDepthBias(true)
                                 .SetCulling(VK_CULL_MODE_NONE)
                                 .SetDebugName("FoliageShadow")
                                 .Build(device, layout);
    const auto replace = [&](Pipeline& current, Pipeline& fresh) {
        if (current)
            m_Renderer.DeferRelease(std::move(current));
        current = std::move(fresh);
    };
    replace(m_Prepass, prepass);
    replace(m_PrepassPicking, picking);
    replace(m_Lighting, lighting);
    replace(m_Shadow, shadow);
    replace(m_FoliagePrepass, foliagePrepass);
    replace(m_FoliagePrepassPicking, foliagePicking);
    replace(m_FoliageLighting, foliageLighting);
    replace(m_FoliageShadow, foliageShadow);
}

void TerrainRenderer::ReleaseTextures(Instance& instance)
{
    Renderer* r = &m_Renderer;
    for (Texture* t : {&instance.height, &instance.splat0, &instance.splat1, &instance.holes}) {
        if (!t->image)
            continue;
        r->DeferCall([r, slot = t->slot] { r->GetBindless().RemoveSampledImage(slot); });
        r->DeferRelease(std::move(t->image));
        *t = Texture{};
    }
}

void TerrainRenderer::CreateTextures(Instance& instance)
{
    ReleaseTextures(instance);
    const VulkanContext& ctx      = m_Renderer.GetContext();
    BindlessRegistry&    bindless = m_Renderer.GetBindless();
    const TerrainData&   data     = *instance.data;
    const auto make = [&](Texture& t, VkFormat format, std::uint32_t size, const char* name) {
        t.image       = Image(ctx, {.extent    = {size, size, 1},
                                    .format    = format,
                                    .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    .debugName = name});
        t.slot        = bindless.AddSampledImage(t.image.View());
        t.initialized = false;
    };
    make(instance.height, VK_FORMAT_R16_UNORM, data.resolution, "TerrainHeight");
    make(instance.splat0, VK_FORMAT_R8G8B8A8_UNORM, data.splatResolution, "TerrainSplat0");
    make(instance.splat1, VK_FORMAT_R8G8B8A8_UNORM, data.splatResolution, "TerrainSplat1");
    if (!data.holes.empty())
        make(instance.holes, VK_FORMAT_R8_UNORM, data.resolution, "TerrainHoles");
    instance.resolution      = data.resolution;
    instance.splatResolution = data.splatResolution;
    instance.hasHoles        = !data.holes.empty();

    // Quadtree of leaf patches (power of two per side) and its min/max pyramid.
    const std::uint32_t cells = data.resolution - 1;
    instance.patches = std::bit_ceil(std::max(1u, (cells + kTerrainPatchQuads - 1) / kTerrainPatchQuads));
    instance.lods    = std::min<std::uint32_t>(kTerrainMaxLods, static_cast<std::uint32_t>(std::countr_zero(instance.patches)) + 1);
    instance.minMax.assign(static_cast<std::size_t>(instance.patches) * instance.patches, glm::vec2(0.0f));
}

void TerrainRenderer::Upload(VkCommandBuffer cmd, Texture& texture, std::uint32_t texelBytes, std::uint32_t width,
                             const void* rows, std::uint32_t rowPitch, glm::uvec4 rect)
{
    (void)width;
    const std::uint32_t w = rect.z - rect.x, h = rect.w - rect.y;
    if (w == 0 || h == 0)
        return;
    Buffer staging(m_Renderer.GetContext(), {.size      = static_cast<VkDeviceSize>(w) * h * texelBytes,
                                             .usage     = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                             .memory    = MemoryUsage::Upload,
                                             .debugName = "TerrainStaging"});
    auto*       dst = static_cast<std::uint8_t*>(staging.Mapped());
    const auto* src = static_cast<const std::uint8_t*>(rows);
    for (std::uint32_t y = 0; y < h; ++y)
        std::memcpy(dst + static_cast<std::size_t>(y) * w * texelBytes,
                    src + static_cast<std::size_t>(rect.y + y) * rowPitch + static_cast<std::size_t>(rect.x) * texelBytes,
                    static_cast<std::size_t>(w) * texelBytes);
    staging.Flush(0, staging.Size());

    // Earlier frames may still read the texture (WAR); first use starts from UNDEFINED.
    CmdImageBarrier(cmd, {.image     = texture.image.Handle(),
                          .oldLayout = texture.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          .srcStage  = kShaderStages,
                          .dstStage  = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT});
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset      = {static_cast<std::int32_t>(rect.x), static_cast<std::int32_t>(rect.y), 0};
    region.imageExtent      = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging.Handle(), texture.image.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    CmdImageBarrier(cmd, {.image     = texture.image.Handle(),
                          .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          .srcStage  = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          .dstStage  = kShaderStages,
                          .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
    // The staging memory is reused once released: later transfer writes into it come after this read
    // (otherwise sync validation sees a write-after-read across the reuse).
    VkMemoryBarrier2 reuse{};
    reuse.sType        = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    reuse.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    reuse.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    VkDependencyInfo dependency{};
    dependency.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers    = &reuse;
    vkCmdPipelineBarrier2(cmd, &dependency);
    texture.initialized = true;
    m_Renderer.DeferRelease(std::move(staging));
}

void TerrainRenderer::UpdateMinMax(Instance& instance, glm::uvec4 rect)
{
    const TerrainData&  data  = *instance.data;
    const std::uint32_t q     = kTerrainPatchQuads;
    const std::uint32_t last  = data.resolution - 1;
    const std::uint32_t px0   = rect.x / q, pz0 = rect.y / q;
    const std::uint32_t px1   = std::min(instance.patches, (rect.z + q - 1) / q + 1);
    const std::uint32_t pz1   = std::min(instance.patches, (rect.w + q - 1) / q + 1);
    for (std::uint32_t pz = pz0; pz < pz1; ++pz)
        for (std::uint32_t px = px0; px < px1; ++px) {
            glm::vec2 mm(1e30f, -1e30f);
            const std::uint32_t x0 = std::min(px * q, last), z0 = std::min(pz * q, last);
            const std::uint32_t x1 = std::min(x0 + q, last), z1 = std::min(z0 + q, last);
            for (std::uint32_t z = z0; z <= z1; ++z)
                for (std::uint32_t x = x0; x <= x1; ++x) {
                    const float h = data.HeightAtSample(x, z);
                    mm = glm::vec2(std::min(mm.x, h), std::max(mm.y, h));
                }
            instance.minMax[static_cast<std::size_t>(pz) * instance.patches + px] = mm;
        }
}

glm::vec2 TerrainRenderer::NodeMinMax(const Instance& instance, glm::uvec2 first, std::uint32_t count) const
{
    glm::vec2 mm(1e30f, -1e30f);
    const std::uint32_t x1 = std::min(instance.patches, first.x + count), z1 = std::min(instance.patches, first.y + count);
    for (std::uint32_t z = first.y; z < z1; ++z)
        for (std::uint32_t x = first.x; x < x1; ++x) {
            const glm::vec2 v = instance.minMax[static_cast<std::size_t>(z) * instance.patches + x];
            mm = glm::vec2(std::min(mm.x, v.x), std::max(mm.y, v.y));
        }
    return mm;
}

void TerrainRenderer::Select(const Instance& instance, const glm::vec3& camera, const Frustum& frustum,
                             std::vector<GpuTerrainNode>& out) const
{
    out.clear();
    const float leaf = static_cast<float>(kTerrainPatchQuads) * instance.data->CellSize();
    // Range of level l: the finest covers lodDistance, each coarser one doubles; the top is unbounded.
    const auto range = [&](std::uint32_t level) { return instance.lodDistance * static_cast<float>(1u << level); };
    const auto recurse = [&](auto&& self, std::uint32_t level, std::uint32_t px, std::uint32_t pz) -> void {
        const std::uint32_t count = 1u << level;
        const float         x0 = static_cast<float>(px) * leaf, z0 = static_cast<float>(pz) * leaf;
        if (x0 >= instance.size || z0 >= instance.size)
            return;
        // Min / max of the node: pyramid-free scan of its leaf patches (coarse levels have few nodes).
        const glm::vec2 mm = NodeMinMax(instance, {px, pz}, count);
        const float     x1 = std::min(x0 + leaf * static_cast<float>(count), instance.size);
        const float     z1 = std::min(z0 + leaf * static_cast<float>(count), instance.size);
        const Aabb      box{instance.origin + glm::vec3(x0, mm.x, z0), instance.origin + glm::vec3(x1, mm.y, z1)};
        if (!frustum.Intersects(box))
            return;
        if (level == 0 || !SphereIntersects(camera, range(level - 1), box)) {
            out.push_back({glm::vec2(x0, z0), leaf * static_cast<float>(count), level});
            return;
        }
        const std::uint32_t half = count / 2;
        for (std::uint32_t c = 0; c < 4; ++c)
            self(self, level - 1, px + (c & 1u) * half, pz + (c >> 1) * half);
    };
    recurse(recurse, instance.lods - 1, 0, 0);
}

std::uint32_t TerrainRenderer::LayerTextureEntry(const std::string& path, TextureKind kind)
{
    if (path.empty())
        return ~0u;
    const std::string key = std::to_string(static_cast<int>(kind)) + '|' + path;
    auto it = m_LayerTextures.find(key);
    if (it == m_LayerTextures.end())
        it = m_LayerTextures.emplace(key, LayerTexture{.handle = m_Assets.LoadTexture(PathFromUtf8(path), kind)}).first;
    it->second.lastUsed = m_Frame;
    return m_Assets.TableEntry(it->second.handle);
}

void TerrainRenderer::Sync(VkCommandBuffer cmd, Instance& instance)
{
    TerrainStore&        store   = TerrainStore::Instance();
    const TerrainData&   data    = *instance.data;
    const std::uint64_t  current = store.Revision(instance.path);
    const bool recreate = !instance.height.image || instance.resolution != data.resolution ||
                          instance.splatResolution != data.splatResolution || instance.hasHoles != !data.holes.empty();
    TerrainChanges changes = store.ChangesSince(instance.path, instance.revision);
    if (recreate) {
        CreateTextures(instance);
        changes.full  = true;
        changes.kinds = TerrainChangeAll;
    }
    if (changes.kinds == 0 && !changes.full)
        return;
    instance.revision = current;
    MarkFoliageDirty(instance, changes);
    if (changes.kinds & (TerrainChangeHeights | TerrainChangeSettings)) {
        const glm::uvec4 rect = SampleRect(changes.rect, data.resolution, changes.full);
        Upload(cmd, instance.height, 2, data.resolution, data.heights.data(), data.resolution * 2, rect);
        UpdateMinMax(instance, rect);
    }
    if (changes.kinds & (TerrainChangeHoles | TerrainChangeSettings))
        if (instance.holes.image) {
            const glm::uvec4 rect = SampleRect(changes.rect, data.resolution, changes.full);
            Upload(cmd, instance.holes, 1, data.resolution, data.holes.data(), data.resolution, rect);
        }
    if (changes.kinds & (TerrainChangeSplat | TerrainChangeSettings)) {
        // Interleaved 8 weights per texel -> two RGBA8 maps (only the rectangle is converted).
        const std::uint32_t  n    = data.splatResolution;
        const glm::uvec4     rect = SampleRect(changes.rect, n, changes.full);
        std::vector<std::uint8_t> a(static_cast<std::size_t>(n) * n * 4), b(a.size());
        for (std::uint32_t y = rect.y; y < rect.w; ++y)
            for (std::uint32_t x = rect.x; x < rect.z; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * n + x;
                std::memcpy(&a[i * 4], &data.splat[i * kMaxTerrainLayers], 4);
                std::memcpy(&b[i * 4], &data.splat[i * kMaxTerrainLayers + 4], 4);
            }
        Upload(cmd, instance.splat0, 4, n, a.data(), n * 4, rect);
        Upload(cmd, instance.splat1, 4, n, b.data(), n * 4, rect);
    }
}

void TerrainRenderer::Prepare(VkCommandBuffer cmd, Scene& scene, const glm::vec3& camera, const Frustum& cameraFrustum,
                              const std::vector<Frustum>& cascades, bool shadows, std::uint32_t frameIndex)
{
    ++m_Frame;
    m_Stats      = {};
    m_FrameIndex = frameIndex % kFramesInFlight;
    m_FoliageScratch.clear();
    TerrainStore& store    = TerrainStore::Instance();
    Registry&     registry = scene.GetRegistry();
    registry.ViewOf<Terrain, WorldTransform>().Each([&](Entity entity, Terrain& terrain, WorldTransform& world) {
        std::shared_ptr<TerrainData> data = store.Get(terrain.data);
        if (!data)
            return;
        auto it = std::ranges::find_if(m_Instances, [&](const auto& i) { return i->entity == entity; });
        if (it == m_Instances.end()) {
            m_Instances.push_back(std::make_unique<Instance>());
            it = std::prev(m_Instances.end());
            (*it)->entity = entity;
        }
        Instance& instance = **it;
        if (instance.path != terrain.data || instance.data != data) { // other file or reloaded data: everything
            instance.path     = terrain.data;
            instance.data     = std::move(data);
            instance.revision = 0;
            instance.foliage.clear();
            ReleaseTextures(instance);
        }
        instance.seen        = m_Frame;
        instance.lodDistance = std::max(terrain.lodDistance, 1.0f);
        instance.castShadows = terrain.castShadows;
        instance.foliageDensity  = std::max(terrain.foliageDensity, 0.0f);
        instance.foliageDistance = std::max(terrain.foliageDistance, 0.0f);
        instance.size        = instance.data->size;
        instance.origin      = glm::vec3(world.matrix[3]) - glm::vec3(0.5f * instance.size, 0.0f, 0.5f * instance.size);
    });
    // Terrains whose entity, component or data went away.
    for (auto it = m_Instances.begin(); it != m_Instances.end();) {
        if ((*it)->seen != m_Frame) {
            ReleaseTextures(**it);
            it = m_Instances.erase(it);
        } else {
            ++it;
        }
    }

    for (auto& ptr : m_Instances) {
        Instance& instance = *ptr;
        Sync(cmd, instance);
        const TerrainData& data = *instance.data;

        GpuTerrain gpu{};
        gpu.origin   = glm::vec4(instance.origin, instance.size);
        gpu.params   = glm::vec4(data.heightScale, static_cast<float>(data.resolution), data.CellSize(),
                                 std::cos(glm::radians(data.triplanarSlope)));
        gpu.textures = glm::uvec4(instance.height.slot, instance.splat0.slot, instance.splat1.slot,
                                  instance.holes.image ? instance.holes.slot : ~0u);
        gpu.info     = glm::uvec4(static_cast<std::uint32_t>(data.layers.size()), EntityIndex(instance.entity) + 1,
                                  instance.lods, kTerrainPatchQuads);
        gpu.camera   = glm::vec4(camera, static_cast<float>(data.splatResolution));
        for (std::uint32_t l = 0; l < instance.lods; ++l) {
            const float end   = instance.lodDistance * static_cast<float>(1u << l);
            const float start = (l == 0 ? 0.0f : end * 0.5f) + (l == 0 ? end : end * 0.5f) * 0.6f;
            gpu.morph[l] = l + 1 == instance.lods ? glm::vec4(1e30f, 2e30f, 0.0f, 0.0f) : glm::vec4(start, end, 0.0f, 0.0f);
        }
        for (std::size_t i = 0; i < data.layers.size() && i < 8; ++i) {
            const TerrainLayer& l = data.layers[i];
            gpu.layers[i].textures = glm::uvec4(LayerTextureEntry(l.albedo, TextureKind::Color),
                                                LayerTextureEntry(l.normal, TextureKind::Normal),
                                                LayerTextureEntry(l.orm, TextureKind::Linear), 0u);
            gpu.layers[i].tint     = glm::vec4(l.tint, 1.0f / std::max(l.tiling, 0.01f));
            gpu.layers[i].material = glm::vec4(l.roughness, l.metallic, l.normalStrength, 0.0f);
        }
        instance.gpu = m_Renderer.PushTransient(gpu, 16);

        const auto pushNodes = [&](const std::vector<GpuTerrainNode>& nodes) -> VkDeviceAddress {
            if (nodes.empty())
                return 0;
            const TransientAllocation a = m_Renderer.AllocateTransient(nodes.size() * sizeof(GpuTerrainNode), 16);
            std::memcpy(a.cpu, nodes.data(), nodes.size() * sizeof(GpuTerrainNode));
            return a.gpu;
        };
        Select(instance, camera, cameraFrustum, instance.cameraNodes);
        instance.cameraAddress = pushNodes(instance.cameraNodes);
        for (std::size_t c = 0; c < instance.cascadeNodes.size(); ++c) {
            instance.cascadeNodes[c].clear();
            if (shadows && instance.castShadows && c < cascades.size())
                Select(instance, camera, cascades[c], instance.cascadeNodes[c]);
            instance.cascadeAddress[c] = pushNodes(instance.cascadeNodes[c]);
            m_Stats.shadowNodes += static_cast<std::uint32_t>(instance.cascadeNodes[c].size());
        }
        PrepareFoliage(instance, camera, cameraFrustum, cascades, shadows && instance.castShadows);
        ++m_Stats.terrains;
        m_Stats.nodes += static_cast<std::uint32_t>(instance.cameraNodes.size());
        m_Stats.triangles += static_cast<std::uint32_t>(instance.cameraNodes.size()) * m_IndexCount / 3;
    }

    // This frame's foliage instances (all terrains and views) -> the frame slot's buffer.
    if (!m_FoliageScratch.empty()) {
        Buffer&            buffer = m_FoliageBuffers[m_FrameIndex];
        const VkDeviceSize bytes  = m_FoliageScratch.size() * sizeof(GpuFoliageInstance);
        if (!buffer || buffer.Size() < bytes) {
            if (buffer)
                m_Renderer.DeferRelease(std::move(buffer));
            buffer = Buffer(m_Renderer.GetContext(), {.size      = std::bit_ceil(std::max<VkDeviceSize>(bytes, 64 * 1024)),
                                                      .usage     = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                      .memory    = MemoryUsage::Upload,
                                                      .debugName = "FoliageInstances"});
        }
        std::memcpy(buffer.Mapped(), m_FoliageScratch.data(), bytes);
        buffer.Flush(0, bytes);
    }
    // Foliage meshes no longer used.
    for (auto it = m_FoliageModels.begin(); it != m_FoliageModels.end();) {
        if (m_Frame - it->second.lastUsed > 300) {
            m_Assets.Release(it->second.handle);
            it = m_FoliageModels.erase(it);
        } else {
            ++it;
        }
    }
    // Layer textures unused for a while (layers removed / terrain gone).
    for (auto it = m_LayerTextures.begin(); it != m_LayerTextures.end();) {
        if (m_Frame - it->second.lastUsed > 300) {
            m_Assets.Release(it->second.handle);
            it = m_LayerTextures.erase(it);
        } else {
            ++it;
        }
    }
}

void TerrainRenderer::Draw(VkCommandBuffer cmd, VkDeviceAddress frameAddress, const Instance& instance, VkDeviceAddress nodes,
                           std::uint32_t count, std::uint32_t cascade)
{
    if (count == 0 || nodes == 0)
        return;
    const TerrainPush push{.frame = frameAddress, .terrain = instance.gpu, .nodes = nodes, .cascade = cascade, .flags = 0};
    vkCmdPushConstants(cmd, m_Renderer.GetBindless().PipelineLayout(), VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdBindIndexBuffer(cmd, m_Indices.Handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_IndexCount, count, 0, 0, 0);
}

void TerrainRenderer::DrawPrepass(VkCommandBuffer cmd, VkDeviceAddress frameAddress, bool picking)
{
    if (m_Instances.empty())
        return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, picking ? m_PrepassPicking.Handle() : m_Prepass.Handle());
    for (const auto& instance : m_Instances)
        Draw(cmd, frameAddress, *instance, instance->cameraAddress, static_cast<std::uint32_t>(instance->cameraNodes.size()), 0);
    DrawFoliage(cmd, frameAddress, picking ? 3u : 0u, 0);
}

void TerrainRenderer::DrawLighting(VkCommandBuffer cmd, VkDeviceAddress frameAddress)
{
    if (m_Instances.empty())
        return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Lighting.Handle());
    for (const auto& instance : m_Instances)
        Draw(cmd, frameAddress, *instance, instance->cameraAddress, static_cast<std::uint32_t>(instance->cameraNodes.size()), 0);
    DrawFoliage(cmd, frameAddress, 1, 0);
}

void TerrainRenderer::DrawShadow(VkCommandBuffer cmd, VkDeviceAddress frameAddress, std::uint32_t cascade)
{
    if (m_Instances.empty() || cascade >= 4)
        return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Shadow.Handle());
    for (const auto& instance : m_Instances)
        Draw(cmd, frameAddress, *instance, instance->cascadeAddress[cascade],
             static_cast<std::uint32_t>(instance->cascadeNodes[cascade].size()), cascade);
    DrawFoliage(cmd, frameAddress, 2, cascade);
}

// --- Foliage ------------------------------------------------------------------------------------

void TerrainRenderer::MarkFoliageDirty(Instance& instance, const TerrainChanges& changes)
{
    if (!(changes.kinds & (TerrainChangeFoliage | TerrainChangeSettings)) && !changes.full)
        return;
    const bool all = changes.full || (changes.kinds & TerrainChangeSettings) != 0;
    for (FoliageState& state : instance.foliage) {
        const std::uint32_t n = state.chunksPerSide;
        if (n == 0)
            continue;
        // Chunk range of the rectangle (normalized over the terrain); a texel of margin.
        const float scale = instance.size / kFoliageChunkSize;
        const auto  lo    = [&](float v) { return static_cast<std::uint32_t>(std::clamp(std::floor(v * scale - 0.05f), 0.0f, static_cast<float>(n - 1))); };
        const auto  hi    = [&](float v) { return static_cast<std::uint32_t>(std::clamp(std::floor(v * scale + 0.05f), 0.0f, static_cast<float>(n - 1))); };
        const glm::uvec4 r = all ? glm::uvec4(0, 0, n - 1, n - 1)
                                 : glm::uvec4(lo(changes.rect.x), lo(changes.rect.y), hi(changes.rect.z), hi(changes.rect.w));
        for (std::uint32_t z = r.y; z <= r.w; ++z)
            for (std::uint32_t x = r.x; x <= r.z; ++x)
                state.chunks[static_cast<std::size_t>(z) * n + x].built = false;
    }
}

void TerrainRenderer::BuildChunk(const Instance& instance, const FoliageLayer& layer, std::uint32_t cx, std::uint32_t cz,
                                 FoliageChunk& out) const
{
    out.instances.clear();
    out.built = true;
    const TerrainData& data    = *instance.data;
    const float        density = std::min(layer.density * instance.foliageDensity, kFoliageMaxDensity);
    if (!(density > 1e-4f) || layer.densityMap.empty())
        return;
    const float spacing = 1.0f / std::sqrt(density);
    const float x0 = static_cast<float>(cx) * kFoliageChunkSize, z0 = static_cast<float>(cz) * kFoliageChunkSize;
    const float x1 = std::min(x0 + kFoliageChunkSize, data.size), z1 = std::min(z0 + kFoliageChunkSize, data.size);
    const auto  nx = static_cast<std::uint32_t>(std::ceil((x1 - x0) / spacing));
    const auto  nz = static_cast<std::uint32_t>(std::ceil((z1 - z0) / spacing));
    const std::uint32_t splat = data.splatResolution;
    const std::uint32_t chunkSeed = Hash32(layer.seed * 0x9E3779B9u ^ Hash32(cx * 73856093u ^ cz * 19349663u));
    // Jittered grid: one candidate per cell, kept with the painted density.
    for (std::uint32_t j = 0; j < nz; ++j)
        for (std::uint32_t i = 0; i < nx; ++i) {
            std::uint32_t rng = chunkSeed ^ Hash32(i * 2654435761u + j * 40503u + 1u);
            const float   px  = x0 + (static_cast<float>(i) + Unit(rng)) * spacing;
            const float   pz  = z0 + (static_cast<float>(j) + Unit(rng)) * spacing;
            if (px >= x1 || pz >= z1)
                continue;
            const auto sx = std::min(splat - 1, static_cast<std::uint32_t>(px / data.size * static_cast<float>(splat)));
            const auto sz = std::min(splat - 1, static_cast<std::uint32_t>(pz / data.size * static_cast<float>(splat)));
            const float painted = static_cast<float>(layer.densityMap[static_cast<std::size_t>(sz) * splat + sx]) / 255.0f;
            if (Unit(rng) >= painted)
                continue;
            const auto yaw   = static_cast<std::uint32_t>(Unit(rng) * 255.99f);
            const auto scale = static_cast<std::uint32_t>(Unit(rng) * 255.99f);
            const auto tint  = static_cast<std::uint32_t>(Unit(rng) * 255.99f);
            out.instances.push_back({glm::vec2(px, pz), yaw | (scale << 8) | (tint << 24), 0u});
        }
    // Shuffle so any prefix is an even thinning; the key is the position in the list.
    std::uint32_t rng = chunkSeed ^ 0x51ED27u;
    for (std::size_t k = out.instances.size(); k > 1; --k)
        std::swap(out.instances[k - 1], out.instances[static_cast<std::size_t>(Unit(rng) * static_cast<float>(k)) % k]);
    const auto count = static_cast<float>(out.instances.size());
    for (std::size_t k = 0; k < out.instances.size(); ++k)
        out.instances[k].bits |= std::min(255u, static_cast<std::uint32_t>(static_cast<float>(k) / count * 256.0f)) << 16;
}

glm::vec2 TerrainRenderer::RectMinMax(const Instance& instance, glm::vec2 lo, glm::vec2 hi) const
{
    const float         leaf = static_cast<float>(kTerrainPatchQuads) * instance.data->CellSize();
    const auto          clampPatch = [&](float v) {
        return static_cast<std::uint32_t>(std::clamp(v / leaf, 0.0f, static_cast<float>(instance.patches - 1)));
    };
    const std::uint32_t px0 = clampPatch(lo.x), pz0 = clampPatch(lo.y), px1 = clampPatch(hi.x), pz1 = clampPatch(hi.y);
    glm::vec2 mm(1e30f, -1e30f);
    for (std::uint32_t z = pz0; z <= pz1; ++z)
        for (std::uint32_t x = px0; x <= px1; ++x) {
            const glm::vec2 v = instance.minMax[static_cast<std::size_t>(z) * instance.patches + x];
            mm = glm::vec2(std::min(mm.x, v.x), std::max(mm.y, v.y));
        }
    return mm;
}

ModelHandle TerrainRenderer::FoliageMesh(const std::string& path)
{
    if (path.empty())
        return {};
    auto it = m_FoliageModels.find(path);
    if (it == m_FoliageModels.end())
        it = m_FoliageModels.emplace(path, FoliageModel{.handle = m_Assets.LoadModel(PathFromUtf8(path))}).first;
    it->second.lastUsed = m_Frame;
    return it->second.handle;
}

void TerrainRenderer::PrepareFoliage(Instance& instance, const glm::vec3& camera, const Frustum& cameraFrustum,
                                     const std::vector<Frustum>& cascades, bool shadows)
{
    const TerrainData& data = *instance.data;
    instance.foliage.resize(data.foliage.size());
    const float time = static_cast<float>(std::fmod(SteadySeconds() - m_StartTime, 3600.0));
    for (std::size_t li = 0; li < data.foliage.size(); ++li) {
        const FoliageLayer& layer = data.foliage[li];
        FoliageState&       state = instance.foliage[li];
        state.camera   = {};
        state.cascades = {};
        state.gpu      = 0;
        const float range = layer.viewDistance * instance.foliageDistance;
        if (!(range > 0.5f) || layer.densityMap.empty())
            continue;

        // Scatter settings: a change rescatters every chunk (density map edits only theirs).
        std::uint64_t key = 1469598103934665603ull;
        for (const std::uint64_t v : {static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(layer.density)),
                                      static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(instance.foliageDensity)),
                                      static_cast<std::uint64_t>(layer.seed), static_cast<std::uint64_t>(data.splatResolution),
                                      static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(data.size))})
            key = (key ^ v) * 1099511628211ull;
        const auto chunks = static_cast<std::uint32_t>(std::ceil(data.size / kFoliageChunkSize));
        if (state.scatterKey != key || state.chunksPerSide != chunks) {
            state.scatterKey    = key;
            state.chunksPerSide = chunks;
            state.chunks.assign(static_cast<std::size_t>(chunks) * chunks, FoliageChunk{});
        }

        // Kind, mesh, layer constants.
        std::uint32_t textureEntry = ~0u, kind = kFoliageBlades;
        float         meshHeight   = layer.height;
        const Model*  model        = nullptr;
        std::uint32_t meshIndex    = 0;
        if (layer.kind == FoliageKind::Mesh) {
            state.mesh = FoliageMesh(layer.mesh);
            const ResolvedMesh resolved = m_Assets.ResolveMesh(state.mesh, layer.meshIndex);
            if (!resolved.model || resolved.placeholder || resolved.meshIndex >= resolved.model->meshes.size())
                continue; // loading / failed: nothing (no placeholder boxes in the grass)
            model     = resolved.model;
            meshIndex = resolved.meshIndex;
            kind      = kFoliageMesh;
            float top = 0.05f;
            for (const Submesh& sm : model->meshes[meshIndex].submeshes)
                top = std::max(top, sm.boundsMax.y);
            meshHeight = top;
        } else if (!layer.texture.empty()) {
            textureEntry = LayerTextureEntry(layer.texture, TextureKind::Color);
            kind         = kFoliageCards;
        }
        state.kind      = kind;
        state.meshIndex = meshIndex;
        GpuFoliageLayer gpu{};
        gpu.baseColor = glm::vec4(layer.baseColor, layer.wind);
        gpu.tipColor  = glm::vec4(layer.tipColor, time);
        gpu.size      = glm::vec4(kind == kFoliageMesh ? 1.0f : layer.width, kind == kFoliageMesh ? meshHeight : layer.height,
                                  std::min(layer.minScale, layer.maxScale), std::max(layer.minScale, layer.maxScale));
        gpu.fade      = glm::vec4(range, std::cos(glm::radians(std::clamp(layer.maxSlope, 0.0f, 90.0f))), 1.0f / 60.0f, 0.0f);
        gpu.info      = glm::uvec4(kind, textureEntry, layer.alignToNormal && kind == kFoliageMesh ? kFoliageAlignToNormal : 0u,
                                   EntityIndex(instance.entity) + 1);
        state.gpu     = m_Renderer.PushTransient(gpu, 16);

        // Chunks in range, per view: the camera, then the cascades (shadow casters).
        const float topMargin = (kind == kFoliageMesh ? meshHeight : layer.height) * gpu.size.w + layer.wind;
        const float sideMargin = (kind == kFoliageMesh ? meshHeight : layer.width) * gpu.size.w + layer.wind;
        const glm::vec2 cam(camera.x - instance.origin.x, camera.z - instance.origin.z);
        const auto lo = [&](float v) { return static_cast<std::int64_t>(std::floor((v - range) / kFoliageChunkSize)); };
        const auto hi = [&](float v) { return static_cast<std::int64_t>(std::floor((v + range) / kFoliageChunkSize)); };
        const std::int64_t cx0 = std::max<std::int64_t>(0, lo(cam.x)), cz0 = std::max<std::int64_t>(0, lo(cam.y));
        const std::int64_t cx1 = std::min<std::int64_t>(chunks - 1, hi(cam.x)), cz1 = std::min<std::int64_t>(chunks - 1, hi(cam.y));
        const bool castShadows = shadows && layer.castShadows;
        const auto gather = [&](const Frustum& frustum, FoliageView& view, std::uint32_t& stat) {
            view.first = static_cast<std::uint32_t>(m_FoliageScratch.size());
            for (std::int64_t cz = cz0; cz <= cz1; ++cz)
                for (std::int64_t cx = cx0; cx <= cx1; ++cx) {
                    const glm::vec2 c0(static_cast<float>(cx) * kFoliageChunkSize, static_cast<float>(cz) * kFoliageChunkSize);
                    const glm::vec2 c1 = glm::min(c0 + glm::vec2(kFoliageChunkSize), glm::vec2(data.size));
                    const glm::vec2 mm = RectMinMax(instance, c0, c1);
                    const Aabb box{instance.origin + glm::vec3(c0.x - sideMargin, mm.x, c0.y - sideMargin),
                                   instance.origin + glm::vec3(c1.x + sideMargin, mm.y + topMargin, c1.y + sideMargin)};
                    const glm::vec3 closest = glm::clamp(camera, box.min, box.max);
                    const float     nearest = glm::distance(camera, closest);
                    if (nearest >= range || !frustum.Intersects(box))
                        continue;
                    FoliageChunk& chunk = state.chunks[static_cast<std::size_t>(cz) * chunks + static_cast<std::size_t>(cx)];
                    if (!chunk.built) {
                        BuildChunk(instance, layer, static_cast<std::uint32_t>(cx), static_cast<std::uint32_t>(cz), chunk);
                        ++m_Stats.foliageChunksBuilt;
                    }
                    const auto n    = static_cast<std::uint32_t>(chunk.instances.size());
                    auto       take = std::min(n, static_cast<std::uint32_t>(std::ceil(static_cast<float>(n) * FoliageKeep(nearest, range))));
                    take = std::min<std::uint32_t>(take, kFoliageMaxInstances - static_cast<std::uint32_t>(std::min<std::size_t>(m_FoliageScratch.size(), kFoliageMaxInstances)));
                    m_FoliageScratch.insert(m_FoliageScratch.end(), chunk.instances.begin(), chunk.instances.begin() + take);
                }
            view.count = static_cast<std::uint32_t>(m_FoliageScratch.size()) - view.first;
            stat += view.count;
        };
        gather(cameraFrustum, state.camera, m_Stats.foliageInstances);
        if (castShadows)
            for (std::size_t c = 0; c < cascades.size() && c < state.cascades.size(); ++c)
                gather(cascades[c], state.cascades[c], m_Stats.foliageShadowInstances);
    }
}

void TerrainRenderer::DrawFoliage(VkCommandBuffer cmd, VkDeviceAddress frameAddress, std::uint32_t pass, std::uint32_t cascade)
{
    const Buffer& buffer = m_FoliageBuffers[m_FrameIndex];
    if (!buffer)
        return;
    const Pipeline& pipeline = pass == 0 ? m_FoliagePrepass : pass == 1 ? m_FoliageLighting : pass == 2 ? m_FoliageShadow
                                                                                                         : m_FoliagePrepassPicking;
    const VkPipelineLayout layout = m_Renderer.GetBindless().PipelineLayout();
    bool bound = false, indexBound = false;
    for (const auto& instance : m_Instances)
        for (const FoliageState& state : instance->foliage) {
            const FoliageView& view = pass == 2 ? state.cascades[cascade] : state.camera;
            if (view.count == 0 || state.gpu == 0)
                continue;
            if (!bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Handle());
                bound = true;
            }
            FoliagePush push{.frame     = frameAddress,
                             .terrain   = instance->gpu,
                             .layer     = state.gpu,
                             .instances = buffer.Address() + static_cast<VkDeviceAddress>(view.first) * sizeof(GpuFoliageInstance),
                             .cascade   = cascade,
                             .material  = 0};
            if (state.kind != kFoliageMesh) {
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                vkCmdDraw(cmd, state.kind == kFoliageCards ? 12u : 15u, view.count, 0, 0);
                ++m_Stats.foliageDrawCalls;
                continue;
            }
            const ResolvedMesh resolved = m_Assets.ResolveMesh(state.mesh, state.meshIndex);
            if (!resolved.model || resolved.placeholder || resolved.meshIndex >= resolved.model->meshes.size())
                continue;
            const Model& model = *resolved.model;
            const Mesh&  mesh  = model.meshes[resolved.meshIndex];
            if (!indexBound) {
                vkCmdBindIndexBuffer(cmd, m_Renderer.Geometry().IndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
                indexBound = true;
            }
            for (std::size_t k = 0; k < mesh.submeshes.size(); ++k) {
                const std::size_t record = mesh.firstGpuSubmesh - model.submeshes.offset + k;
                if (record >= model.gpuSubmeshes.size())
                    break;
                const GpuSubmesh& s = model.gpuSubmeshes[record];
                push.material       = s.material;
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
                vkCmdDrawIndexed(cmd, s.indexCount, view.count, s.firstIndex, s.vertexOffset, 0);
                ++m_Stats.foliageDrawCalls;
            }
        }
}

} // namespace Engine
