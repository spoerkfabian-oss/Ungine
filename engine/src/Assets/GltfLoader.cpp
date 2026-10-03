#include "Engine/Assets/GltfLoader.h"
#include "Engine/Core/Log.h"

#include <cgltf.h>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
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
        ParseSkins();
        ParseAnimations();
        if (!m_Out->skins.empty() || !m_Out->animations.empty())
            ENGINE_WARN("glTF: skeletal data loaded, but runtime animation is not implemented yet - bind pose used");
    }

private:
    std::int32_t Texture(const cgltf_texture_view& view, TextureKind kind)
    {
        if (!view.texture)
            return kNoTexture;
        const cgltf_image* image = view.texture->image;
        if (!image) {
            ENGINE_WARN("glTF: texture without PNG/JPEG/KTX2 image (basisu/webp?) - using default");
            return kNoTexture;
        }
        if (view.texcoord != 0)
            ENGINE_WARN("glTF: only TEXCOORD_0 is supported, texture will use UV0");

        const auto key = (static_cast<std::uint64_t>(IndexIn(m_Data.images, image)) << 2) | static_cast<std::uint64_t>(kind);
        if (const auto it = m_TextureCache.find(key); it != m_TextureCache.end())
            return it->second;

        const std::int32_t result = Source(*image, kind);
        m_TextureCache.emplace(key, result);
        return result;
    }

    // Collects the image's source; decoding and compression happen in the texture asset's job.
    std::int32_t Source(const cgltf_image& image, TextureKind kind)
    {
        TextureData tex;
        tex.kind = kind;
        tex.name = image.name ? image.name
                              : (image.uri && std::strncmp(image.uri, "data:", 5) != 0
                                     ? image.uri
                                     : std::format("image{}", IndexIn(m_Data.images, &image)));

        if (image.buffer_view) {
            const cgltf_buffer_view& bv    = *image.buffer_view;
            const auto*              bytes = static_cast<const std::byte*>(bv.buffer->data) + bv.offset;
            tex.encoded.assign(bytes, bytes + bv.size);
        } else if (image.uri && std::strncmp(image.uri, "data:", 5) == 0) {
            const char* comma = std::strchr(image.uri, ',');
            if (comma && comma - image.uri >= 7 && std::strncmp(comma - 7, ";base64", 7) == 0) {
                const std::size_t len     = std::strlen(comma + 1);
                const std::size_t padding = (len > 0 && comma[len] == '=') + (len > 1 && comma[len - 1] == '=');
                const std::size_t decoded = len / 4 * 3 - padding;
                void*             data    = nullptr;
                if (cgltf_load_buffer_base64(&m_Options, decoded, comma + 1, &data) == cgltf_result_success) {
                    tex.encoded.assign(static_cast<std::byte*>(data), static_cast<std::byte*>(data) + decoded);
                    std::free(data); // default cgltf allocator
                }
            }
        } else if (image.uri) {
            std::string uri = image.uri;
            cgltf_decode_uri(uri.data()); // %20 etc.
            uri.resize(std::strlen(uri.c_str()));
            tex.file = (m_BaseDir / Utf8Path(uri)).lexically_normal();
        }

        if (tex.file.empty() && tex.encoded.empty()) {
            ENGINE_WARN("glTF: could not read image '{}'", tex.name);
            return kErrorTexture;
        }
        tex.hash = TextureContentHash(tex.encoded);
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
                md.baseColorTexture         = Texture(pbr.base_color_texture, TextureKind::Color);
                md.metallicRoughnessTexture = Texture(pbr.metallic_roughness_texture, TextureKind::Linear);
            }
            md.normalTexture     = Texture(m.normal_texture, TextureKind::Normal);
            md.occlusionTexture  = Texture(m.occlusion_texture, TextureKind::Linear);
            md.normalScale       = m.normal_texture.texture ? m.normal_texture.scale : 1.0f;
            md.occlusionStrength = m.occlusion_texture.texture ? m.occlusion_texture.scale : 1.0f;
            md.emissiveTexture  = Texture(m.emissive_texture, TextureKind::Color);
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
                const cgltf_accessor *joints = nullptr, *weights = nullptr;
                for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
                    const cgltf_attribute& a = prim.attributes[ai];
                    switch (a.type) {
                    case cgltf_attribute_type_position: pos = a.data; break;
                    case cgltf_attribute_type_normal:   nrm = a.data; break;
                    case cgltf_attribute_type_tangent:  tan = a.data; break;
                    case cgltf_attribute_type_texcoord: if (a.index == 0) uv = a.data; break;
                    case cgltf_attribute_type_joints:   if (a.index == 0) joints = a.data; break;
                    case cgltf_attribute_type_weights:  if (a.index == 0) weights = a.data; break;
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

                if ((joints == nullptr) != (weights == nullptr)) {
                    ENGINE_WARN("glTF: '{}' primitive {} has only one of JOINTS_0 / WEIGHTS_0 - skinning ignored",
                                mesh.name, pi);
                } else if (joints && (joints->count != count || weights->count != count ||
                                      cgltf_num_components(joints->type) != 4 ||
                                      cgltf_num_components(weights->type) != 4)) {
                    if (!m_Out->skinInfluences.empty())
                        m_Out->skinInfluences.resize(base + count);
                    ENGINE_WARN("glTF: '{}' primitive {} has invalid JOINTS_0 / WEIGHTS_0 - skinning ignored",
                                mesh.name, pi);
                } else if (joints && weights) {
                    m_Out->skinInfluences.resize(base + count);
                    std::vector<float> jointValues(count * 4);
                    Unpack(*joints, 4);
                    jointValues = m_Scratch;
                    Unpack(*weights, 4);
                    for (std::size_t i = 0; i < count; ++i) {
                        VertexSkinInfluence influence;
                        for (std::size_t c = 0; c < 4; ++c) {
                            const float joint = jointValues[i * 4 + c];
                            if (std::isfinite(joint) && joint >= 0.0f &&
                                joint <= static_cast<float>(std::numeric_limits<std::uint16_t>::max()))
                                influence.joints[static_cast<glm::length_t>(c)] = static_cast<std::uint16_t>(joint);
                            float weight = m_Scratch[i * 4 + c];
                            influence.weights[static_cast<glm::length_t>(c)] =
                                std::isfinite(weight) ? std::max(weight, 0.0f) : 0.0f;
                        }
                        const float sum = influence.weights.x + influence.weights.y + influence.weights.z + influence.weights.w;
                        if (sum > 1.0e-6f)
                            influence.weights /= sum;
                        else {
                            influence.joints = glm::u16vec4{0};
                            influence.weights = glm::vec4{1.0f, 0.0f, 0.0f, 0.0f};
                        }
                        m_Out->skinInfluences[base + i] = influence;
                    }
                } else if (!m_Out->skinInfluences.empty()) {
                    m_Out->skinInfluences.resize(base + count);
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
        m_NodeIndices.assign(m_Data.nodes_count, -1);

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
            mn.skin   = IndexIn(m_Data.skins, node->skin);

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
            if (node->light)
                mn.light = ConvertLight(*node->light);

            const auto index = static_cast<std::int32_t>(m_Out->nodes.size());
            const auto sourceIndex = IndexIn(m_Data.nodes, node);
            if (sourceIndex >= 0)
                m_NodeIndices[static_cast<std::size_t>(sourceIndex)] = index;
            m_Out->nodes.push_back(std::move(mn));
            for (cgltf_size c = node->children_count; c-- > 0;)
                stack.push_back({node->children[c], index}); // parents always precede children
        }

        if (anyGeometry) {
            m_Out->boundsMin = bmin;
            m_Out->boundsMax = bmax;
        }
        if (m_IgnoredDirectional)
            ENGINE_WARN("glTF: directional lights ignored (the sky's sun is the only directional light)");
    }

    std::int32_t OutputNode(const cgltf_node* node) const
    {
        const std::int32_t sourceIndex = IndexIn(m_Data.nodes, node);
        if (sourceIndex < 0 || static_cast<std::size_t>(sourceIndex) >= m_NodeIndices.size())
            return -1;
        return m_NodeIndices[static_cast<std::size_t>(sourceIndex)];
    }

    void ParseSkins()
    {
        for (cgltf_size si = 0; si < m_Data.skins_count; ++si) {
            const cgltf_skin& source = m_Data.skins[si];
            Skin skin;
            skin.name = source.name ? source.name : std::format("Skin{}", si);
            skin.skeletonRoot = OutputNode(source.skeleton);
            if (source.skeleton && skin.skeletonRoot < 0)
                ENGINE_WARN("glTF: skin '{}' skeleton root is outside the active scene", skin.name);

            const bool validMatrices = source.inverse_bind_matrices &&
                source.inverse_bind_matrices->type == cgltf_type_mat4 &&
                source.inverse_bind_matrices->count >= source.joints_count;
            if (source.inverse_bind_matrices && !validMatrices)
                ENGINE_WARN("glTF: skin '{}' has invalid inverse bind matrices; identity matrices used", skin.name);
            if (validMatrices)
                Unpack(*source.inverse_bind_matrices, 16);

            skin.joints.reserve(source.joints_count);
            for (cgltf_size ji = 0; ji < source.joints_count; ++ji) {
                SkinJoint joint;
                joint.node = OutputNode(source.joints[ji]);
                if (joint.node < 0)
                    ENGINE_WARN("glTF: skin '{}' joint {} is outside the active scene", skin.name, ji);
                if (validMatrices)
                    joint.inverseBindMatrix = glm::make_mat4(&m_Scratch[ji * 16]);
                skin.joints.push_back(joint);
            }
            m_Out->skins.push_back(std::move(skin));
        }
    }

    void ParseAnimations()
    {
        for (cgltf_size ai = 0; ai < m_Data.animations_count; ++ai) {
            const cgltf_animation& source = m_Data.animations[ai];
            AnimationClip clip;
            clip.name = source.name ? source.name : std::format("Animation{}", ai);

            for (cgltf_size ci = 0; ci < source.channels_count; ++ci) {
                const cgltf_animation_channel& channel = source.channels[ci];
                if (!channel.sampler || !channel.sampler->input || !channel.sampler->output || !channel.target_node) {
                    ENGINE_WARN("glTF: animation '{}' channel {} is incomplete - skipped", clip.name, ci);
                    continue;
                }
                const std::int32_t node = OutputNode(channel.target_node);
                if (node < 0) {
                    ENGINE_WARN("glTF: animation '{}' targets a node outside the active scene - skipped", clip.name);
                    continue;
                }

                AnimationPath path;
                std::size_t components = 3;
                switch (channel.target_path) {
                case cgltf_animation_path_type_translation: path = AnimationPath::Translation; break;
                case cgltf_animation_path_type_rotation: path = AnimationPath::Rotation; components = 4; break;
                case cgltf_animation_path_type_scale: path = AnimationPath::Scale; break;
                case cgltf_animation_path_type_weights:
                    ENGINE_WARN("glTF: animation '{}' morph-weight channel is not supported yet - skipped", clip.name);
                    continue;
                default:
                    ENGINE_WARN("glTF: animation '{}' has an unknown target path - skipped", clip.name);
                    continue;
                }

                const cgltf_animation_sampler& sampler = *channel.sampler;
                const bool cubic = sampler.interpolation == cgltf_interpolation_type_cubic_spline;
                const cgltf_size keyCount = sampler.input->count;
                const cgltf_size expectedValues = keyCount * (cubic ? 3 : 1);
                if (sampler.input->type != cgltf_type_scalar || sampler.output->count != expectedValues ||
                    cgltf_num_components(sampler.output->type) != components) {
                    ENGINE_WARN("glTF: animation '{}' channel {} has unsupported or mismatched accessors - skipped",
                                clip.name, ci);
                    continue;
                }

                AnimationTrack track;
                track.node = static_cast<std::uint32_t>(node);
                track.path = path;
                switch (sampler.interpolation) {
                case cgltf_interpolation_type_step: track.interpolation = AnimationInterpolation::Step; break;
                case cgltf_interpolation_type_cubic_spline: track.interpolation = AnimationInterpolation::CubicSpline; break;
                default: track.interpolation = AnimationInterpolation::Linear; break;
                }

                Unpack(*sampler.input, 1);
                track.times = m_Scratch;
                const bool invalidTimes = track.times.empty() ||
                    std::any_of(track.times.begin(), track.times.end(),
                                [](float time) { return !std::isfinite(time); }) ||
                    std::adjacent_find(track.times.begin(), track.times.end(), [](float a, float b) {
                        return a >= b;
                    }) != track.times.end();
                if (invalidTimes) {
                    ENGINE_WARN("glTF: animation '{}' channel {} has empty, non-finite, or unordered key times - skipped",
                                clip.name, ci);
                    continue;
                }
                clip.duration = std::max(clip.duration, track.times.back());

                Unpack(*sampler.output, components);
                const auto valueAt = [&](cgltf_size index) {
                    glm::vec4 value{0.0f};
                    for (std::size_t c = 0; c < components; ++c)
                        value[static_cast<glm::length_t>(c)] = m_Scratch[index * components + c];
                    return value;
                };
                track.values.reserve(keyCount);
                if (cubic) {
                    track.inTangents.reserve(keyCount);
                    track.outTangents.reserve(keyCount);
                    for (cgltf_size key = 0; key < keyCount; ++key) {
                        track.inTangents.push_back(valueAt(key * 3));
                        track.values.push_back(valueAt(key * 3 + 1));
                        track.outTangents.push_back(valueAt(key * 3 + 2));
                    }
                } else {
                    for (cgltf_size key = 0; key < keyCount; ++key)
                        track.values.push_back(valueAt(key));
                }
                clip.tracks.push_back(std::move(track));
            }
            m_Out->animations.push_back(std::move(clip));
        }
    }

    // KHR_lights_punctual: same conventions (candela, local -Z, cone half-angles, range 0 = unbounded).
    std::optional<Light> ConvertLight(const cgltf_light& light)
    {
        Light out{.color = glm::make_vec3(light.color), .intensity = light.intensity, .range = light.range};
        switch (light.type) {
        case cgltf_light_type_point:
            out.type = LightType::Point;
            return out;
        case cgltf_light_type_spot:
            out.type           = LightType::Spot;
            out.outerConeAngle = std::clamp(light.spot_outer_cone_angle, 0.0f, glm::half_pi<float>());
            out.innerConeAngle = std::clamp(light.spot_inner_cone_angle, 0.0f, out.outerConeAngle);
            return out;
        default:
            m_IgnoredDirectional = true;
            return std::nullopt;
        }
    }

    std::filesystem::path                     m_BaseDir;
    const cgltf_options&                      m_Options;
    const cgltf_data&                         m_Data;
    ModelData*                                m_Out = nullptr;
    std::unordered_map<std::uint64_t, std::int32_t> m_TextureCache;
    std::vector<float>                        m_Scratch;
    std::vector<std::int32_t>                 m_NodeIndices;
    std::int32_t                              m_DefaultMaterial = -1;
    std::size_t                               m_MissingNormals  = 0;
    std::size_t                               m_MissingTangents = 0;
    bool                                      m_IgnoredDirectional = false;
};

} // namespace

