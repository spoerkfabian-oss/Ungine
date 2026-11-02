#pragma once
#include "Engine/Assets/Texture.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

// Node-graph materials (.umat): nodes (constants, parameters, textures, scene inputs, math,
// Fresnel, Panner, ...) wired into the inputs of one output node. The graph is compiled to GLSL
// (GenerateMaterialCode) and injected into the engine's mesh / shadow shaders, one pipeline set per
// material. Instances (.uminst) override the parameters of a parent material or instance.
// glTF materials stay the default; a .umat replaces them per submesh (MaterialOverrides).

enum class MaterialBlendMode : std::uint8_t { Opaque, Masked, Translucent, Additive };
enum class MaterialShadingModel : std::uint8_t { Lit, Unlit };

[[nodiscard]] const char* ToString(MaterialBlendMode mode);
[[nodiscard]] const char* ToString(MaterialShadingModel model);
[[nodiscard]] std::optional<MaterialBlendMode>    MaterialBlendModeFromString(std::string_view text);
[[nodiscard]] std::optional<MaterialShadingModel> MaterialShadingModelFromString(std::string_view text);

struct MaterialSettings {
    MaterialBlendMode    blendMode    = MaterialBlendMode::Opaque;
    MaterialShadingModel shadingModel = MaterialShadingModel::Lit;
    bool                 twoSided     = false;
    bool                 castShadows  = true;  // opaque / masked only
    float                alphaCutoff  = 0.5f;  // masked: opacity mask below -> discarded
    float                maxDisplacement = 0.0f; // world position offset bound (m): culling boxes grow by it
    bool operator==(const MaterialSettings&) const = default;
};

enum class MaterialParameterType : std::uint8_t { Scalar, Vector, Texture };
[[nodiscard]] const char* ToString(MaterialParameterType type);

// Named value an instance can override (and blueprints at runtime: scalars / vectors).
struct MaterialParameter {
    std::string           name = "Param";
    MaterialParameterType type = MaterialParameterType::Scalar;
    glm::vec4             value{0.0f};     // scalar: x; vector: rgba
    std::string           texture{};       // texture: file (absolute in memory, relative in files)
    TextureKind           textureKind = TextureKind::Color;
    std::string           group{};         // editor grouping
    glm::vec2             range{0.0f, 1.0f}; // scalar slider range (editor)
    bool operator==(const MaterialParameter&) const = default;
};

// Value of a parameter set by an instance / per entity (texture: path).
struct MaterialParameterValue {
    std::string           name{};
    MaterialParameterType type = MaterialParameterType::Scalar;
    glm::vec4             value{0.0f};
    std::string           texture{};
    bool operator==(const MaterialParameterValue&) const = default;
};

// Value of an unconnected input pin (else the pin's default).
struct MaterialPinValue {
    std::string pin{};
    glm::vec4   value{0.0f};
    bool operator==(const MaterialPinValue&) const = default;
};

struct MaterialNode {
    std::uint32_t id = 0;
    std::string   type{};
    glm::vec2     position{0.0f};
    std::string   param{};         // parameter name, component mask, texture file, ...
    glm::vec4     value{0.0f};     // constants, tiling, ...
    TextureKind   textureKind = TextureKind::Color; // Texture.Sample
    std::vector<MaterialPinValue> inputs{}; // values of unconnected inputs
    bool operator==(const MaterialNode&) const = default;
};

// Output pin of one node -> input pin of another (an input takes one link).
struct MaterialLink {
    std::uint32_t fromNode = 0;
    std::string   fromPin{};
    std::uint32_t toNode = 0;
    std::string   toPin{};
    bool operator==(const MaterialLink&) const = default;
};

struct MaterialComment {
    std::uint32_t id = 0;
    std::string   text = "Comment";
    glm::vec2     position{0.0f};
    glm::vec2     size{300.0f, 200.0f};
    glm::vec4     color{0.3f, 0.3f, 0.3f, 0.5f};
    bool operator==(const MaterialComment&) const = default;
};

