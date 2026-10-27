// Phase 33 materials (CPU): node graph files, instances, code generation and compiling the
// generated shaders from the engine's templates (glslang). Rendering is covered by the GPU tests.
#include "BlueprintTestUtil.h"
#include "Test.h"

#include "Engine/Core/Platform.h"
#include "Engine/Material/MaterialGraph.h"
#include "Engine/Renderer/ShaderCompiler.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path MaterialTempDir(const char* name)
{
    const fs::path  dir = fs::temp_directory_path() / "ungine_tests" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool HasError(const MaterialCode& code, std::uint32_t node, std::string_view text)
{
    return std::any_of(code.problems.begin(), code.problems.end(), [&](const MaterialProblem& p) {
        return p.error && p.node == node && p.message.find(text) != std::string::npos;
    });
}

std::uint32_t OutputId(const MaterialGraph& graph)
{
    return graph.OutputNode()->id;
}

// Compiles every shader variant the material needs; returns the first failure's log.
std::string CompileVariants(const MaterialGraph& graph)
{
    const MaterialCode code = GenerateMaterialCode(graph);
    if (!code.Ok())
        return "codegen failed: " + (code.problems.empty() ? std::string{} : code.problems.front().message);
    for (const MaterialShader variant : MaterialShaderVariants(graph.settings)) {
        ShaderCompileOptions options;
        options.defines = MaterialShaderDefines(graph.settings, code);
        options.virtualFiles.push_back({"material.generated.glsl", code.glsl});
        const ShaderCompileResult result =
            CompileEngineShader(MaterialShaderIsVertex(variant) ? ShaderStage::Vertex : ShaderStage::Fragment,
                                MaterialShaderTemplate(variant), options);
        if (!result.Ok())
            return std::string(ToString(variant)) + ": " + result.log;
    }
    return {};
}

} // namespace

