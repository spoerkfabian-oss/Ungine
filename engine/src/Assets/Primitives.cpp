#include "Engine/Assets/Primitives.h"

#include <utility>

namespace Engine {

ModelData MakePlane(std::string name, float size, const MaterialData& material, float uvScale)
{
    const float h  = size * 0.5f;
    const float uv = size / uvScale;

    ModelData data;
    data.name = std::move(name);
    // Tangent +X, bitangent sign +1: u grows along +X, v along +Z (glTF UV origin top-left).
    const glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    data.vertices = {
        {.position = {-h, 0.0f, -h}, .uvX = 0.0f, .normal = up, .uvY = 0.0f, .tangent = tangent},
        {.position = {h, 0.0f, -h}, .uvX = uv, .normal = up, .uvY = 0.0f, .tangent = tangent},
        {.position = {h, 0.0f, h}, .uvX = uv, .normal = up, .uvY = uv, .tangent = tangent},
        {.position = {-h, 0.0f, h}, .uvX = 0.0f, .normal = up, .uvY = uv, .tangent = tangent},
    };
    data.indices = {0, 2, 1, 0, 3, 2}; // counter-clockwise seen from +Y

    data.materials = {material};
    data.meshes    = {Mesh{.name      = data.name,
                           .submeshes = {Submesh{.firstIndex   = 0,
                                                 .indexCount   = 6,
                                                 .vertexOffset = 0,
                                                 .material     = 0,
                                                 .boundsMin    = {-h, 0.0f, -h},
                                                 .boundsMax    = {h, 0.0f, h}}}}};
    data.nodes     = {ModelNode{.name = data.name, .local = {}, .mesh = 0, .parent = -1}};
    data.boundsMin = {-h, 0.0f, -h};
    data.boundsMax = {h, 0.0f, h};
    return data;
}

} // namespace Engine