struct MaterialGraph {
    MaterialSettings               settings{};
    std::vector<MaterialParameter> parameters{};
    std::vector<MaterialNode>      nodes{};
    std::vector<MaterialLink>      links{};
    std::vector<MaterialComment>   comments{};
    std::uint32_t                  nextId = 1;

    [[nodiscard]] MaterialNode*            FindNode(std::uint32_t id);
    [[nodiscard]] const MaterialNode*      FindNode(std::uint32_t id) const;
    [[nodiscard]] const MaterialNode*      OutputNode() const;
    [[nodiscard]] MaterialParameter*       FindParameter(std::string_view name);
    [[nodiscard]] const MaterialParameter* FindParameter(std::string_view name) const;
    [[nodiscard]] const MaterialLink*      LinkInto(std::uint32_t node, std::string_view pin) const;
    std::uint32_t AddNode(std::string type, glm::vec2 position, std::string param = {});
    void          RemoveNode(std::uint32_t id); // and its links (the output node stays)
    // Replaces an existing link into the input. False for unknown nodes / pins or a link to itself.
    bool Connect(std::uint32_t fromNode, std::string_view fromPin, std::uint32_t toNode, std::string_view toPin);
    void Disconnect(std::uint32_t toNode, std::string_view toPin);
    // Renames a parameter and the nodes referring to it.
    bool RenameParameter(std::string_view from, std::string_view to);
    [[nodiscard]] glm::vec4 InputValue(const MaterialNode& node, std::string_view pin, glm::vec4 fallback) const;
    void SetInputValue(MaterialNode& node, std::string_view pin, glm::vec4 value);

    bool operator==(const MaterialGraph&) const = default;
};

// Output node + base color / roughness / metallic parameters.
[[nodiscard]] MaterialGraph MakeDefaultMaterialGraph();

// --- Node types ---

// Component count of a pin: 1..4, 0 = generic (follows the other inputs).
struct MaterialPinDesc {
    std::string   name;
    std::uint32_t width = 1;
    glm::vec4     fallback{0.0f}; // unconnected input
    std::string   implicit{};     // unconnected input reads this instead ("UV0", "Time", "Normal", "WorldPosition")
};

enum class MaterialNodeParam : std::uint8_t {
    None,
    Parameter,  // param = parameter name (type from the node)
    Mask,       // param = components ("rgb", "xz", ...)
    Texture,    // param = texture file, textureKind
};

enum class MaterialNodeValue : std::uint8_t { None, Float, Float2, Float3, Color };

struct MaterialNodeDesc {
    std::string                  type;
    std::string                  title;
    std::string                  category;
    std::string                  description;
    std::vector<MaterialPinDesc> inputs;
    std::vector<MaterialPinDesc> outputs;
    MaterialNodeParam            param = MaterialNodeParam::None;
    MaterialNodeValue            value = MaterialNodeValue::None;
    bool                         pixelOnly = false; // screen position, pixel depth, derivatives
    bool                         hidden    = false; // not in the palette (output node)
};

[[nodiscard]] const std::vector<MaterialNodeDesc>& MaterialNodeTypes();
[[nodiscard]] const MaterialNodeDesc*              FindMaterialNodeType(std::string_view type);
inline constexpr const char*                       kMaterialOutputNode = "Material.Output";

// --- Files ---

[[nodiscard]] std::optional<MaterialGraph> LoadMaterialGraph(const std::filesystem::path& file, std::string* error = nullptr);
[[nodiscard]] std::optional<MaterialGraph> ParseMaterialGraph(std::string_view json, const std::filesystem::path& folder,
                                                              std::string* error = nullptr);
[[nodiscard]] std::string MaterialGraphToJson(const MaterialGraph& graph, const std::filesystem::path& folder);
bool SaveMaterialGraph(const std::filesystem::path& file, const MaterialGraph& graph, std::string* error = nullptr);

