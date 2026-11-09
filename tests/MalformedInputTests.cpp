// Malformed and crafted input files (CPU): loaders must reject them without huge allocations,
// out-of-bounds reads or invalid geometry reaching later stages.
#include "AllocationProbe.h"
#include "Test.h"

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
