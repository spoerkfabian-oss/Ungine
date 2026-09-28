#include "Engine/Assets/GltfLoader.h"
#include "Engine/Core/Log.h"

#include <cgltf.h>
#define STBI_NO_STDIO
#include <stb_image.h>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Engine {

namespace {

struct CgltfDeleter {
    void operator()(cgltf_data* d) const noexcept { cgltf_free(d); }
};
using CgltfPtr = std::unique_ptr<cgltf_data, CgltfDeleter>;

template <class T>
std::int32_t IndexIn(const T* base, const T* element)
{
    return element ? static_cast<std::int32_t>(element - base) : -1;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

std::filesystem::path Utf8Path(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

Transform NodeTransform(const cgltf_node& node)
{
    Transform t;
    if (node.has_matrix) {
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(glm::make_mat4(node.matrix), t.scale, t.rotation, t.position, skew, perspective);
        return t;
    }
    if (node.has_translation)
        t.position = glm::make_vec3(node.translation);
    if (node.has_rotation) // glTF stores x, y, z, w
        t.rotation = glm::quat(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
    if (node.has_scale)
        t.scale = glm::make_vec3(node.scale);
    return t;
}

class Parser {
public:
    Parser(const std::filesystem::path& path, const cgltf_options& options, const cgltf_data& data)
        : m_BaseDir(path.parent_path()), m_Options(options), m_Data(data) {}

    void Run(ModelData& out)
    {
        m_Out = &out;
        ParseMaterials();
        ParseMeshes();
        ParseNodes();
    }

private:
    std::int32_t Texture(const cgltf_texture_view& view, bool srgb)
    {
        if (!view.texture)
            return kNoTexture;
        const cgltf_image* image = view.texture->image;
        if (!image) {
            ENGINE_WARN("glTF: texture without PNG/JPEG image (basisu/webp?) - using default");
            return kNoTexture;
        }
        if (view.texcoord != 0)
            ENGINE_WARN("glTF: only TEXCOORD_0 is supported, texture will use UV0");

        const auto key = (static_cast<std::uint64_t>(IndexIn(m_Data.images, image)) << 1) | (srgb ? 1u : 0u);
        if (const auto it = m_TextureCache.find(key); it != m_TextureCache.end())
            return it->second;

        const std::int32_t result = Decode(*image, srgb);
        m_TextureCache.emplace(key, result);
        return result;
    }

    std::int32_t Decode(const cgltf_image& image, bool srgb)
    {
        std::vector<std::uint8_t> owned; // file contents or decoded data URI
        const std::uint8_t*       bytes = nullptr;
        std::size_t               size  = 0;

        if (image.buffer_view) {
            const cgltf_buffer_view& bv = *image.buffer_view;
            bytes = static_cast<const std::uint8_t*>(bv.buffer->data) + bv.offset;
            size  = bv.size;
        } else if (image.uri && std::strncmp(image.uri, "data:", 5) == 0) {
            const char* comma = std::strchr(image.uri, ',');
            if (comma && comma - image.uri >= 7 && std::strncmp(comma - 7, ";base64", 7) == 0) {
                const std::size_t len     = std::strlen(comma + 1);
                const std::size_t padding = (len > 0 && comma[len] == '=') + (len > 1 && comma[len - 1] == '=');
                const std::size_t decoded = len / 4 * 3 - padding;
                void*             data    = nullptr;
                if (cgltf_load_buffer_base64(&m_Options, decoded, comma + 1, &data) == cgltf_result_success) {
                    owned.assign(static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + decoded);
                    std::free(data); // default cgltf allocator
                }
            }
            bytes = owned.data();
            size  = owned.size();
        } else if (image.uri) {
            std::string uri = image.uri;
            cgltf_decode_uri(uri.data()); // %20 etc.
            uri.resize(std::strlen(uri.c_str()));
            owned = ReadFile(m_BaseDir / Utf8Path(uri));
            bytes = owned.data();
            size  = owned.size();
        }

        const std::string name = image.name ? image.name : (image.uri && std::strncmp(image.uri, "data:", 5) != 0 ? image.uri : "embedded");
        if (!bytes || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            ENGINE_WARN("glTF: could not read image '{}'", name);
            return kNoTexture;
        }

        int w = 0, h = 0, comp = 0;
        stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &comp, STBI_rgb_alpha);
        if (!pixels) {
            ENGINE_WARN("glTF: failed to decode '{}': {}", name, stbi_failure_reason());
            return kNoTexture;
        }

        TextureData tex;
        tex.name   = name;
        tex.width  = static_cast<std::uint32_t>(w);
        tex.height = static_cast<std::uint32_t>(h);
        tex.srgb   = srgb;
        tex.pixels.assign(pixels, pixels + std::size_t{tex.width} * tex.height * 4);
        stbi_image_free(pixels);

        m_Out->textures.push_back(std::move(tex));
        return static_cast<std::int32_t>(m_Out->textures.size() - 1);
    }

    void ParseMaterials()
    {
        for (cgltf_size i = 0; i < m_Data.materials_count; ++i) {
            const cgltf_material& m = m_Data.materials[i];
            MaterialData md;
            md.name = m.name ? m.name : std::format("Material{}", i);

            if (m.has_pbr_metallic_roughness) {
                const auto& pbr             = m.pbr_metallic_roughness;
                md.baseColorFactor          = glm::make_vec4(pbr.base_color_factor);
                md.metallic                 = pbr.metallic_factor;
                md.roughness                = pbr.roughness_factor;
                md.baseColorTexture         = Texture(pbr.base_color_texture, true);
                md.metallicRoughnessTexture = Texture(pbr.metallic_roughness_texture, false);
            }
            md.normalTexture    = Texture(m.normal_texture, false);
            md.occlusionTexture = Texture(m.occlusion_texture, false);
            md.emissiveTexture  = Texture(m.emissive_texture, true);
            md.emissiveFactor   = glm::make_vec3(m.emissive_factor);
            if (m.has_emissive_strength)
                md.emissiveFactor *= m.emissive_strength.emissive_strength;

            md.alphaMask   = m.alpha_mode == cgltf_alpha_mode_mask;
            md.alphaBlend  = m.alpha_mode == cgltf_alpha_mode_blend;
            md.alphaCutoff = m.alpha_cutoff;
            md.doubleSided = m.double_sided;
            m_Out->materials.push_back(std::move(md));
        }
    }

    std::uint32_t DefaultMaterial()
    {
        if (m_DefaultMaterial < 0) {
            m_Out->materials.push_back(MaterialData{.name = "Default", .metallic = 0.0f, .roughness = 0.8f});
            m_DefaultMaterial = static_cast<std::int32_t>(m_Out->materials.size() - 1);
        }
        return static_cast<std::uint32_t>(m_DefaultMaterial);
    }

    void Unpack(const cgltf_accessor& accessor, std::size_t components)
    {
        m_Scratch.resize(accessor.count * components);
        cgltf_accessor_unpack_floats(&accessor, m_Scratch.data(), m_Scratch.size());
    }

    void ParseMeshes()
    {
        for (cgltf_size mi = 0; mi < m_Data.meshes_count; ++mi) {
            const cgltf_mesh& gm = m_Data.meshes[mi];
            Mesh mesh;
            mesh.name = gm.name ? gm.name : std::format("Mesh{}", mi);

            for (cgltf_size pi = 0; pi < gm.primitives_count; ++pi) {
                const cgltf_primitive& prim = gm.primitives[pi];
                if (prim.type != cgltf_primitive_type_triangles) {
                    ENGINE_WARN("glTF: '{}' primitive {} is not a triangle list - skipped", mesh.name, pi);
                    continue;
                }

                const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *tan = nullptr;
                for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
                    const cgltf_attribute& a = prim.attributes[ai];
                    switch (a.type) {
                    case cgltf_attribute_type_position: pos = a.data; break;
                    case cgltf_attribute_type_normal:   nrm = a.data; break;
                    case cgltf_attribute_type_tangent:  tan = a.data; break;
                    case cgltf_attribute_type_texcoord: if (a.index == 0) uv = a.data; break;
                    default: break;
                    }
                }
                if (!pos || pos->count == 0) {
                    ENGINE_WARN("glTF: '{}' primitive {} has no positions - skipped", mesh.name, pi);
                    continue;
                }

                auto& vertices           = m_Out->vertices;
                const std::size_t base   = vertices.size();
                const std::size_t count  = pos->count;
                if (base + count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
                    throw std::runtime_error("glTF: model exceeds 2^31 vertices");
                vertices.resize(base + count);

                Submesh sm;
                sm.boundsMin = glm::vec3{std::numeric_limits<float>::max()};
                sm.boundsMax = glm::vec3{std::numeric_limits<float>::lowest()};

                Unpack(*pos, 3);
                for (std::size_t i = 0; i < count; ++i) {
                    const glm::vec3 p = glm::make_vec3(&m_Scratch[i * 3]);
                    vertices[base + i].position = p;
                    sm.boundsMin = glm::min(sm.boundsMin, p);
                    sm.boundsMax = glm::max(sm.boundsMax, p);
                }
                if (nrm && nrm->count == count) {
                    Unpack(*nrm, 3);
                    for (std::size_t i = 0; i < count; ++i)
                        vertices[base + i].normal = glm::make_vec3(&m_Scratch[i * 3]);
                } else {
                    ++m_MissingNormals;
                }
                if (uv && uv->count == count) {
                    Unpack(*uv, 2);
                    for (std::size_t i = 0; i < count; ++i) {
                        vertices[base + i].uvX = m_Scratch[i * 2];
                        vertices[base + i].uvY = m_Scratch[i * 2 + 1];
                    }
                }
                if (tan && tan->count == count) {
                    Unpack(*tan, 4);
                    for (std::size_t i = 0; i < count; ++i)
                        vertices[base + i].tangent = glm::make_vec4(&m_Scratch[i * 4]);
                } else {
                    ++m_MissingTangents;
                }

                auto& indices = m_Out->indices;
                sm.firstIndex = static_cast<std::uint32_t>(indices.size());
                if (prim.indices) {
                    const std::size_t first = indices.size();
                    indices.resize(first + prim.indices->count);
                    cgltf_accessor_unpack_indices(prim.indices, indices.data() + first, sizeof(std::uint32_t),
                                                  prim.indices->count);
                } else { // non-indexed: 0..n-1
                    for (std::size_t i = 0; i < count; ++i)
                        indices.push_back(static_cast<std::uint32_t>(i));
                }
                sm.indexCount   = static_cast<std::uint32_t>(indices.size()) - sm.firstIndex;
                sm.vertexOffset = static_cast<std::int32_t>(base);
                sm.material     = prim.material ? static_cast<std::uint32_t>(IndexIn(m_Data.materials, prim.material))
                                                : DefaultMaterial();
                mesh.submeshes.push_back(sm);
            }
            m_Out->meshes.push_back(std::move(mesh));
        }

        if (m_MissingNormals)
            ENGINE_WARN("glTF: {} primitive(s) without normals (flat +Z used)", m_MissingNormals);
        if (m_MissingTangents)
            ENGINE_TRACE("glTF: {} primitive(s) without tangents (derive in shader)", m_MissingTangents);
    }

    void ParseNodes()
    {
        struct Item {
            const cgltf_node* node;
            std::int32_t      parent;
        };
        std::vector<Item>      stack;
        std::vector<glm::mat4> world;

        const cgltf_scene* scene = m_Data.scene ? m_Data.scene : (m_Data.scenes_count ? &m_Data.scenes[0] : nullptr);
        if (scene) {
            for (cgltf_size i = scene->nodes_count; i-- > 0;)
                stack.push_back({scene->nodes[i], -1});
        } else {
            for (cgltf_size i = m_Data.nodes_count; i-- > 0;)
                if (!m_Data.nodes[i].parent)
                    stack.push_back({&m_Data.nodes[i], -1});
        }

        glm::vec3 bmin{std::numeric_limits<float>::max()}, bmax{std::numeric_limits<float>::lowest()};
        bool      anyGeometry = false;

        while (!stack.empty()) {
            const auto [node, parent] = stack.back();
            stack.pop_back();

            ModelNode mn;
            mn.name   = node->name ? node->name : std::format("Node{}", IndexIn(m_Data.nodes, node));
            mn.local  = NodeTransform(*node);
            mn.parent = parent;
            mn.mesh   = IndexIn(m_Data.meshes, node->mesh);

            const glm::mat4 w = (parent >= 0 ? world[static_cast<std::size_t>(parent)] : glm::mat4{1.0f}) *
                                mn.local.LocalMatrix();
            world.push_back(w);

            if (mn.mesh >= 0) {
                for (const Submesh& sm : m_Out->meshes[static_cast<std::size_t>(mn.mesh)].submeshes) {
                    for (int c = 0; c < 8; ++c) { // transform all 8 AABB corners
                        const glm::vec3 corner{(c & 1) ? sm.boundsMax.x : sm.boundsMin.x,
                                               (c & 2) ? sm.boundsMax.y : sm.boundsMin.y,
                                               (c & 4) ? sm.boundsMax.z : sm.boundsMin.z};
                        const glm::vec3 p = glm::vec3(w * glm::vec4(corner, 1.0f));
                        bmin = glm::min(bmin, p);
                        bmax = glm::max(bmax, p);
                        anyGeometry = true;
                    }
                }
            }
            if (node->skin)
                m_HasSkins = true;

            const auto index = static_cast<std::int32_t>(m_Out->nodes.size());
            m_Out->nodes.push_back(std::move(mn));
            for (cgltf_size c = node->children_count; c-- > 0;)
                stack.push_back({node->children[c], index}); // parents always precede children
        }

        if (anyGeometry) {
            m_Out->boundsMin = bmin;
            m_Out->boundsMax = bmax;
        }
        if (m_HasSkins || m_Data.animations_count)
            ENGINE_WARN("glTF: skins/animations are not supported yet - rendering bind pose");
    }

    std::filesystem::path                     m_BaseDir;
    const cgltf_options&                      m_Options;
    const cgltf_data&                         m_Data;
    ModelData*                                m_Out = nullptr;
    std::unordered_map<std::uint64_t, std::int32_t> m_TextureCache;
    std::vector<float>                        m_Scratch;
    std::int32_t                              m_DefaultMaterial = -1;
    std::size_t                               m_MissingNormals  = 0;
    std::size_t                               m_MissingTangents = 0;
    bool                                      m_HasSkins        = false;
};

} // namespace

ModelData LoadGltf(const std::filesystem::path& path)
{
    const std::vector<std::uint8_t> file = ReadFile(path);
    if (file.empty())
        throw std::runtime_error(std::format("glTF: cannot read '{}'", path.string()));

    cgltf_options options{};
    cgltf_data*   raw = nullptr;
    if (const cgltf_result r = cgltf_parse(&options, file.data(), file.size(), &raw); r != cgltf_result_success)
        throw std::runtime_error(std::format("glTF: parse error {} in '{}'", static_cast<int>(r), path.string()));
    CgltfPtr data{raw};

    // External .bin files are resolved relative to the .gltf (cgltf uses fopen here; ASCII paths only).
    if (const cgltf_result r = cgltf_load_buffers(&options, data.get(), path.string().c_str());
        r != cgltf_result_success)
        throw std::runtime_error(std::format("glTF: failed to load buffers ({}) for '{}'", static_cast<int>(r),
                                             path.string()));
    if (const cgltf_result r = cgltf_validate(data.get()); r != cgltf_result_success)
        throw std::runtime_error(std::format("glTF: validation failed ({}) for '{}'", static_cast<int>(r),
                                             path.string()));

    ModelData out;
    out.name = path.stem().string();
    Parser{path, options, *data}.Run(out);

    ENGINE_INFO("Loaded '{}': {} vertices, {} triangles, {} meshes, {} materials, {} textures, {} nodes", out.name,
                out.vertices.size(), out.indices.size() / 3, out.meshes.size(), out.materials.size(),
                out.textures.size(), out.nodes.size());
    return out;
}

} // namespace Engine
