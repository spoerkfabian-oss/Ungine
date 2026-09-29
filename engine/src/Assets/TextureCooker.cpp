#include "Engine/Assets/TextureCooker.h"
#include "Engine/Core/Log.h"

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace Engine {

namespace {

constexpr std::uint32_t kCookerVersion = 1; // bump when the cooked output changes

constexpr std::array<std::uint8_t, 12> kKtx2Id{0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

// Data format descriptor color models (KHR_DF_MODEL_*).
constexpr std::uint8_t kModelRgbsda = 1, kModelBc1 = 128, kModelBc3 = 130, kModelBc4 = 131, kModelBc5 = 132,
                       kModelBc7 = 134;

struct FormatInfo {
    VkFormat      format;
    std::uint32_t blockBytes; // per 4x4 block, or per texel for RGBA8
    bool          compressed;
    bool          srgb;
    std::uint8_t  colorModel;
    const char*   name;
};

constexpr std::array<FormatInfo, 10> kFormats{{
    {VK_FORMAT_R8G8B8A8_UNORM, 4, false, false, kModelRgbsda, "RGBA8"},
    {VK_FORMAT_R8G8B8A8_SRGB, 4, false, true, kModelRgbsda, "RGBA8 sRGB"},
    {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, true, false, kModelBc1, "BC1"},
    {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, true, true, kModelBc1, "BC1 sRGB"},
    {VK_FORMAT_BC3_UNORM_BLOCK, 16, true, false, kModelBc3, "BC3"},
    {VK_FORMAT_BC3_SRGB_BLOCK, 16, true, true, kModelBc3, "BC3 sRGB"},
    {VK_FORMAT_BC4_UNORM_BLOCK, 8, true, false, kModelBc4, "BC4"},
    {VK_FORMAT_BC5_UNORM_BLOCK, 16, true, false, kModelBc5, "BC5"},
    {VK_FORMAT_BC7_UNORM_BLOCK, 16, true, false, kModelBc7, "BC7"},
    {VK_FORMAT_BC7_SRGB_BLOCK, 16, true, true, kModelBc7, "BC7 sRGB"},
}};

const FormatInfo* Info(VkFormat format)
{
    for (const FormatInfo& f : kFormats)
        if (f.format == format)
            return &f;
    return nullptr;
}

const FormatInfo& RequireInfo(VkFormat format)
{
    const FormatInfo* info = Info(format);
    if (!info)
        throw std::runtime_error(std::format("unsupported texture format {}", static_cast<int>(format)));
    return *info;
}

std::uint64_t Align(std::uint64_t value, std::uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

// --- Little-endian byte streams -----------------------------------------------------------------

struct Writer {
    std::vector<std::byte> bytes;
    template <class T>
    void Put(T value)
    {
        const auto* p = reinterpret_cast<const std::byte*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(T)); // all supported targets are little-endian
    }
    void Pad(std::size_t alignment) { bytes.resize(Align(bytes.size(), alignment)); }
};

template <class T>
T Get(std::span<const std::byte> bytes, std::size_t offset)
{
    if (offset + sizeof(T) > bytes.size())
        throw std::runtime_error("KTX2: truncated file");
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

// --- Hashing / cache ------------------------------------------------------------------------------

std::uint64_t Fnv1a(std::span<const std::byte> bytes, std::uint64_t hash = 0xcbf29ce484222325ull)
{
    for (std::byte b : bytes) {
        hash ^= static_cast<std::uint64_t>(b);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

std::vector<std::byte> ReadWholeFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return {};
    const std::streamsize size = file.tellg();
    std::vector<std::byte> bytes(static_cast<std::size_t>(std::max<std::streamsize>(size, 0)));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return file ? bytes : std::vector<std::byte>{};
}

void WriteCacheFile(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // Unique temporary name: concurrent cooks of the same source may race, the rename is atomic.
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    std::filesystem::path tmp = path;
    tmp += std::format(".{:x}.tmp", tid);
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            ENGINE_WARN("Texture cache: cannot write '{}'", tmp.string());
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        ENGINE_WARN("Texture cache: cannot store '{}'", path.string());
    }
}

// --- Pixel processing -----------------------------------------------------------------------------

struct Rgba8 {
    std::vector<std::uint8_t> pixels;
    std::uint32_t             width = 0, height = 0;
};

float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float LinearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
std::uint8_t ToByte(float v) { return static_cast<std::uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); }

// 2x2 box filter (the last odd row / column folds into its neighbor).
Rgba8 Downsample(const Rgba8& src, TextureKind kind)
{
    static const std::array<float, 256> toLinear = [] {
        std::array<float, 256> table{};
        for (int i = 0; i < 256; ++i)
            table[static_cast<std::size_t>(i)] = SrgbToLinear(static_cast<float>(i) / 255.0f);
        return table;
    }();
    Rgba8 dst{.pixels = {}, .width = std::max(src.width / 2, 1u), .height = std::max(src.height / 2, 1u)};
    dst.pixels.resize(std::size_t{dst.width} * dst.height * 4);
    for (std::uint32_t y = 0; y < dst.height; ++y) {
        for (std::uint32_t x = 0; x < dst.width; ++x) {
            float sum[4] = {0, 0, 0, 0};
            for (std::uint32_t dy = 0; dy < 2; ++dy)
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::uint32_t sx = std::min(x * 2 + dx, src.width - 1);
                    const std::uint32_t sy = std::min(y * 2 + dy, src.height - 1);
                    const std::uint8_t* p  = &src.pixels[(std::size_t{sy} * src.width + sx) * 4];
                    for (int c = 0; c < 4; ++c) {
                        const float v = static_cast<float>(p[c]) / 255.0f;
                        sum[c] += (kind == TextureKind::Color && c < 3) ? toLinear[p[c]]
                                  : (kind == TextureKind::Normal && c < 3) ? v * 2.0f - 1.0f
                                                                           : v;
                    }
                }
            std::uint8_t* out = &dst.pixels[(std::size_t{y} * dst.width + x) * 4];
            if (kind == TextureKind::Normal) {
                const float len = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                for (int c = 0; c < 3; ++c)
                    out[c] = ToByte(len > 0.0f ? sum[c] / len * 0.5f + 0.5f : 0.5f);
            } else {
                for (int c = 0; c < 3; ++c)
                    out[c] = ToByte(kind == TextureKind::Color ? LinearToSrgb(sum[c] * 0.25f) : sum[c] * 0.25f);
            }
            out[3] = ToByte(sum[3] * 0.25f);
        }
    }
    return dst;
}

// Runs fn(i) for i in [0, count) on a few threads (large images only).
void ParallelFor(std::uint32_t count, const std::function<void(std::uint32_t)>& fn)
{
    const std::uint32_t threads = count >= 64 ? std::clamp(std::thread::hardware_concurrency(), 1u, 8u) : 1u;
    if (threads <= 1) {
        for (std::uint32_t i = 0; i < count; ++i)
            fn(i);
        return;
    }
    std::atomic<std::uint32_t> next{0};
    std::vector<std::jthread>  workers;
    for (std::uint32_t t = 0; t < threads; ++t)
        workers.emplace_back([&] {
            for (std::uint32_t i; (i = next.fetch_add(1)) < count;)
                fn(i);
        });
}

void InitEncoders()
{
    static std::once_flag once;
    std::call_once(once, [] {
        bc7enc_compress_block_init();
        rgbcx::init();
    });
}

// One level into BC7 or BC5 blocks (edge texels repeated for partial blocks).
void EncodeLevel(const Rgba8& level, VkFormat format, TextureKind kind, std::uint32_t quality, std::byte* out)
{
    const std::uint32_t bw = (level.width + 3) / 4, bh = (level.height + 3) / 4;
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    if (kind != TextureKind::Color)
        bc7enc_compress_block_params_init_linear_weights(&params);
    params.m_uber_level     = std::min(quality, static_cast<std::uint32_t>(BC7ENC_MAX_UBER_LEVEL));
    params.m_max_partitions = quality == 0 ? 16u : BC7ENC_MAX_PARTITIONS;
    const std::uint32_t blockBytes = RequireInfo(format).blockBytes;

    ParallelFor(bh, [&](std::uint32_t by) {
        std::uint8_t block[16 * 4];
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            for (std::uint32_t y = 0; y < 4; ++y)
                for (std::uint32_t x = 0; x < 4; ++x) {
                    const std::uint32_t sx = std::min(bx * 4 + x, level.width - 1);
                    const std::uint32_t sy = std::min(by * 4 + y, level.height - 1);
                    std::memcpy(&block[(y * 4 + x) * 4], &level.pixels[(std::size_t{sy} * level.width + sx) * 4], 4);
                }
            std::byte* dst = out + (std::size_t{by} * bw + bx) * blockBytes;
            if (format == VK_FORMAT_BC5_UNORM_BLOCK)
                rgbcx::encode_bc5(dst, block, 0, 1, 4);
            else
                bc7enc_compress_block(dst, block, &params);
        }
    });
}

TextureImage Pack(std::vector<Rgba8> levels, VkFormat format, TextureKind kind, std::uint32_t quality)
{
    TextureImage image;
    image.format = format;
    image.width  = levels.front().width;
    image.height = levels.front().height;
    for (const Rgba8& l : levels) {
        TextureImage::Level level{.offset = Align(image.data.size(), 16),
                                  .size   = LevelBytes(format, l.width, l.height),
                                  .width  = l.width,
                                  .height = l.height};
        image.data.resize(level.offset + level.size);
        if (IsBlockCompressed(format))
            EncodeLevel(l, format, kind, quality, image.data.data() + level.offset);
        else
            std::memcpy(image.data.data() + level.offset, l.pixels.data(), l.pixels.size());
        image.levels.push_back(level);
    }
    return image;
}

// KTX2 input kept as it is, or decompressed when compression is off.
TextureImage FromKtx2(std::span<const std::byte> source, TextureKind kind, bool compress)
{
    TextureImage image = ReadKtx2(source);
    if (compress || !IsBlockCompressed(image.format))
        return image;
    std::vector<Rgba8> levels;
    for (std::size_t i = 0; i < image.levels.size(); ++i)
        levels.push_back({DecodeLevel(image, i), image.levels[i].width, image.levels[i].height});
    const bool srgb = RequireInfo(image.format).srgb || kind == TextureKind::Color;
    return Pack(std::move(levels), srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, kind, 0);
}

} // namespace

