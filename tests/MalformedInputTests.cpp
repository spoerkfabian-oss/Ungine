// Malformed and crafted input files (CPU): loaders must reject them without huge allocations,
// out-of-bounds reads or invalid geometry reaching later stages.
#include "AllocationProbe.h"
#include "Test.h"

#include "Engine/Terrain/TerrainData.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <new>
#include <random>
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