namespace {
std::string Utf8(const std::filesystem::path& path)
{
    const std::u8string s = path.u8string();
    return {s.begin(), s.end()};
}

// cgltf resolves external buffers by concatenating the .gltf path (we pass UTF-8) and the URI;
// reading through std::filesystem keeps non-ASCII paths working on Windows (fopen would not).
cgltf_result ReadUtf8File(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size,
                          void** data)
{
    const std::vector<std::uint8_t> bytes = ReadFile(Utf8Path(path));
    if (bytes.empty())
        return cgltf_result_file_not_found;
    void* copy = std::malloc(bytes.size());
    if (!copy)
        return cgltf_result_out_of_memory;
    std::memcpy(copy, bytes.data(), bytes.size());
    *size = bytes.size();
    *data = copy;
    return cgltf_result_success;
}

void ReleaseFile(const cgltf_memory_options*, const cgltf_file_options*, void* data)
{
    std::free(data);
}
} // namespace

ModelData LoadGltf(const std::filesystem::path& path)
{
    const std::vector<std::uint8_t> file = ReadFile(path);
    if (file.empty())
        throw std::runtime_error(std::format("glTF: cannot read '{}'", Utf8(path)));

    cgltf_options options{};
    options.file.read    = ReadUtf8File;
    options.file.release = ReleaseFile;
    cgltf_data*   raw = nullptr;
    if (const cgltf_result r = cgltf_parse(&options, file.data(), file.size(), &raw); r != cgltf_result_success)
        throw std::runtime_error(std::format("glTF: parse error {} in '{}'", static_cast<int>(r), Utf8(path)));
    CgltfPtr data{raw};

    // External .bin files are resolved relative to the .gltf (through ReadUtf8File).
    if (const cgltf_result r = cgltf_load_buffers(&options, data.get(), Utf8(path).c_str());
        r != cgltf_result_success)
        throw std::runtime_error(std::format("glTF: failed to load buffers ({}) for '{}'", static_cast<int>(r),
                                             Utf8(path)));
    if (const cgltf_result r = cgltf_validate(data.get()); r != cgltf_result_success)
        throw std::runtime_error(std::format("glTF: validation failed ({}) for '{}'", static_cast<int>(r),
                                             Utf8(path)));

    ModelData out;
    out.name = Utf8(path.stem());
    Parser{path, options, *data}.Run(out);
    for (cgltf_size i = 0; i < data->buffers_count; ++i) { // external .bin files
        const char* uri = data->buffers[i].uri;
        if (uri && std::strncmp(uri, "data:", 5) != 0) {
            std::string decoded = uri;
            cgltf_decode_uri(decoded.data());
            decoded.resize(std::strlen(decoded.c_str()));
            out.dependencies.push_back((path.parent_path() / Utf8Path(decoded)).lexically_normal());
        }
    }

    ENGINE_INFO("Loaded '{}': {} vertices, {} triangles, {} meshes, {} materials, {} textures, {} nodes", out.name,
                out.vertices.size(), out.indices.size() / 3, out.meshes.size(), out.materials.size(),
                out.textures.size(), out.nodes.size());
    return out;
}

} // namespace Engine