const char* FormatName(VkFormat format)
{
    const FormatInfo* info = Info(format);
    return info ? info->name : "?";
}

const char* ToString(TextureKind kind)
{
    switch (kind) {
    case TextureKind::Color: return "color";
    case TextureKind::Linear: return "linear";
    case TextureKind::Normal: return "normal";
    default: return "?";
    }
}

bool IsBlockCompressed(VkFormat format)
{
    const FormatInfo* info = Info(format);
    return info && info->compressed;
}

std::uint32_t BlockBytes(VkFormat format)
{
    return RequireInfo(format).blockBytes;
}

std::uint64_t LevelBytes(VkFormat format, std::uint32_t width, std::uint32_t height)
{
    const FormatInfo& info = RequireInfo(format);
    if (!info.compressed)
        return std::uint64_t{width} * height * info.blockBytes;
    return std::uint64_t{(width + 3) / 4} * ((height + 3) / 4) * info.blockBytes;
}

CookResult CookTexture(std::span<const std::byte> source, TextureKind kind, const TextureCookSettings& settings)
{
    if (source.empty())
        throw std::runtime_error("empty texture source");
    if (IsKtx2(source))
        return {std::make_shared<TextureImage>(FromKtx2(source, kind, settings.compress)), false};

    // Cache: keyed by the source bytes and everything that shapes the output.
    std::filesystem::path cacheFile;
    if (!settings.cacheDirectory.empty()) {
        std::uint64_t hash = Fnv1a(source);
        const std::uint32_t salt[4] = {kCookerVersion, static_cast<std::uint32_t>(kind), settings.compress ? 1u : 0u,
                                       settings.quality};
        hash      = Fnv1a(std::as_bytes(std::span{salt}), hash);
        cacheFile = settings.cacheDirectory / std::format("{:016x}.ktx2", hash);
        if (const std::vector<std::byte> cached = ReadWholeFile(cacheFile); !cached.empty()) {
            try {
                return {std::make_shared<TextureImage>(ReadKtx2(cached)), true};
            } catch (const std::exception& e) {
                ENGINE_WARN("Texture cache: ignoring '{}': {}", cacheFile.string(), e.what());
            }
        }
    }

    int      w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(source.data()),
                                            static_cast<int>(source.size()), &w, &h, &comp, STBI_rgb_alpha);
    if (!pixels)
        throw std::runtime_error(std::format("cannot decode image: {}", stbi_failure_reason()));
    std::vector<Rgba8> levels(1);
    levels[0].width  = static_cast<std::uint32_t>(w);
    levels[0].height = static_cast<std::uint32_t>(h);
    levels[0].pixels.assign(pixels, pixels + std::size_t{levels[0].width} * levels[0].height * 4);
    stbi_image_free(pixels);
    while (levels.back().width > 1 || levels.back().height > 1)
        levels.push_back(Downsample(levels.back(), kind));

    VkFormat format = kind == TextureKind::Color ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (settings.compress) {
        InitEncoders();
        format = kind == TextureKind::Normal ? VK_FORMAT_BC5_UNORM_BLOCK
                 : kind == TextureKind::Color ? VK_FORMAT_BC7_SRGB_BLOCK
                                              : VK_FORMAT_BC7_UNORM_BLOCK;
    }
    auto image = std::make_shared<TextureImage>(Pack(std::move(levels), format, kind, settings.quality));
    if (!cacheFile.empty())
        WriteCacheFile(cacheFile, WriteKtx2(*image));
    return {std::move(image), false};
}