// .uminst: parent (.umat or .uminst) + parameter overrides.
struct MaterialInstanceFile {
    std::string                         parent{}; // absolute in memory, relative in files
    std::vector<MaterialParameterValue> overrides{};
    bool operator==(const MaterialInstanceFile&) const = default;
};
[[nodiscard]] std::optional<MaterialInstanceFile> LoadMaterialInstance(const std::filesystem::path& file,
                                                                        std::string* error = nullptr);
[[nodiscard]] std::optional<MaterialInstanceFile> ParseMaterialInstance(std::string_view json,
                                                                         const std::filesystem::path& folder,
                                                                         std::string* error = nullptr);
[[nodiscard]] std::string MaterialInstanceToJson(const MaterialInstanceFile& instance, const std::filesystem::path& folder);
bool SaveMaterialInstance(const std::filesystem::path& file, const MaterialInstanceFile& instance, std::string* error = nullptr);

[[nodiscard]] bool IsMaterialFile(const std::filesystem::path& path);         // .umat
[[nodiscard]] bool IsMaterialInstanceFile(const std::filesystem::path& path); // .uminst

// Parameters of `graph` with the overrides applied in order (unknown names / other types skipped).
[[nodiscard]] std::vector<MaterialParameter> ResolveMaterialParameters(const MaterialGraph& graph,
                                                                       const std::vector<MaterialParameterValue>& overrides);

// --- Code generation ---

struct MaterialProblem {
    std::uint32_t node = 0; // 0: the graph
    std::string   message;
    bool          error = true;
};

// One vec4 of the parameter block (index = slot): a named parameter, a Texture.Sample node's
// texture (texture table entry in x) or the alpha cutoff.
enum class MaterialSlotKind : std::uint8_t { Parameter, Texture, AlphaCutoff };

struct MaterialSlot {
    MaterialSlotKind      kind = MaterialSlotKind::Parameter;
    std::string           name{};    // Parameter
    MaterialParameterType type = MaterialParameterType::Scalar;
    std::uint32_t         node = 0;  // Texture: node id
    std::string           texture{}; // Texture: file
    TextureKind           textureKind = TextureKind::Color;
};

struct MaterialCode {
    std::string                  glsl;     // material.generated.glsl (empty when there are errors)
    std::vector<MaterialSlot>    slots;    // parameter block layout
    std::vector<MaterialProblem> problems; // errors and warnings
    bool                         worldPositionOffset = false;
    bool                         normal = false;
    std::uint64_t                hash = 0; // of glsl (+ settings that change the shaders)
    [[nodiscard]] bool Ok() const;
};

[[nodiscard]] MaterialCode GenerateMaterialCode(const MaterialGraph& graph);

// Shader variants of a material, compiled from the engine's templates with CUSTOM_MATERIAL.
enum class MaterialShader : std::uint8_t {
    Vertex,         // mesh.vert (world position offset, motion vectors)
    Prepass,        // depth_normal.frag
    PrepassPicking, // depth_normal_id.frag
    Lit,            // mesh.frag (opaque / masked)
    Blend,          // mesh_blend.frag (translucent / additive)
    ShadowVertex,   // shadow.vert
    ShadowMask,     // shadow_mask.frag (masked)
    Count
};
[[nodiscard]] const char* ToString(MaterialShader shader);
// Variants the material needs (by blend mode / shadows).
[[nodiscard]] std::vector<MaterialShader> MaterialShaderVariants(const MaterialSettings& settings);
// Template source file and stage of a variant.
[[nodiscard]] const char* MaterialShaderTemplate(MaterialShader shader);
[[nodiscard]] bool        MaterialShaderIsVertex(MaterialShader shader);
// Defines of a variant (blend mode, shading model, two-sided, ...).
[[nodiscard]] std::vector<std::pair<std::string, std::string>> MaterialShaderDefines(const MaterialSettings& settings,
                                                                                     const MaterialCode& code);

} // namespace Engine
