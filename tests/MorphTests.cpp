// Morph targets (CPU): glTF import, bounds, the optimizer's vertex reorder, cooked models.
#include "MorphTestUtil.h"
#include "Test.h"

#include "Engine/Assets/ContentCooker.h"
#include "Engine/Assets/GltfLoader.h"
#include "Engine/Animation/AnimGraph.h"
#include "Engine/Animation/AnimGraphPlayer.h"
#include "Engine/Assets/Animation.h"
#include "Engine/Assets/MeshOptimizer.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <stdexcept>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path WriteFace(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / std::format("ungine_{}_{}", name, std::random_device{}());
    fs::create_directories(dir);
    std::ofstream(dir / "Face.gltf") << MorphTest::FaceGltf();
    return dir / "Face.gltf";
}

// The position delta of target `t` for the vertex at `position` (the optimizer may reorder vertices).
glm::vec3 DeltaAt(const ModelData& d, std::uint32_t t, const glm::vec3& position)
{
    const MeshMorph& morph = d.meshes.at(0).morph;
    for (std::uint32_t v = 0; v < morph.vertexCount; ++v)
        if (glm::distance(d.vertices[morph.vertexBase + v].position, position) < 1e-6f)
            return glm::vec3(morph.deltas[(std::size_t{t} * morph.vertexCount + v) * 2]);
    return glm::vec3(-99.0f);
}

} // namespace

TEST_CASE(Morph_GltfImportOptimizeAndCook)
{
    const fs::path file = WriteFace("morph_import");
    ModelData      d    = LoadGltf(file);
    CHECK(d.meshes.size() == 1);
    const MeshMorph& morph = d.meshes[0].morph;
    CHECK((morph.targetNames == std::vector<std::string>{"Raise", "Push"}));
    CHECK((morph.defaultWeights == std::vector<float>{0.25f, 0.0f}));
    CHECK(morph.vertexBase == 0 && morph.vertexCount == 4 && morph.deltas.size() == 2 * 4 * 2);
    // Bounds grow by what the targets can add (weights in [0, 1]).
    const Submesh& sm = d.meshes[0].submeshes[0];
    CHECK(glm::distance(sm.boundsMax, glm::vec3(1.0f, 1.5f, 1.0f)) < 1e-5f && glm::distance(sm.boundsMin, glm::vec3(-1.0f, -1.0f, 0.0f)) < 1e-5f);
    CHECK(DeltaAt(d, 0, {1, 1, 0}) == glm::vec3(0.0f, 0.5f, 0.0f) && DeltaAt(d, 0, {1, -1, 0}) == glm::vec3(0.0f));
    CHECK(DeltaAt(d, 1, {-1, -1, 0}) == glm::vec3(0.0f, 0.0f, 1.0f));

    // The optimizer reorders the vertices: the deltas travel with them.
    (void)OptimizeMeshes(d, MeshOptimizeSettings{});
    CHECK(DeltaAt(d, 0, {1, 1, 0}) == glm::vec3(0.0f, 0.5f, 0.0f) && DeltaAt(d, 0, {-1, 1, 0}) == glm::vec3(0.0f, 0.5f, 0.0f));
    CHECK(DeltaAt(d, 0, {-1, -1, 0}) == glm::vec3(0.0f) && DeltaAt(d, 1, {1, -1, 0}) == glm::vec3(0.0f, 0.0f, 1.0f));

    // Cooked models keep them; inconsistent sizes are refused.
    const MeshOptimizeSettings settings;
    const ModelData cooked = DeserializeModelData(SerializeModelData(d, file, settings), file, settings);
    CHECK(cooked.meshes[0].morph.targetNames == d.meshes[0].morph.targetNames &&
          cooked.meshes[0].morph.deltas == d.meshes[0].morph.deltas &&
          cooked.meshes[0].morph.defaultWeights == d.meshes[0].morph.defaultWeights);
    ModelData broken = d;
    broken.meshes[0].morph.deltas.pop_back();
    bool refused = false;
    try {
        (void)DeserializeModelData(SerializeModelData(broken, file, settings), file, settings);
    } catch (const std::runtime_error& e) {
        refused = std::string(e.what()).find("morph") != std::string::npos;
    }
    CHECK(refused);
    std::error_code ec;
    fs::remove_all(file.parent_path(), ec);
}