TEST_CASE(Material_GraphFilesAndInstances)
{
    const fs::path dir = MaterialTempDir("material_files");
    MaterialGraph  graph = MakeDefaultMaterialGraph();
    CHECK(graph.OutputNode() != nullptr);
    CHECK(graph.parameters.size() == 3);
    CHECK(GenerateMaterialCode(graph).Ok());

    // Texture parameter + texture node with paths outside the material folder.
    MaterialParameter tex;
    tex.name        = "Albedo";
    tex.type        = MaterialParameterType::Texture;
    tex.texture     = PathToUtf8(dir / "Textures" / "albedo.png");
    tex.textureKind = TextureKind::Color;
    graph.parameters.push_back(tex);
    const std::uint32_t sample = graph.AddNode("Texture.Sample", {0.0f, 200.0f}, PathToUtf8(dir / "Textures" / "n.png"));
    graph.FindNode(sample)->textureKind = TextureKind::Normal;
    CHECK(graph.Connect(sample, "RGB", OutputId(graph), "Normal"));
    CHECK(!graph.Connect(sample, "Nope", OutputId(graph), "Normal"));
    CHECK(!graph.Connect(sample, "RGB", sample, "UV"));
    graph.settings.blendMode       = MaterialBlendMode::Masked;
    graph.settings.twoSided        = true;
    graph.settings.alphaCutoff     = 0.3f;
    graph.settings.maxDisplacement = 0.25f;
    graph.comments.push_back({graph.nextId++, "Note", {1.0f, 2.0f}, {100.0f, 50.0f}, {1.0f, 0.0f, 0.0f, 0.5f}});
    MaterialNode* output = graph.FindNode(OutputId(graph));
    graph.SetInputValue(*output, "Emissive", glm::vec4(0.5f, 0.25f, 0.0f, 0.0f));

    const fs::path file = dir / "Materials" / "Test.umat";
    std::string    error;
    CHECK(SaveMaterialGraph(file, graph, &error));
    const std::string text = MaterialGraphToJson(graph, file.parent_path());
    CHECK(text.find("../Textures/albedo.png") != std::string::npos); // relative in the file
    const std::optional<MaterialGraph> loaded = LoadMaterialGraph(file, &error);
    CHECK(loaded.has_value());
    CHECK(*loaded == graph);

    // Renaming a parameter follows its nodes.
    MaterialGraph renamed = graph;
    CHECK(renamed.RenameParameter("BaseColor", "Tint"));
    CHECK(!renamed.RenameParameter("Tint", "Roughness")); // taken
    CHECK(GenerateMaterialCode(renamed).Ok());
    CHECK(renamed.FindParameter("Tint") && !renamed.FindParameter("BaseColor"));

    // Removing a node drops its links; the output node stays.
    MaterialGraph removed = graph;
    removed.RemoveNode(sample);
    removed.RemoveNode(OutputId(removed));
    CHECK(removed.OutputNode() != nullptr);
    CHECK(!removed.LinkInto(OutputId(removed), "Normal"));

    // Broken files: missing output node is added, duplicate ids / links into one input dropped.
    const std::optional<MaterialGraph> repaired = ParseMaterialGraph(
        R"({"nodes":[{"id":3,"type":"Constant","value":[2,0,0,0]},{"id":3,"type":"Constant"}],
            "links":[{"from":[3,"Value"],"to":[9,"X"]}], "settings":{"alphaCutoff":7}})",
        dir, &error);
    CHECK(repaired.has_value());
    CHECK(repaired->nodes.size() == 2 && repaired->OutputNode());
    CHECK(repaired->links.empty());
    CHECK(repaired->settings.alphaCutoff == 1.0f);
    CHECK(repaired->nextId > 3);
    CHECK(!ParseMaterialGraph("{ not json", dir, &error).has_value());

    // Instances: parent relative, overrides by name and type.
    MaterialInstanceFile instance;
    instance.parent = PathToUtf8(file);
    instance.overrides.push_back({"Roughness", MaterialParameterType::Scalar, glm::vec4(0.9f, 0, 0, 0), {}});
    instance.overrides.push_back({"Albedo", MaterialParameterType::Texture, glm::vec4(0.0f), PathToUtf8(dir / "Textures" / "b.png")});
    instance.overrides.push_back({"BaseColor", MaterialParameterType::Scalar, glm::vec4(1.0f), {}}); // wrong type
    const fs::path instanceFile = dir / "Materials" / "Instances" / "Red.uminst";
    CHECK(SaveMaterialInstance(instanceFile, instance, &error));
    CHECK(MaterialInstanceToJson(instance, instanceFile.parent_path()).find("\"../Test.umat\"") != std::string::npos);
    const std::optional<MaterialInstanceFile> loadedInstance = LoadMaterialInstance(instanceFile, &error);
    CHECK(loadedInstance && *loadedInstance == instance);
    const std::vector<MaterialParameter> resolved = ResolveMaterialParameters(graph, instance.overrides);
    const auto find = [&](std::string_view name) {
        return *std::find_if(resolved.begin(), resolved.end(), [&](const MaterialParameter& p) { return p.name == name; });
    };
    CHECK(find("Roughness").value.x == 0.9f);
    CHECK(find("Albedo").texture == PathToUtf8(dir / "Textures" / "b.png"));
    CHECK(find("BaseColor").value == glm::vec4(0.8f, 0.8f, 0.8f, 1.0f)); // type mismatch ignored
    CHECK(IsMaterialFile("a/B.UMAT") && IsMaterialInstanceFile("x.uminst") && !IsMaterialFile("x.uminst"));
}

