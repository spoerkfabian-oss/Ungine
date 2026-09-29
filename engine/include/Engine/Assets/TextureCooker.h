#pragma once
#include "Engine/Assets/Texture.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace Engine {

struct TextureCookSettings {
    // BC7 (color / linear) and BC5 (normals). Off, or without device support: RGBA8.
    bool compress = true;
    // Cooked KTX2 files, named by a hash of the source bytes + kind + settings; empty: no cache.
    std::filesystem::path cacheDirectory;
    // Encoder effort: 0 = fastest. BC7 partition search grows with it.
    std::uint32_t quality = 0;
};

struct CookResult {
    std::shared_ptr<const TextureImage> image;
    bool                                cacheHit = false;
};

// PNG / JPEG / KTX2 bytes -> GPU-ready image with mips (CPU box filter, linear-space for sRGB,
// renormalized for normal maps) in the kind's format. KTX2 input is used as it is (decompressed
// to RGBA8 when compression is off). Thread-safe; throws std::runtime_error on bad input.
[[nodiscard]] CookResult CookTexture(std::span<const std::byte> source, TextureKind kind,
                                     const TextureCookSettings& settings);

// KTX 2.0 container without supercompression: RGBA8, BC1, BC3, BC4, BC5, BC7 (UNORM / SRGB).
[[nodiscard]] bool                   IsKtx2(std::span<const std::byte> bytes);
[[nodiscard]] TextureImage           ReadKtx2(std::span<const std::byte> bytes); // throws
[[nodiscard]] std::vector<std::byte> WriteKtx2(const TextureImage& image);

// RGBA8 copy of a (possibly block-compressed) level, e.g. to inspect cooked data.
[[nodiscard]] std::vector<std::uint8_t> DecodeLevel(const TextureImage& image, std::size_t level);

[[nodiscard]] const char*   FormatName(VkFormat format); // "BC7 sRGB", ...; "?" for others
[[nodiscard]] bool          IsBlockCompressed(VkFormat format);
[[nodiscard]] std::uint32_t BlockBytes(VkFormat format); // bytes per 4x4 block (RGBA8: per texel)
[[nodiscard]] std::uint64_t LevelBytes(VkFormat format, std::uint32_t width, std::uint32_t height);

} // namespace Engine
