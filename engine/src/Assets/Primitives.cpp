#include "Engine/Assets/Primitives.h"

#include <format>
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
    data.nodes     = {ModelNode{.name = data.name, .local = {}, .mesh = 0, .parent = -1, .light = {}}};
    data.boundsMin = {-h, 0.0f, -h};
    data.boundsMax = {h, 0.0f, h};
    return data;
}

ModelData MakeBox(std::string name, float size, const MaterialData& material)
{
    const float h = size * 0.5f;
    ModelData   data;
    data.name = std::move(name);

    // Per face: normal, tangent (u direction), bitangent (v direction, downwards in UV space).
    struct Face {
        glm::vec3 n, t, b;
    };
    static constexpr Face kFaces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, -1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},   {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, -1, 0}},
    };
    for (const Face& f : kFaces) {
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        for (int i = 0; i < 4; ++i) {
            const float u = (i == 1 || i == 2) ? 1.0f : 0.0f;
            const float v = (i >= 2) ? 1.0f : 0.0f;
            const glm::vec3 p = (f.n + f.t * (u * 2.0f - 1.0f) + f.b * (v * 2.0f - 1.0f)) * h;
            // Bitangent = cross(normal, tangent) * w points towards decreasing v (as in MakePlane).
            const float w = glm::dot(glm::cross(f.n, f.t), f.b) > 0.0f ? -1.0f : 1.0f;
            data.vertices.push_back({.position = p, .uvX = u, .normal = f.n, .uvY = v, .tangent = glm::vec4(f.t, w)});
        }
        // Counter-clockwise seen from outside: (t, b) is left-handed w.r.t. n when w < 0.
        const bool ccw = glm::dot(glm::cross(f.t, f.b), f.n) > 0.0f;
        const std::uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i)
            data.indices.push_back(base + (ccw ? quad[i] : quad[5 - i]));
    }

    data.materials = {material};
    data.meshes    = {Mesh{.name      = data.name,
                           .submeshes = {Submesh{.firstIndex   = 0,
                                                 .indexCount   = static_cast<std::uint32_t>(data.indices.size()),
                                                 .vertexOffset = 0,
                                                 .material     = 0,
                                                 .boundsMin    = glm::vec3(-h),
                                                 .boundsMax    = glm::vec3(h)}}}};
    data.nodes     = {ModelNode{.name = data.name, .local = {}, .mesh = 0, .parent = -1, .light = {}}};
    data.boundsMin = glm::vec3(-h);
    data.boundsMax = glm::vec3(h);
    return data;
}

const char* ToString(PrimitiveShape shape)
{
    return shape == PrimitiveShape::Plane ? "plane" : "box";
}

ModelData MakePrimitive(const PrimitiveDesc& desc)
{
    const MaterialData material{.name            = std::string(ToString(desc.shape)),
                                .baseColorFactor = desc.baseColor,
                                .metallic        = desc.metallic,
                                .roughness       = desc.roughness};
    const std::string name = desc.shape == PrimitiveShape::Plane ? "Plane" : "Box";
    return desc.shape == PrimitiveShape::Plane ? MakePlane(name, desc.size, material) : MakeBox(name, desc.size, material);
}

std::string PrimitiveKey(const PrimitiveDesc& desc)
{
    return std::format("primitive:{}:{}:{},{},{},{}:{}:{}", ToString(desc.shape), desc.size, desc.baseColor.r,
                       desc.baseColor.g, desc.baseColor.b, desc.baseColor.a, desc.metallic, desc.roughness);
}

} // namespace Engine
