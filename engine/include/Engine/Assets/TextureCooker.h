#pragma once
#include "Engine/Assets/Texture.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Engine {

struct TextureCacheStats {
    std::uint64_t bytes   = 0; // left in the cache
    std::uint32_t files   = 0;
    std::uint32_t removed = 0;
};

struct TextureCookSettings {
    // BC7 (color / linear) and BC5 (normals). Off, or without device support: RGBA8.
    bool compress = true;
    // Cooked KTX2 files, named by a hash of the source bytes + kind + settings; empty: no cache.
    std::filesystem::path cacheDirectory;
    // Encoder effort: 0 = fastest. BC7 partition search grows with it.
    std::uint32_t quality = 0;
    // PruneTextureCache limit applied by the AssetManager at startup (least recently used first).
    std::uint64_t cacheLimitBytes = 4ull << 30;
};

struct CookResult {
    std::shared_ptr<const TextureImage> image;
    bool                                cacheHit = false; // from the cache directory or a pak
    bool                                packed   = false; // cooked data of a mounted pak
    bool                                encoded  = false; // decoded + mipmapped + compressed just now
};

// Identifies the cooked result of `source` for `kind` and the settings that shape it (not the
// cache directory): 16 hex digits. Cache files are "<key>.ktx2"; paks hold ":cooked/textures/<key>.ktx2".
[[nodiscard]] std::string TextureCookKey(std::span<const std::byte> source, TextureKind kind,
                                         const TextureCookSettings& settings);

// PNG / JPEG / KTX2 bytes -> GPU-ready image with mips. Looks into the mounted paks' cooked
// textures and the cache directory first.
// Encoding: CPU box filter, linear-space for sRGB,
// renormalized for normal maps) in the kind's format. KTX2 input is used as it is (decompressed
// to RGBA8 when compression is off). Thread-safe; throws std::runtime_error on bad input.
[[nodiscard]] CookResult CookTexture(std::span<const std::byte> source, TextureKind kind,
                                     const TextureCookSettings& settings);

// Deletes the least recently used cooked files (cache hits refresh the timestamp) until the
// directory holds at most `maxBytes`; 0 clears it.
TextureCacheStats PruneTextureCache(const std::filesystem::path& directory, std::uint64_t maxBytes);

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
