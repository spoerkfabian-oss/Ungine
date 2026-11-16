// Morph targets (CPU): glTF import, bounds, the optimizer's vertex reorder, cooked models.
#include "MorphTestUtil.h"
#include "Test.h"

#include "Engine/Assets/ContentCooker.h"
#include "Engine/Assets/GltfLoader.h"
#include "Engine/Assets/MeshOptimizer.h"

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