// --- KTX2 ---------------------------------------------------------------------------------------

bool IsKtx2(std::span<const std::byte> bytes)
{
    return bytes.size() >= kKtx2Id.size() && std::memcmp(bytes.data(), kKtx2Id.data(), kKtx2Id.size()) == 0;
}

std::vector<std::byte> WriteKtx2(const TextureImage& image)
{
    const FormatInfo& info   = RequireInfo(image.format);
    const auto        levels = static_cast<std::uint32_t>(image.levels.size());

    // Data format descriptor: one basic block (Khronos Data Format 1.3, section 5.6).
    Writer dfd;
    const std::uint32_t samples = info.colorModel == kModelRgbsda ? 4 : (info.colorModel == kModelBc5 ? 2 : 1);
    dfd.Put<std::uint32_t>(0);                                                   // total size, patched below
    dfd.Put<std::uint32_t>(0);                                                   // vendor 0, descriptor type 0
    dfd.Put<std::uint16_t>(2);                                                   // version
    dfd.Put<std::uint16_t>(static_cast<std::uint16_t>(24 + 16 * samples));       // block size
    dfd.Put<std::uint8_t>(info.colorModel);
    dfd.Put<std::uint8_t>(1);                                                    // primaries: BT.709
    dfd.Put<std::uint8_t>(info.srgb ? 2 : 1);                                    // transfer: sRGB / linear
    dfd.Put<std::uint8_t>(0);                                                    // flags: straight alpha
    const std::uint8_t dim = info.compressed ? 3 : 0;
    for (std::uint8_t d : {dim, dim, std::uint8_t{0}, std::uint8_t{0}})          // block dimensions - 1
        dfd.Put<std::uint8_t>(d);
    for (int i = 0; i < 8; ++i)                                                  // bytes per plane
        dfd.Put<std::uint8_t>(i == 0 ? static_cast<std::uint8_t>(info.compressed ? info.blockBytes : 4) : 0);
    for (std::uint32_t s = 0; s < samples; ++s) {
        const bool rgba = info.colorModel == kModelRgbsda;
        const std::uint32_t bits = rgba ? 8 : (samples == 2 ? 64 : info.blockBytes * 8);
        dfd.Put<std::uint16_t>(static_cast<std::uint16_t>(s * bits));          // bit offset
        dfd.Put<std::uint8_t>(static_cast<std::uint8_t>(bits - 1));             // bit length - 1
        dfd.Put<std::uint8_t>(static_cast<std::uint8_t>(rgba && s == 3 ? 15 : s)); // channel (alpha = 15)
        dfd.Put<std::uint32_t>(0);                                               // sample position
        dfd.Put<std::uint32_t>(0);                                               // lower
        dfd.Put<std::uint32_t>(rgba ? 255u : 0xFFFFFFFFu);                       // upper
    }
    const auto dfdSize = static_cast<std::uint32_t>(dfd.bytes.size());
    std::memcpy(dfd.bytes.data(), &dfdSize, sizeof(dfdSize));

    Writer out;
    for (std::uint8_t b : kKtx2Id)
        out.Put(b);
    out.Put<std::uint32_t>(static_cast<std::uint32_t>(image.format));
    out.Put<std::uint32_t>(1);            // typeSize
    out.Put<std::uint32_t>(image.width);
    out.Put<std::uint32_t>(image.height);
    out.Put<std::uint32_t>(0);            // depth
    out.Put<std::uint32_t>(0);            // layers (not an array)
    out.Put<std::uint32_t>(1);            // faces
    out.Put<std::uint32_t>(levels);
    out.Put<std::uint32_t>(0);            // no supercompression
    const std::size_t indexEnd = 80 + std::size_t{levels} * 24;
    out.Put<std::uint32_t>(static_cast<std::uint32_t>(indexEnd)); // dfd offset
    out.Put<std::uint32_t>(dfdSize);
    out.Put<std::uint32_t>(0);            // no key/value data
    out.Put<std::uint32_t>(0);
    out.Put<std::uint64_t>(0);            // no supercompression global data
    out.Put<std::uint64_t>(0);

    // Level data follows the DFD, smallest level first (as the specification requires).
    // mipPadding: each level starts at the next multiple of lcm(texel block size, 4).
    const std::uint64_t        alignment = std::lcm(std::uint64_t{info.blockBytes}, std::uint64_t{4});
    std::vector<std::uint64_t> offsets(levels);
    std::uint64_t              cursor = indexEnd + dfdSize;
    for (std::uint32_t i = levels; i-- > 0;) {
        cursor     = Align(cursor, alignment);
        offsets[i] = cursor;
        cursor += image.levels[i].size;
    }
    for (std::uint32_t i = 0; i < levels; ++i) {
        out.Put<std::uint64_t>(offsets[i]);
        out.Put<std::uint64_t>(image.levels[i].size);
        out.Put<std::uint64_t>(image.levels[i].size);
    }
    out.bytes.insert(out.bytes.end(), dfd.bytes.begin(), dfd.bytes.end());
    for (std::uint32_t i = levels; i-- > 0;) {
        out.bytes.resize(offsets[i]);
        const std::byte* src = image.data.data() + image.levels[i].offset;
        out.bytes.insert(out.bytes.end(), src, src + image.levels[i].size);
    }
    return std::move(out.bytes);
}