TEST_CASE(Material_CodegenProblemsAndSlots)
{
    MaterialGraph graph = MakeDefaultMaterialGraph();
    const std::uint32_t out = OutputId(graph);

    // Slot layout: parameters, node textures, alpha cutoff last.
    const std::uint32_t tex = graph.AddNode("Texture.Sample", {0, 0}, "");
    const std::uint32_t mul = graph.AddNode("Math.Multiply", {0, 0});
    graph.Connect(tex, "RGB", mul, "A");
    graph.Connect(graph.nodes[1].id, "RGB", mul, "B"); // BaseColor parameter
    graph.Connect(mul, "Result", out, "BaseColor");
    MaterialCode code = GenerateMaterialCode(graph);
    CHECK(code.Ok());
    CHECK(code.slots.size() == 5);
    CHECK(code.slots[0].kind == MaterialSlotKind::Parameter && code.slots[0].name == "BaseColor");
    CHECK(code.slots[3].kind == MaterialSlotKind::Texture && code.slots[3].node == tex);
    CHECK(code.slots[4].kind == MaterialSlotKind::AlphaCutoff);
    CHECK(code.glsl.find("MATERIAL_ALPHA_CUTOFF_SLOT 4u") != std::string::npos);
    CHECK(std::any_of(code.problems.begin(), code.problems.end(),
                      [&](const MaterialProblem& p) { return !p.error && p.node == tex; })); // no texture: warning
    CHECK(!code.worldPositionOffset && !code.normal);
    const std::uint64_t hash = code.hash;
    CHECK(GenerateMaterialCode(graph).hash == hash); // deterministic

    // Parameter values do not change the code (instances share the shaders).
    MaterialGraph valueEdit = graph;
    valueEdit.FindParameter("Roughness")->value.x = 0.1f;
    CHECK(GenerateMaterialCode(valueEdit).hash == hash);

    // Width mismatch (vec2 * vec3).
    MaterialGraph widths = graph;
    const std::uint32_t c2 = widths.AddNode("Constant2", {0, 0});
    widths.Connect(c2, "Value", mul, "A");
    CHECK(HasError(GenerateMaterialCode(widths), mul, "components"));

    // Cycle.
    MaterialGraph cycle = MakeDefaultMaterialGraph();
    const std::uint32_t a = cycle.AddNode("Math.Add", {0, 0});
    const std::uint32_t b = cycle.AddNode("Math.Add", {0, 0});
    cycle.Connect(a, "Result", b, "A");
    cycle.Connect(b, "Result", a, "A");
    cycle.Connect(a, "Result", OutputId(cycle), "Metallic");
    CHECK(HasError(GenerateMaterialCode(cycle), b, "cycle") || HasError(GenerateMaterialCode(cycle), a, "cycle"));

    // Unknown / mistyped parameter, unknown node type, pixel-only node in the vertex stage.
    MaterialGraph params = MakeDefaultMaterialGraph();
    const std::uint32_t missing = params.AddNode("Param.Scalar", {0, 0}, "Nope");
    const std::uint32_t typed   = params.AddNode("Param.Scalar", {0, 0}, "BaseColor");
    params.Connect(missing, "Value", OutputId(params), "Opacity");
    params.Connect(typed, "Value", OutputId(params), "OpacityMask");
    const MaterialCode paramCode = GenerateMaterialCode(params);
    CHECK(HasError(paramCode, missing, "unknown parameter"));
    CHECK(HasError(paramCode, typed, "Vector parameter"));
    CHECK(!paramCode.Ok() && paramCode.glsl.empty());

    MaterialGraph stages = MakeDefaultMaterialGraph();
    const std::uint32_t depth = stages.AddNode("Input.PixelDepth", {0, 0});
    stages.Connect(depth, "Depth", OutputId(stages), "WorldPositionOffset");
    CHECK(HasError(GenerateMaterialCode(stages), depth, "world position offset"));
    stages.Connect(depth, "Depth", OutputId(stages), "Emissive"); // fine in the pixel stage
    stages.Disconnect(OutputId(stages), "WorldPositionOffset");
    CHECK(GenerateMaterialCode(stages).Ok());

    MaterialGraph unknown = MakeDefaultMaterialGraph();
    MaterialNode  bogus;
    bogus.id   = unknown.nextId++;
    bogus.type = "Math.Teleport";
    unknown.nodes.push_back(bogus);
    unknown.Disconnect(OutputId(unknown), "Metallic");
    unknown.links.push_back({bogus.id, "Result", OutputId(unknown), "Metallic"});
    CHECK(HasError(GenerateMaterialCode(unknown), bogus.id, "unknown node type"));

    // Component masks.
    MaterialGraph masks = MakeDefaultMaterialGraph();
    const std::uint32_t pos  = masks.AddNode("Input.WorldPosition", {0, 0});
    const std::uint32_t mask = masks.AddNode("Math.Mask", {0, 0}, "xz");
    masks.Connect(pos, "Position", mask, "X");
    masks.Connect(mask, "Result", OutputId(masks), "Emissive");
    const MaterialCode maskCode = GenerateMaterialCode(masks);
    CHECK(maskCode.Ok() && maskCode.glsl.find(").xz") != std::string::npos);
    masks.FindNode(mask)->param = "xw";
    CHECK(HasError(GenerateMaterialCode(masks), mask, "does not exist"));

    // Variants per blend mode.
    MaterialSettings s;
    CHECK(MaterialShaderVariants(s).size() == 5); // vertex, prepass, picking, lit, shadow vertex
    s.blendMode = MaterialBlendMode::Masked;
    CHECK(MaterialShaderVariants(s).size() == 6);
    s.castShadows = false;
    CHECK(MaterialShaderVariants(s).size() == 4);
    s.blendMode = MaterialBlendMode::Additive;
    CHECK(MaterialShaderVariants(s).size() == 2);
}

