#pragma once
#include "Engine/Assets/AssetHandle.h"
#include "Engine/Renderer/Vulkan/Image.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Engine {

// How a texture is sampled; decides its compressed format and mip filtering.
enum class TextureKind : std::uint8_t {
    Color,  // sRGB color (base color, emissive): BC7_SRGB
    Linear, // linear data (metallic-roughness, occlusion): BC7_UNORM
    Normal, // tangent-space normal map, XY only (Z is reconstructed): BC5_UNORM
    Count
};
[[nodiscard]] const char* ToString(TextureKind kind);

// CPU image with its whole mip chain, tightly packed (levels 16-byte aligned), level 0 first.
struct TextureImage {
    struct Level {
        std::uint64_t offset = 0;
        std::uint64_t size   = 0;
        std::uint32_t width  = 0;
        std::uint32_t height = 0;
    };
    VkFormat               format = VK_FORMAT_UNDEFINED;
    std::uint32_t          width  = 0;
    std::uint32_t          height = 0;
    std::vector<Level>     levels;
    std::vector<std::byte> data;
};

// GPU-resident texture asset. Materials reference it through the texture table (see
// AssetManager::TextureTable), so a reload can swap the image without touching them.
struct Texture {
    std::string   name;
    Image         image;
    std::uint32_t bindlessSlot = ~0u; // ~0u: none
    TextureKind   kind         = TextureKind::Color;
    VkFormat      format       = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0, height = 0, mipLevels = 0;
    std::uint64_t gpuBytes = 0;
    bool          cacheHit = false; // cooked data came from the texture cache
};

} // namespace Engine