TextureImage ReadKtx2(std::span<const std::byte> bytes)
{
    if (!IsKtx2(bytes))
        throw std::runtime_error("not a KTX2 file");
    const auto format      = static_cast<VkFormat>(Get<std::uint32_t>(bytes, 12));
    const auto width       = Get<std::uint32_t>(bytes, 20);
    const auto height      = Get<std::uint32_t>(bytes, 24);
    const auto depth       = Get<std::uint32_t>(bytes, 28);
    const auto layers      = Get<std::uint32_t>(bytes, 32);
    const auto faces       = Get<std::uint32_t>(bytes, 36);
    const auto levelCount  = std::max(Get<std::uint32_t>(bytes, 40), 1u);
    const auto supercomp   = Get<std::uint32_t>(bytes, 44);
    if (format == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("KTX2: Basis Universal payloads (VK_FORMAT_UNDEFINED) are not supported");
    if (!Info(format))
        throw std::runtime_error(std::format("KTX2: unsupported format {}", static_cast<int>(format)));
    if (supercomp != 0)
        throw std::runtime_error("KTX2: supercompression (zstd / BasisLZ) is not supported");
    if (depth > 1 || layers > 1 || faces != 1 || width == 0 || height == 0)
        throw std::runtime_error("KTX2: only single 2D images are supported");
    if (levelCount > 16)
        throw std::runtime_error("KTX2: too many levels");

    TextureImage image;
    image.format = format;
    image.width  = width;
    image.height = height;
    for (std::uint32_t i = 0; i < levelCount; ++i) {
        const auto offset = Get<std::uint64_t>(bytes, 80 + std::size_t{i} * 24);
        const auto size   = Get<std::uint64_t>(bytes, 80 + std::size_t{i} * 24 + 8);
        const std::uint32_t w = std::max(width >> i, 1u), h = std::max(height >> i, 1u);
        if (size != LevelBytes(format, w, h) || offset + size > bytes.size())
            throw std::runtime_error(std::format("KTX2: level {} has an unexpected size", i));
        TextureImage::Level level{.offset = Align(image.data.size(), 16), .size = size, .width = w, .height = h};
        image.data.resize(level.offset + size);
        std::memcpy(image.data.data() + level.offset, bytes.data() + offset, size);
        image.levels.push_back(level);
    }
    return image;
}

std::vector<std::uint8_t> DecodeLevel(const TextureImage& image, std::size_t levelIndex)
{
    const TextureImage::Level& level = image.levels.at(levelIndex);
    const FormatInfo&          info  = RequireInfo(image.format);
    std::vector<std::uint8_t>  out(std::size_t{level.width} * level.height * 4);
    const std::byte*           data = image.data.data() + level.offset;
    if (!info.compressed) {
        std::memcpy(out.data(), data, out.size());
        return out;
    }
    InitEncoders(); // rgbcx tables
    const std::uint32_t bw = (level.width + 3) / 4, bh = (level.height + 3) / 4;
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            const std::byte* block = data + (std::size_t{by} * bw + bx) * info.blockBytes;
            std::uint8_t     texels[16 * 4];
            for (std::size_t i = 0; i < 16; ++i) { // defaults for channels a format lacks
                texels[i * 4 + 0] = texels[i * 4 + 1] = texels[i * 4 + 2] = 0;
                texels[i * 4 + 3] = 255;
            }
            switch (info.colorModel) {
            case kModelBc1: rgbcx::unpack_bc1(block, texels); break;
            case kModelBc3: rgbcx::unpack_bc3(block, texels); break;
            case kModelBc4: rgbcx::unpack_bc4(block, texels); break;
            case kModelBc5: rgbcx::unpack_bc5(block, texels, 0, 1, 4); break;
            default: bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba*>(texels)); break;
            }
            for (std::uint32_t y = 0; y < 4; ++y)
                for (std::uint32_t x = 0; x < 4; ++x) {
                    const std::uint32_t px = bx * 4 + x, py = by * 4 + y;
                    if (px < level.width && py < level.height)
                        std::memcpy(&out[(std::size_t{py} * level.width + px) * 4], &texels[(y * 4 + x) * 4], 4);
                }
        }
    }
    return out;
}

} // namespace Engine
