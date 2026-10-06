#include "Engine/Assets/MeshOptimizer.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <stdexcept>

namespace Engine {

namespace {
constexpr float kOverdrawThreshold = 1.05f; // allowed vertex cache degradation for less overdraw

// Vertex range [first, first + count) of every submesh; true where no other submesh overlaps it.
std::vector<bool> ExclusiveVertexRanges(const ModelData& data, const std::vector<std::uint32_t>& counts)
{
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges; // begin, end
    for (const Mesh& mesh : data.meshes)
        for (const Submesh& sm : mesh.submeshes) {
            const auto begin = static_cast<std::uint64_t>(sm.vertexOffset);
            ranges.emplace_back(begin, begin + counts[ranges.size()]);
        }
    std::vector<std::size_t> order(ranges.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return ranges[a] < ranges[b]; });

    std::vector<bool> exclusive(ranges.size(), true);
    std::uint64_t     reach = 0;
    std::size_t       reachOwner = 0;
    for (std::size_t k = 0; k < order.size(); ++k) {
        const auto [begin, end] = ranges[order[k]];
        if (k > 0 && begin < reach) { // overlaps an earlier range
            exclusive[order[k]] = false;
            exclusive[reachOwner] = false;
        }
        if (end > reach) {
            reach      = end;
            reachOwner = order[k];
        }
    }
    return exclusive;
}
} // namespace

MeshOptimizeStats OptimizeMeshes(ModelData& data, const MeshOptimizeSettings& settings)
{
    MeshOptimizeStats stats;
    const std::uint32_t lodCount = std::clamp(settings.lodCount, 1u, kMaxLods);
    if (data.optimized) // cooked: already done (a second pass would add LODs of LODs)
        return stats;
    data.optimized = true;
    if (!settings.optimize && lodCount == 1)
        return stats;

    // Vertex count of each submesh: highest referenced index + 1.
    std::vector<std::uint32_t> counts;
    for (const Mesh& mesh : data.meshes)
        for (const Submesh& sm : mesh.submeshes) {
            if (std::uint64_t{sm.firstIndex} + sm.indexCount > data.indices.size())
                throw std::runtime_error("OptimizeMeshes: index range out of bounds");
            std::uint32_t highest = 0;
            for (std::uint32_t i = 0; i < sm.indexCount; ++i)
                highest = std::max(highest, data.indices[sm.firstIndex + i]);
            const std::uint32_t count = sm.indexCount > 0 ? highest + 1 : 0;
            if (static_cast<std::uint64_t>(sm.vertexOffset) + count > data.vertices.size())
                throw std::runtime_error("OptimizeMeshes: vertex range out of bounds");
            counts.push_back(count);
        }
    const std::vector<bool> exclusive = ExclusiveVertexRanges(data, counts);

    std::vector<std::uint32_t> indices, scratch, remap;
    std::size_t                k = 0;
    for (Mesh& mesh : data.meshes) {
        for (Submesh& sm : mesh.submeshes) {
            const std::uint32_t vertexCount = counts[k];
            const bool          ownVertices = exclusive[k];
            ++k;
            ++stats.submeshes;
            if (sm.indexCount < 3 || sm.indexCount % 3 != 0)
                continue;

            Vertex* const  vertices  = data.vertices.data() + sm.vertexOffset;
            const float*   positions = &vertices[0].position.x;
            std::uint32_t* lod0      = data.indices.data() + sm.firstIndex;
            indices.assign(lod0, lod0 + sm.indexCount);

            if (settings.optimize) {
                scratch.resize(indices.size());
                meshopt_optimizeVertexCache(scratch.data(), indices.data(), indices.size(), vertexCount);
                meshopt_optimizeOverdraw(indices.data(), scratch.data(), indices.size(), positions, vertexCount,
                                         sizeof(Vertex), kOverdrawThreshold);
                if (ownVertices) { // shared vertices would break the other submesh
                    remap.resize(vertexCount);
                    meshopt_optimizeVertexFetchRemap(remap.data(), indices.data(), indices.size(), vertexCount);
                    meshopt_remapVertexBuffer(vertices, vertices, vertexCount, sizeof(Vertex), remap.data());
                    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
                    ++stats.reorderedMeshes;
                }
                std::ranges::copy(indices, lod0);
            }

            sm.lodCount = 1;
            sm.lods[0]  = {.firstIndex = sm.firstIndex, .indexCount = sm.indexCount, .error = 0.0f};
            if (lodCount == 1 || sm.indexCount / 3 < settings.minLodTriangles)
                continue;

            const float scale  = meshopt_simplifyScale(positions, vertexCount, sizeof(Vertex));
            std::size_t target = sm.indexCount;
            for (std::uint32_t level = 1; level < lodCount; ++level) {
                target = static_cast<std::size_t>(static_cast<float>(target) * settings.lodRatio) / 3 * 3;
                if (target < 3)
                    break;
                scratch.resize(indices.size());
                float error = 0.0f;
                // Always from LOD 0: errors do not accumulate across levels.
                const std::size_t count = meshopt_simplify(scratch.data(), indices.data(), indices.size(), positions,
                                                           vertexCount, sizeof(Vertex), target, settings.maxLodError,
                                                           meshopt_SimplifyLockBorder, &error);
                const std::uint32_t previous = sm.lods[level - 1].indexCount;
                if (count == 0 || count * 10 > std::size_t{previous} * 9) // < 10 % fewer: not worth a level
                    break;
                scratch.resize(count);
                meshopt_optimizeVertexCache(scratch.data(), scratch.data(), count, vertexCount);

                sm.lods[level] = {.firstIndex = static_cast<std::uint32_t>(data.indices.size()),
                                  .indexCount = static_cast<std::uint32_t>(count),
                                  .error      = error * scale};
                data.indices.insert(data.indices.end(), scratch.begin(), scratch.end());
                lod0     = data.indices.data() + sm.firstIndex; // insert may reallocate
                target   = count;
                sm.lodCount = level + 1;
                ++stats.lodLevels;
                stats.lodIndices += count;
            }
        }
    }
    return stats;
}

} // namespace Engine