TEST_CASE(Material_EveryNodeCompiles)
{
    // Every palette node feeds the emissive sum (via Length: any width -> float); masked + WPO
    // + shadows cover all opaque templates, a translucent unlit copy the blend template.
    MaterialGraph graph = MakeDefaultMaterialGraph();
    MaterialParameter tex;
    tex.name = "Tex";
    tex.type = MaterialParameterType::Texture;
    graph.parameters.push_back(tex);
    graph.settings.blendMode       = MaterialBlendMode::Masked;
    graph.settings.maxDisplacement = 1.0f;
    std::uint32_t sum = 0;
    for (const MaterialNodeDesc& desc : MaterialNodeTypes()) {
        if (desc.hidden)
            continue;
        std::string param;
        if (desc.param == MaterialNodeParam::Parameter)
            param = desc.type == "Param.Scalar" ? "Roughness" : desc.type == "Param.Vector" ? "BaseColor" : "Tex";
        else if (desc.param == MaterialNodeParam::Mask)
            param = "x";
        const std::uint32_t node   = graph.AddNode(desc.type, {0, 0}, param);
        const std::uint32_t length = graph.AddNode("Math.Length", {0, 0});
        CHECK(graph.Connect(node, desc.outputs.front().name, length, "X"));
        if (sum == 0) {
            sum = length;
        } else {
            const std::uint32_t add = graph.AddNode("Math.Add", {0, 0});
            graph.Connect(sum, "Result", add, "A");
            graph.Connect(length, "Result", add, "B");
            sum = add;
        }
    }
    const std::uint32_t out = OutputId(graph);
    graph.Connect(sum, "Result", out, "Emissive");
    graph.Connect(sum, "Result", out, "OpacityMask");
    // World position offset from vertex-safe nodes (time + noise + a texture read at LOD 0).
    const std::uint32_t time  = graph.AddNode("Input.Time", {0, 0});
    const std::uint32_t noise = graph.AddNode("Utility.Noise", {0, 0});
    const std::uint32_t texel = graph.AddNode("Param.Texture", {0, 0}, "Tex");
    const std::uint32_t up    = graph.AddNode("Math.Multiply", {0, 0});
    graph.Connect(time, "Time", noise, "Scale");
    graph.Connect(noise, "Result", up, "A");
    graph.Connect(texel, "RGB", up, "B");
    graph.Connect(up, "Result", out, "WorldPositionOffset");
    const std::uint32_t normal = graph.AddNode("Utility.NormalStrength", {0, 0});
    graph.Connect(normal, "Normal", out, "Normal");

    const MaterialCode code = GenerateMaterialCode(graph);
    for (const MaterialProblem& p : code.problems)
        if (p.error)
            std::fprintf(stderr, "  node %u: %s\n", p.node, p.message.c_str());
    CHECK(code.Ok());
    CHECK(code.worldPositionOffset && code.normal);
    const std::string masked = CompileVariants(graph);
    if (!masked.empty())
        std::fprintf(stderr, "  %s\n", masked.c_str());
    CHECK(masked.empty());

    graph.settings.blendMode    = MaterialBlendMode::Translucent;
    graph.settings.shadingModel = MaterialShadingModel::Unlit;
    graph.settings.twoSided     = true;
    graph.Connect(sum, "Result", out, "Opacity");
    const std::string blend = CompileVariants(graph);
    if (!blend.empty())
        std::fprintf(stderr, "  %s\n", blend.c_str());
    CHECK(blend.empty());
    graph.settings.blendMode    = MaterialBlendMode::Additive;
    graph.settings.shadingModel = MaterialShadingModel::Lit;
    CHECK(CompileVariants(graph).empty());

    // A compile error comes back as a log, not a crash.
    ShaderCompileOptions options;
    options.virtualFiles.push_back({"material.generated.glsl", "this is not glsl"});
    options.defines = {{"CUSTOM_MATERIAL", "1"}, {"MATERIAL_BLEND_MODE", "0"}};
    const ShaderCompileResult broken = CompileEngineShader(ShaderStage::Fragment, "mesh.frag", options);
    CHECK(!broken.Ok() && !broken.log.empty());
    CHECK(ShaderSourceHash() != 0 && ShaderSourceHash() == ShaderSourceHash());
}