namespace {

Model FaceModel(const fs::path& file)
{
    ModelData data = LoadGltf(file);
    Model     model;
    model.name       = data.name;
    model.nodes      = std::move(data.nodes);
    model.meshes     = std::move(data.meshes);
    model.animations = std::move(data.animations);
    BuildMorphPoseLayout(model);
    return model;
}

float Weight(const Model& model, std::span<const Transform> pose, std::uint32_t target)
{
    const auto slot = MorphPoseSlot(model, 0, target);
    return slot ? pose[slot->first].position[slot->second] : -1.0f;
}

} // namespace

// Weight channels: one track per target, sampled into the pose's morph entries, blended by the
// graph player (crossfades included) and written to MorphWeights only where clips drive them.
TEST_CASE(Morph_AnimationWeights)
{
    const fs::path file  = WriteFace("morph_anim");
    const Model    model = FaceModel(file);
    CHECK(model.animations.size() == 1 && model.animations[0].tracks.size() == 2);
    CHECK(model.animations[0].tracks[0].path == AnimationPath::Weights && model.animations[0].tracks[1].morphTarget == 1);
    CHECK(model.morphPoseFirst.size() == 1 && model.morphPoseWeights == 2 && AnimationPoseSize(model) == 2);

    std::vector<Transform> pose(AnimationPoseSize(model));
    RestPose(model, pose);
    CHECK(Weight(model, pose, 0) == 0.25f && Weight(model, pose, 1) == 0.0f); // the mesh's default weights
    EvaluateAnimation(model, model.animations[0], 0.5f, AnimationPlayback::Once, pose);
    CHECK(std::abs(Weight(model, pose, 0) - 0.5f) < 1e-5f && Weight(model, pose, 1) == 0.0f);
    EvaluateAnimation(model, model.animations[0], 1.0f, AnimationPlayback::Once, pose);
    CHECK(std::abs(Weight(model, pose, 0) - 1.0f) < 1e-5f);

    // Graph: Blink, then a 1 s crossfade to a state without a clip (rest pose: default weights).
    AnimGraph graph;
    graph.parameters = {{.name = "Rest", .type = AnimParamType::Trigger}};
    AnimLayer layer;
    layer.entryState = "Blink";
    layer.states.push_back(AnimState{.name = "Blink", .clip = "Blink", .loop = false});
    layer.states.push_back(AnimState{.name = "Rest", .clip = ""});
    layer.transitions.push_back(AnimTransition{.from = "Blink", .to = "Rest", .duration = 1.0f,
                                               .conditions = {{.parameter = "Rest", .op = AnimCompare::Triggered}}});
    graph.layers.push_back(layer);
    AnimGraphPlayer player;
    player.Bind(std::make_shared<const AnimGraph>(graph), &model);
    player.Update(1.0f, pose); // Blink at 1 s: Raise = 1
    CHECK(std::abs(Weight(model, pose, 0) - 1.0f) < 1e-4f);
    CHECK(player.DrivenNodes().size() == pose.size() && player.DrivenNodes()[1] == 1 && player.DrivenNodes()[0] == 0);
    CHECK(player.SetTrigger("Rest"));
    player.Update(0.5f, pose); // halfway: Blink at 1.5 s (0.5) blended with rest (0.25) -> 0.375
    CHECK(std::abs(Weight(model, pose, 0) - 0.375f) < 1e-3f);

    // Into the component of the mesh entity; undriven entries leave it alone.
    Scene        scene;
    const Entity face = scene.CreateEntity("Face");
    const std::vector<Entity>       nodes{face};
    const std::vector<std::uint8_t> none(pose.size(), 0);
    ApplyMorphPose(scene.GetRegistry(), model, pose, nodes, none);
    CHECK(!scene.GetRegistry().Has<MorphWeights>(face));
    ApplyMorphPose(scene.GetRegistry(), model, pose, nodes, player.DrivenNodes());
    const MorphWeights* weights = scene.GetRegistry().TryGet<MorphWeights>(face);
    CHECK(weights && weights->weights.size() == 2 && std::abs(weights->weights[0] - 0.375f) < 1e-3f);
    std::error_code ec;
    fs::remove_all(file.parent_path(), ec);
}
