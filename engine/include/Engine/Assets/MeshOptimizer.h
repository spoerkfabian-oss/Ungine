#pragma once
#include "Engine/Assets/Model.h"

#include <cstdint>

namespace Engine {

struct MeshOptimizeSettings {
    bool          optimize        = true; // vertex cache, overdraw and vertex fetch order
    std::uint32_t lodCount        = 4;    // levels incl. LOD 0 (1: none), at most kMaxLods
    float         lodRatio        = 0.5f; // target index count of each level relative to the previous one
    std::uint32_t minLodTriangles = 256;  // smaller submeshes get no LODs
    float         maxLodError     = 0.05f; // relative to the submesh extent; coarser levels are not kept
};

struct MeshOptimizeStats {
    std::uint32_t submeshes       = 0;
    std::uint32_t lodLevels       = 0; // levels generated beyond LOD 0
    std::uint64_t lodIndices      = 0; // indices appended for them
    std::uint32_t reorderedMeshes = 0; // submeshes whose vertices were reordered (fetch optimization)
};

// In place, on a worker thread: reorders each submesh's triangles (and its vertices, where no
// other submesh shares them), then appends simplified index ranges as Submesh::lods. Borders are
// locked so neighbouring submeshes stay crack-free.
MeshOptimizeStats OptimizeMeshes(ModelData& data, const MeshOptimizeSettings& settings);

} // namespace Engine
