// Malformed and crafted input files (CPU): loaders must reject them without huge allocations,
// out-of-bounds reads or invalid geometry reaching later stages.
#include "AllocationProbe.h"
#include "Test.h"

#include "Engine/Assets/ContentCooker.h"
#include "Engine/Assets/TextureCooker.h"
#include "Engine/Terrain/TerrainData.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <new>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Engine;
namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / std::format("ungine_{}_{}", name, std::random_device{}());
    fs::create_directories(dir);
    return dir;
}

template <class T>
void Put(std::vector<char>& out, const T& value)
{
    const char* p = reinterpret_cast<const char*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

void WriteFile(const fs::path& file, const std::vector<char>& bytes)
{
    std::ofstream(file, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

constexpr std::size_t kSmallLimit = 1u << 20; // no loader may ask for more than 1 MB for these tiny files

} // namespace

// A terrain header claiming a ~4 GB JSON block in a 20-byte file is rejected before allocating.
TEST_CASE(Malformed_TerrainJsonSize)
{
    const fs::path   dir = TempDir("bad_terrain");
    std::vector<char> bytes{'U', 'T', 'R', 'N'};
    Put(bytes, std::uint32_t{1});
    Put(bytes, std::uint32_t{0xFFFFFFF0u});
    bytes.insert(bytes.end(), {'{', '}', ' ', ' ', ' ', ' ', ' ', ' '});
    WriteFile(dir / "Bad.uterrain", bytes);

    std::string error;
    bool        loaded = true;
    AllocationProbe::Begin(kSmallLimit);
    try {
        loaded = LoadTerrain(dir / "Bad.uterrain", &error).has_value();
    } catch (const std::bad_alloc&) {
    }
    AllocationProbe::End();
    CHECK(!AllocationProbe::Exceeded());
    CHECK(!loaded && error.find("truncated") != std::string::npos);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

namespace {

std::vector<std::byte> TinyKtx2(VkFormat format, std::size_t levelBytes)
{
    TextureImage image;
    image.format = format;
    image.width  = 4;
    image.height = 4;
    image.data.assign(levelBytes, std::byte{0x7f});
    image.levels.push_back({.offset = 0, .size = levelBytes, .width = 4, .height = 4});
    return WriteKtx2(image);
}

template <class T>
void Patch(std::vector<std::byte>& bytes, std::size_t offset, T value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

bool RejectsKtx2(std::span<const std::byte> bytes)
{
    try {
        (void)ReadKtx2(bytes);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

// Level ranges whose offset + size wraps around 2^64 and widths whose block math wraps in 32 bits
// are rejected instead of copying from outside the file or describing a 4-billion-texel level.
TEST_CASE(Malformed_Ktx2LevelRange)
{
    const std::vector<std::byte> valid = TinyKtx2(VK_FORMAT_R8G8B8A8_UNORM, 64);
    CHECK(!RejectsKtx2(valid));

    std::vector<std::byte> wrapped = valid;
    Patch(wrapped, 80, std::uint64_t{0xFFFFFFFFFFFFFFF0ull}); // level 0 offset; size stays 64
    CHECK(RejectsKtx2(wrapped));

    std::vector<std::byte> past = valid;
    Patch(past, 80, std::uint64_t{valid.size() - 8}); // starts inside, ends past the end
    CHECK(RejectsKtx2(past));

    std::vector<std::byte> wide = TinyKtx2(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8);
    CHECK(!RejectsKtx2(wide));
    Patch(wide, 20, std::uint32_t{0xFFFFFFFEu}); // (width + 3) / 4 == 0 in 32-bit math
    Patch(wide, 88, std::uint64_t{0});           // level size that matches the wrapped math
    CHECK(RejectsKtx2(wide));
    CHECK(LevelBytes(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0xFFFFFFFEu, 4) == std::uint64_t{0x40000000} * 8);
}

// Record counts in a cooked model are checked against the smallest serialized record, so a count
// of "one per remaining byte" no longer turns 64 KB of file into megabytes of empty records.
TEST_CASE(Malformed_CookedModelCounts)
{
    const fs::path             file = fs::temp_directory_path() / "Model.glb";
    const MeshOptimizeSettings settings;
    std::vector<std::byte>     bytes = SerializeModelData(ModelData{}, file, settings);
    CHECK(DeserializeModelData(bytes, file, settings).optimized);

    // An empty model ends with: textures, materials, meshes, nodes, skin influences, skins and
    // animations counts (7 x 8 B), bounds (24 B) and the dependency count (8 B).
    const std::size_t texturesAt = bytes.size() - 88;
    bytes.resize(bytes.size() + 65536, std::byte{0});
    Patch(bytes, texturesAt, std::uint64_t{bytes.size() - texturesAt - 8});

    for (const std::size_t countAt : {texturesAt, texturesAt + 8, texturesAt + 16, texturesAt + 24}) {
        std::vector<std::byte> crafted = bytes;
        if (countAt != texturesAt) {
            Patch(crafted, texturesAt, std::uint64_t{0});
            Patch(crafted, countAt, std::uint64_t{crafted.size() - countAt - 8});
        }
        bool rejected = false;
        AllocationProbe::Begin(kSmallLimit);
        try {
            (void)DeserializeModelData(crafted, file, settings);
        } catch (const std::runtime_error&) {
            rejected = true;
        } catch (const std::bad_alloc&) {
        }
        AllocationProbe::End();
        CHECK(!AllocationProbe::Exceeded());
        CHECK(rejected);
    }
}

namespace {

ModelData OneTriangle(std::uint32_t lastIndex)
{
    ModelData d;
    d.name     = "Tri";
    d.vertices.resize(3);
    d.vertices[1].position = {1.0f, 0.0f, 0.0f};
    d.vertices[2].position = {0.0f, 1.0f, 0.0f};
    d.indices  = {0, 1, lastIndex};
    d.meshes.push_back(Mesh{.name = "Tri", .submeshes = {Submesh{.firstIndex = 0, .indexCount = 3}}});
    return d;
}

} // namespace

// Index values that address no vertex are rejected: in cooked models (no glTF validation on that
// path) and in OptimizeMeshes, where the highest index 0xFFFFFFFF used to wrap the count to 0.
TEST_CASE(Malformed_IndicesPastTheVertices)
{
    const fs::path             file = fs::temp_directory_path() / "Tri.glb";
    const MeshOptimizeSettings settings;
    const auto deserializes = [&](const ModelData& d) {
        try {
            return DeserializeModelData(SerializeModelData(d, file, settings), file, settings).indices.size() == 3;
        } catch (const std::runtime_error&) {
            return false;
        }
    };
    CHECK(deserializes(OneTriangle(2)));
    CHECK(!deserializes(OneTriangle(3)));
    CHECK(!deserializes(OneTriangle(0xFFFFFFFFu)));
    ModelData negative = OneTriangle(2);
    negative.meshes[0].submeshes[0].vertexOffset = -1;
    CHECK(!deserializes(negative));
    ModelData lod = OneTriangle(2);
    lod.indices.push_back(7);
    lod.meshes[0].submeshes[0].lodCount = 2;
    lod.meshes[0].submeshes[0].lods[1]  = {.firstIndex = 3, .indexCount = 1};
    CHECK(!deserializes(lod));

    for (const std::uint32_t last : {3u, 0xFFFFFFFFu}) {
        ModelData d        = OneTriangle(last);
        bool      rejected = false;
        try {
            (void)OptimizeMeshes(d, settings);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
    }
    ModelData valid = OneTriangle(2);
    (void)OptimizeMeshes(valid, settings);
    CHECK(valid.indices.size() >= 3);
}
