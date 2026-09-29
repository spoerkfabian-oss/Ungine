#include "Engine/Assets/Primitives.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <format>
#include <iterator>
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

ModelData MakeCapsule(std::string name, float radius, float halfHeight, const MaterialData& material,
                      std::uint32_t segments, std::uint32_t rings)
{
    segments = std::max(segments, 3u);
    rings    = std::max(rings + (rings & 1u), 2u); // even: the equator is a row
    ModelData data;
    data.name = std::move(name);

    // Rows from the top pole to the bottom one; the equator appears twice (top and bottom of the
    // cylinder) unless there is no cylinder. v follows the arc length along the profile.
    struct Row {
        float theta, y;
    };
    std::vector<Row> rows;
    for (std::uint32_t i = 0; i <= rings / 2; ++i)
        rows.push_back({glm::pi<float>() * static_cast<float>(i) / static_cast<float>(rings), halfHeight});
    for (std::uint32_t i = rings / 2 + (halfHeight > 0.0f ? 0u : 1u); i <= rings; ++i)
        rows.push_back({glm::pi<float>() * static_cast<float>(i) / static_cast<float>(rings), -halfHeight});
    const float length = glm::pi<float>() * radius + 2.0f * halfHeight;

    for (const Row& row : rows) {
        const float arc = row.theta * radius + (row.y < 0.0f ? 2.0f * halfHeight : 0.0f);
        const float v   = length > 0.0f ? arc / length : 0.0f;
        for (std::uint32_t c = 0; c <= segments; ++c) {
            const float     u   = static_cast<float>(c) / static_cast<float>(segments);
            const float     phi = glm::two_pi<float>() * u;
            const glm::vec3 n{std::sin(row.theta) * std::sin(phi), std::cos(row.theta), std::sin(row.theta) * std::cos(phi)};
            const glm::vec3 t{std::cos(phi), 0.0f, -std::sin(phi)}; // d/du
            const glm::vec3 b{std::cos(row.theta) * std::sin(phi), -std::sin(row.theta),
                              std::cos(row.theta) * std::cos(phi)}; // d/dv (downwards)
            const float     w = glm::dot(glm::cross(n, t), b) > 0.0f ? -1.0f : 1.0f; // as in MakeBox
            data.vertices.push_back({.position = n * radius + glm::vec3(0.0f, row.y, 0.0f),
                                     .uvX      = u,
                                     .normal   = n,
                                     .uvY      = v,
                                     .tangent  = glm::vec4(t, w)});
        }
    }
    // (d/dv x d/du) points outwards: rows downwards, columns around -> counter-clockwise from outside.
    const std::uint32_t stride = segments + 1;
    for (std::uint32_t r = 0; r + 1 < rows.size(); ++r) {
        for (std::uint32_t c = 0; c < segments; ++c) {
            const std::uint32_t a = r * stride + c, b = a + stride;
            if (r > 0) // no zero-area triangles at the top pole
                data.indices.insert(data.indices.end(), {a, b, a + 1});
            if (r + 2 < rows.size()) // ...nor at the bottom one
                data.indices.insert(data.indices.end(), {a + 1, b, b + 1});
        }
    }

    const glm::vec3 extent{radius, radius + halfHeight, radius};
    data.materials = {material};
    data.meshes    = {Mesh{.name      = data.name,
                           .submeshes = {Submesh{.firstIndex   = 0,
                                                 .indexCount   = static_cast<std::uint32_t>(data.indices.size()),
                                                 .vertexOffset = 0,
                                                 .material     = 0,
                                                 .boundsMin    = -extent,
                                                 .boundsMax    = extent}}}};
    data.nodes     = {ModelNode{.name = data.name, .local = {}, .mesh = 0, .parent = -1, .light = {}}};
    data.boundsMin = -extent;
    data.boundsMax = extent;
    return data;
}

namespace {
constexpr const char* kShapeNames[] = {"plane", "box", "sphere", "capsule"};
constexpr const char* kModelNames[] = {"Plane", "Box", "Sphere", "Capsule"};
} // namespace

const char* ToString(PrimitiveShape shape)
{
    return kShapeNames[static_cast<std::size_t>(shape)];
}

std::optional<PrimitiveShape> PrimitiveShapeFromString(std::string_view name)
{
    for (std::size_t i = 0; i < std::size(kShapeNames); ++i)
        if (name == kShapeNames[i])
            return static_cast<PrimitiveShape>(i);
    return std::nullopt;
}

ModelData MakePrimitive(const PrimitiveDesc& desc)
{
    const MaterialData material{.name            = std::string(ToString(desc.shape)),
                                .baseColorFactor = desc.baseColor,
                                .metallic        = desc.metallic,
                                .roughness       = desc.roughness};
    const std::string name = kModelNames[static_cast<std::size_t>(desc.shape)];
    switch (desc.shape) {
    case PrimitiveShape::Plane: return MakePlane(name, desc.size, material);
    case PrimitiveShape::Box: return MakeBox(name, desc.size, material);
    case PrimitiveShape::Sphere: return MakeCapsule(name, desc.size * 0.5f, 0.0f, material);
    case PrimitiveShape::Capsule: return MakeCapsule(name, desc.size * 0.5f, desc.size * 0.5f, material);
    }
    return MakeBox(name, desc.size, material);
}

std::string PrimitiveKey(const PrimitiveDesc& desc)
{
    return std::format("primitive:{}:{}:{},{},{},{}:{}:{}", ToString(desc.shape), desc.size, desc.baseColor.r,
                       desc.baseColor.g, desc.baseColor.b, desc.baseColor.a, desc.metallic, desc.roughness);
}

} // namespace Engine