TEST_CASE(Material_OverridesFilesNodesAndTemplate)
{
    const fs::path dir = MaterialTempDir("material_overrides");
    fs::create_directories(dir / "Scenes");
    // Scene files: material paths relative to the scene, parameters kept; undo snapshots.
    {
        Scene        scene;
        const Entity e = scene.CreateEntity("Mesh");
        scene.GetRegistry().Emplace<MaterialOverrides>(
            e, MaterialOverrides{.materials  = {PathToUtf8(dir / "Materials" / "A.umat"), ""},
                                 .parameters = {{"Glow", glm::vec4(2.0f, 0.0f, 0.0f, 0.0f), -1},
                                                {"Tint", glm::vec4(1.0f, 0.5f, 0.0f, 1.0f), 1}}});
        const fs::path file = dir / "Scenes" / "S.scene.json";
        SaveSceneFile(file, scene, nullptr);
        std::ifstream     in(file);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("../Materials/A.umat") != std::string::npos);
        Scene loaded;
        CHECK(LoadSceneFile(file, loaded, nullptr).empty());
        bool found = false;
        loaded.GetRegistry().ViewOf<MaterialOverrides>().Each([&](Entity, const MaterialOverrides& m) {
            found = m == scene.GetRegistry().Get<MaterialOverrides>(e);
        });
        CHECK(found);
        const std::string before = SnapshotEntityState(scene, e);
        scene.GetRegistry().Remove<MaterialOverrides>(e);
        ApplyEntityState(scene, e, before);
        CHECK(scene.GetRegistry().Has<MaterialOverrides>(e));
    }

    // Blueprint nodes reach every mesh below the target.
    {
        using namespace BlueprintTest;
        Runner     r;
        Graph      b;
        const auto begin  = b.Node("Event.BeginPlay");
        const auto scalar = b.Node("Material.SetScalar");
        b.Set(scalar, "Name", std::string("Glow"));
        b.Set(scalar, "Value", 3.0f);
        const auto vector = b.Node("Material.SetVector");
        b.Set(vector, "Name", std::string("Tint"));
        b.Set(vector, "Color", glm::vec3(1.0f, 0.0f, 0.0f));
        b.Set(vector, "Submesh", std::int32_t{0});
        const auto set = b.Node("Material.Set");
        b.Set(set, "Material", PathToUtf8(dir / "Materials" / "B.uminst"));
        const auto wrong = b.Node("Material.Set");
        b.Set(wrong, "Material", std::string("Textures/x.png"));
        b.Link(begin, "Out", scalar, "In");
        b.Link(scalar, "Then", vector, "In");
        b.Link(vector, "Then", set, "In");
        b.Link(set, "Then", wrong, "In");
        CHECK(b.Valid());
        const Entity holder = r.Add("Holder", "materials.ugraph", b.g);
        const Entity mesh   = r.scene.CreateEntity("Mesh", holder);
        r.scene.GetRegistry().Emplace<MeshRenderer>(mesh, MeshRenderer{});
        r.scripts.Begin(r.scene);
        r.Run(0.1f);
        CHECK(r.scripts.Stats().errors >= 1); // the png is no material
        const MaterialOverrides* o = r.scene.GetRegistry().TryGet<MaterialOverrides>(mesh);
        CHECK(o != nullptr);
        if (o) {
            CHECK(o->parameters.size() == 2);
            CHECK(o->parameters[0].name == "Glow" && o->parameters[0].value.x == 3.0f && o->parameters[0].submesh == -1);
            CHECK(o->parameters[1].value == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) && o->parameters[1].submesh == 0);
            CHECK(o->materials.size() == 1 && o->materials[0].ends_with("B.uminst"));
        }
        CHECK(!r.scene.GetRegistry().Has<MaterialOverrides>(holder)); // no mesh: untouched

        Runner     c;
        Graph      g;
        const auto start = g.Node("Event.BeginPlay");
        const auto clear = g.Node("Material.ClearParameters");
        g.Link(start, "Out", clear, "In");
        const Entity owner = c.Add("Owner", "clear.ugraph", g.g);
        c.scene.GetRegistry().Emplace<MeshRenderer>(owner, MeshRenderer{});
        c.scene.GetRegistry().Emplace<MaterialOverrides>(owner, MaterialOverrides{.materials = {"x.umat"}, .parameters = {{"Glow", glm::vec4(1.0f), -1}}});
        c.scripts.Begin(c.scene);
        c.Run(0.1f);
        CHECK(c.scene.GetRegistry().Get<MaterialOverrides>(owner).parameters.empty());
        CHECK(c.scene.GetRegistry().Get<MaterialOverrides>(owner).materials.size() == 1);
    }

    // The Basic template's example material and instance.
    const fs::path materials = TemplateDirectory() / "Basic" / "Content" / "Materials";
    const auto     pulse     = LoadMaterialGraph(materials / "Pulse.umat");
    CHECK(pulse.has_value());
    if (pulse) {
        CHECK(GenerateMaterialCode(*pulse).Ok());
        CHECK(CompileVariants(*pulse).empty());
    }
    const auto warm = LoadMaterialInstance(materials / "Pulse_Warm.uminst");
    CHECK(warm && fs::exists(PathFromUtf8(warm->parent)));
    if (pulse && warm) {
        const std::vector<MaterialParameter> resolved = ResolveMaterialParameters(*pulse, warm->overrides);
        CHECK(std::any_of(resolved.begin(), resolved.end(), [](const MaterialParameter& p) {
            return p.name == "BaseColor" && p.value == glm::vec4(1.0f, 0.45f, 0.1f, 1.0f);
        }));
    }
}
