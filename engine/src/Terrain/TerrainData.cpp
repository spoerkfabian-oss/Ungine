#include "Engine/Terrain/TerrainData.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

namespace Engine {

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

constexpr std::uint32_t kFileVersion = 1;
constexpr char          kMagic[4]    = {'U', 'T', 'R', 'N'};

void SetError(std::string* error, std::string text)
{
    if (error)
        *error = std::move(text);
}

float Finite(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

// Relative to the terrain file (also "../Textures/x.png"); absolute only without a relative path.
std::string StorePath(const std::string& path, const fs::path& folder)
{
    if (path.empty())
        return path;
    const fs::path absolute = PathFromUtf8(path).lexically_normal();
    const fs::path relative = absolute.lexically_relative(folder);
    if (!absolute.is_absolute() || relative.empty())
        return path;
    const std::u8string generic = relative.generic_u8string();
    return {generic.begin(), generic.end()};
}

std::string LoadPath(const std::string& path, const fs::path& folder)
{
    if (path.empty())
        return path;
    const fs::path p = PathFromUtf8(path);
    return PathToUtf8((p.is_absolute() ? p : folder / p).lexically_normal());
}

json Vec3Json(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

void ReadVec3(const json& j, const char* key, glm::vec3& v)
{
    if (const auto it = j.find(key); it != j.end() && it->is_array() && it->size() == 3)
        for (int i = 0; i < 3; ++i)
            if ((*it)[static_cast<std::size_t>(i)].is_number())
                v[i] = (*it)[static_cast<std::size_t>(i)].get<float>();
}

template <class T>
void Read(const json& j, const char* key, T& value)
{
    if (const auto it = j.find(key); it != j.end()) {
        try {
            value = it->get<T>();
        } catch (const json::exception&) { // wrong type: keep the default
        }
    }
}

const char* ToString(FoliageKind kind) { return kind == FoliageKind::Mesh ? "mesh" : "grass"; }

std::uint32_t Crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0)
{
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::uint8_t b : bytes)
        crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

void PutBigEndian32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.insert(out.end(), {static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16),
                           static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)});
}

void PngChunk(std::vector<std::uint8_t>& png, const char type[4], const std::vector<std::uint8_t>& data)
{
    PutBigEndian32(png, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    PutBigEndian32(png, Crc32(std::span(png).subspan(start)));
}

// 16-bit grayscale PNG with stored (uncompressed) deflate blocks: no zlib dependency.
std::vector<std::uint8_t> EncodePng16(const std::vector<std::uint16_t>& samples, std::uint32_t width, std::uint32_t height)
{
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (1 + width * 2));
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint16_t v = samples[static_cast<std::size_t>(y) * width + x];
            raw.push_back(static_cast<std::uint8_t>(v >> 8));
            raw.push_back(static_cast<std::uint8_t>(v));
        }
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    for (std::size_t offset = 0; offset < raw.size() || raw.empty();) {
        const std::size_t   n    = std::min<std::size_t>(65535, raw.size() - offset);
        const bool          last = offset + n >= raw.size();
        const std::uint16_t len  = static_cast<std::uint16_t>(n);
        z.insert(z.end(), {static_cast<std::uint8_t>(last ? 1 : 0), static_cast<std::uint8_t>(len),
                           static_cast<std::uint8_t>(len >> 8), static_cast<std::uint8_t>(~len),
                           static_cast<std::uint8_t>(static_cast<std::uint16_t>(~len) >> 8)});
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset), raw.begin() + static_cast<std::ptrdiff_t>(offset + n));
        offset += n;
        if (last)
            break;
    }
    std::uint32_t a = 1, b = 0; // Adler-32
    for (std::uint8_t byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    PutBigEndian32(z, (b << 16) | a);

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    PutBigEndian32(ihdr, width);
    PutBigEndian32(ihdr, height);
    ihdr.insert(ihdr.end(), {16, 0, 0, 0, 0}); // 16 bit, grayscale, deflate, no filter, no interlace
    PngChunk(png, "IHDR", ihdr);
    PngChunk(png, "IDAT", z);
    PngChunk(png, "IEND", {});
    return png;
}

// Bilinear resample of a square 16-bit grid.
std::vector<std::uint16_t> Resample(const std::vector<std::uint16_t>& src, std::uint32_t srcSize, std::uint32_t dstSize)
{
    std::vector<std::uint16_t> dst(static_cast<std::size_t>(dstSize) * dstSize);
    const float scale = dstSize > 1 ? static_cast<float>(srcSize - 1) / static_cast<float>(dstSize - 1) : 0.0f;
    for (std::uint32_t y = 0; y < dstSize; ++y)
        for (std::uint32_t x = 0; x < dstSize; ++x) {
            const float         fx = static_cast<float>(x) * scale, fy = static_cast<float>(y) * scale;
            const std::uint32_t x0 = std::min(static_cast<std::uint32_t>(fx), srcSize - 1), y0 = std::min(static_cast<std::uint32_t>(fy), srcSize - 1);
            const std::uint32_t x1 = std::min(x0 + 1, srcSize - 1), y1 = std::min(y0 + 1, srcSize - 1);
            const float         tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
            const auto          at = [&](std::uint32_t px, std::uint32_t py) {
                return static_cast<float>(src[static_cast<std::size_t>(py) * srcSize + px]);
            };
            const float v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
            dst[static_cast<std::size_t>(y) * dstSize + x] = static_cast<std::uint16_t>(std::clamp(std::lround(v), 0l, 65535l));
        }
    return dst;
}

bool LowerExtension(const fs::path& file, std::initializer_list<const char*> extensions)
{
    std::string ext = PathToUtf8(file.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::ranges::any_of(extensions, [&](const char* e) { return ext == e; });
}

} // namespace

TerrainData MakeFlatTerrain(std::uint32_t resolution, float size, float heightScale, float height)
{
    TerrainData t;
    t.resolution  = resolution;
    t.size        = size;
    t.heightScale = heightScale;
    t.splatResolution = std::max(16u, resolution - 1);
    t.layers.push_back(TerrainLayer{.name = "Ground", .tint = {0.35f, 0.42f, 0.25f}});
    SanitizeTerrain(t);
    const auto value = static_cast<std::uint16_t>(std::clamp(height / std::max(heightScale, 1e-3f), 0.0f, 1.0f) * 65535.0f + 0.5f);
    std::ranges::fill(t.heights, value);
    return t;
}

void SanitizeTerrain(TerrainData& t)
{
    t.resolution      = std::clamp(t.resolution, kMinTerrainResolution, kMaxTerrainResolution);
    t.splatResolution = std::clamp(t.splatResolution, 16u, 4096u);
    t.size            = std::clamp(Finite(t.size, 256.0f), 1.0f, 100000.0f);
    t.heightScale     = std::clamp(Finite(t.heightScale, 64.0f), 0.01f, 100000.0f);
    t.triplanarSlope  = std::clamp(Finite(t.triplanarSlope, 40.0f), 0.0f, 90.0f);
    const std::size_t heights = static_cast<std::size_t>(t.resolution) * t.resolution;
    const std::size_t splat   = static_cast<std::size_t>(t.splatResolution) * t.splatResolution;
    if (t.heights.size() != heights)
        t.heights.assign(heights, 0);
    if (!t.holes.empty() && t.holes.size() != heights)
        t.holes.assign(heights, 0);
    if (t.layers.size() > kMaxTerrainLayers)
        t.layers.resize(kMaxTerrainLayers);
    if (t.layers.empty())
        t.layers.push_back(TerrainLayer{.name = "Ground"});
    if (t.splat.size() != splat * kMaxTerrainLayers) {
        t.splat.assign(splat * kMaxTerrainLayers, 0);
        for (std::size_t i = 0; i < splat; ++i)
            t.splat[i * kMaxTerrainLayers] = 255;
    }
    for (TerrainLayer& l : t.layers) {
        l.tiling         = std::clamp(Finite(l.tiling, 4.0f), 0.01f, 10000.0f);
        l.roughness      = std::clamp(Finite(l.roughness, 0.9f), 0.0f, 1.0f);
        l.metallic       = std::clamp(Finite(l.metallic, 0.0f), 0.0f, 1.0f);
        l.normalStrength = std::clamp(Finite(l.normalStrength, 1.0f), 0.0f, 4.0f);
        l.tint           = glm::clamp(l.tint, glm::vec3(0.0f), glm::vec3(4.0f));
    }
    if (t.foliage.size() > kMaxFoliageLayers)
        t.foliage.resize(kMaxFoliageLayers);
    for (FoliageLayer& f : t.foliage) {
        f.density      = std::clamp(Finite(f.density, 6.0f), 0.0f, 200.0f);
        f.width        = std::clamp(Finite(f.width, 0.35f), 0.01f, 100.0f);
        f.height       = std::clamp(Finite(f.height, 0.5f), 0.01f, 100.0f);
        f.minScale     = std::clamp(Finite(f.minScale, 0.8f), 0.01f, 100.0f);
        f.maxScale     = std::clamp(Finite(f.maxScale, 1.25f), f.minScale, 100.0f);
        f.wind         = std::clamp(Finite(f.wind, 0.25f), 0.0f, 10.0f);
        f.viewDistance = std::clamp(Finite(f.viewDistance, 60.0f), 1.0f, 10000.0f);
        f.maxSlope     = std::clamp(Finite(f.maxSlope, 40.0f), 0.0f, 90.0f);
        if (f.densityMap.size() != splat)
            f.densityMap.assign(splat, 0);
    }
}

float TerrainHeight(const TerrainData& t, glm::vec2 local)
{
    const float         cell = t.CellSize();
    const float         fx   = std::clamp(local.x / cell, 0.0f, static_cast<float>(t.resolution - 1));
    const float         fz   = std::clamp(local.y / cell, 0.0f, static_cast<float>(t.resolution - 1));
    const std::uint32_t x0   = std::min(static_cast<std::uint32_t>(fx), t.resolution - 2);
    const std::uint32_t z0   = std::min(static_cast<std::uint32_t>(fz), t.resolution - 2);
    const float         tx = fx - static_cast<float>(x0), tz = fz - static_cast<float>(z0);
    const float h00 = t.HeightAtSample(x0, z0), h10 = t.HeightAtSample(x0 + 1, z0);
    const float h01 = t.HeightAtSample(x0, z0 + 1), h11 = t.HeightAtSample(x0 + 1, z0 + 1);
    return (h00 * (1 - tx) + h10 * tx) * (1 - tz) + (h01 * (1 - tx) + h11 * tx) * tz;
}

glm::vec3 TerrainNormal(const TerrainData& t, glm::vec2 local)
{
    const float c  = t.CellSize();
    const float hl = TerrainHeight(t, local - glm::vec2(c, 0.0f)), hr = TerrainHeight(t, local + glm::vec2(c, 0.0f));
    const float hd = TerrainHeight(t, local - glm::vec2(0.0f, c)), hu = TerrainHeight(t, local + glm::vec2(0.0f, c));
    return glm::normalize(glm::vec3(hl - hr, 2.0f * c, hd - hu));
}

bool TerrainHole(const TerrainData& t, glm::vec2 local)
{
    if (t.holes.empty())
        return false;
    const float cell = t.CellSize();
    const auto  x    = static_cast<std::uint32_t>(std::clamp(local.x / cell, 0.0f, static_cast<float>(t.resolution - 2)));
    const auto  z    = static_cast<std::uint32_t>(std::clamp(local.y / cell, 0.0f, static_cast<float>(t.resolution - 2)));
    return t.HoleAtSample(x, z);
}

std::optional<float> RaycastTerrain(const TerrainData& t, glm::vec3 origin, glm::vec3 direction, float maxDistance)
{
    const float len = glm::length(direction);
    if (!(len > 1e-8f) || !(maxDistance > 0.0f))
        return std::nullopt;
    const glm::vec3 dir = direction / len;
    // Clip against the terrain box (x/z in [0, size], y in [0, heightScale]).
    float          t0 = 0.0f, t1 = maxDistance;
    const glm::vec3 lo(0.0f, -0.01f, 0.0f), hi(t.size, t.heightScale + 0.01f, t.size);
    for (int a = 0; a < 3; ++a) {
        if (std::abs(dir[a]) < 1e-9f) {
            if (origin[a] < lo[a] || origin[a] > hi[a])
                return std::nullopt;
            continue;
        }
        float n = (lo[a] - origin[a]) / dir[a], f = (hi[a] - origin[a]) / dir[a];
        if (n > f)
            std::swap(n, f);
        t0 = std::max(t0, n);
        t1 = std::min(t1, f);
        if (t0 > t1)
            return std::nullopt;
    }
    const auto above = [&](float s) {
        const glm::vec3 p = origin + dir * s;
        return p.y - TerrainHeight(t, glm::vec2(p.x, p.z));
    };
    const float step = t.CellSize() * 0.5f;
    float       prev = t0, prevValue = above(t0);
    if (prevValue < 0.0f && !TerrainHole(t, glm::vec2(origin.x + dir.x * t0, origin.z + dir.z * t0)))
        return t0; // starts below the surface
    for (float s = t0 + step;; s += step) {
        const float cur   = std::min(s, t1);
        const float value = above(cur);
        if (prevValue >= 0.0f && value < 0.0f) {
            float a = prev, b = cur; // bisection
            for (int i = 0; i < 20; ++i) {
                const float m = 0.5f * (a + b);
                (above(m) >= 0.0f ? a : b) = m;
            }
            const glm::vec3 p = origin + dir * b;
            if (!TerrainHole(t, glm::vec2(p.x, p.z)))
                return b;
        }
        prev      = cur;
        prevValue = value;
        if (cur >= t1)
            return std::nullopt;
    }
}

bool SaveTerrain(const fs::path& file, const TerrainData& input, std::string* error)
{
    TerrainData t = input;
    SanitizeTerrain(t);
    const fs::path folder = file.parent_path().lexically_normal();
    json           j;
    j["version"]         = kFileVersion;
    j["resolution"]      = t.resolution;
    j["size"]            = t.size;
    j["heightScale"]     = t.heightScale;
    j["splatResolution"] = t.splatResolution;
    j["triplanarSlope"]  = t.triplanarSlope;
    j["holes"]           = !t.holes.empty();
    j["layers"]          = json::array();
    for (const TerrainLayer& l : t.layers)
        j["layers"].push_back({{"name", l.name},
                               {"albedo", StorePath(l.albedo, folder)},
                               {"normal", StorePath(l.normal, folder)},
                               {"orm", StorePath(l.orm, folder)},
                               {"tint", Vec3Json(l.tint)},
                               {"tiling", l.tiling},
                               {"roughness", l.roughness},
                               {"metallic", l.metallic},
                               {"normalStrength", l.normalStrength}});
    j["foliage"] = json::array();
    for (const FoliageLayer& f : t.foliage)
        j["foliage"].push_back({{"name", f.name},
                                {"kind", ToString(f.kind)},
                                {"texture", StorePath(f.texture, folder)},
                                {"mesh", StorePath(f.mesh, folder)},
                                {"meshIndex", f.meshIndex},
                                {"density", f.density},
                                {"width", f.width},
                                {"height", f.height},
                                {"minScale", f.minScale},
                                {"maxScale", f.maxScale},
                                {"baseColor", Vec3Json(f.baseColor)},
                                {"tipColor", Vec3Json(f.tipColor)},
                                {"wind", f.wind},
                                {"viewDistance", f.viewDistance},
                                {"maxSlope", f.maxSlope},
                                {"alignToNormal", f.alignToNormal},
                                {"castShadows", f.castShadows},
                                {"seed", f.seed}});
    const std::string text = j.dump();

    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            SetError(error, "cannot write '" + PathToUtf8(temp) + "'");
            return false;
        }
        const std::uint32_t version = kFileVersion, jsonSize = static_cast<std::uint32_t>(text.size());
        out.write(kMagic, 4);
        out.write(reinterpret_cast<const char*>(&version), 4);
        out.write(reinterpret_cast<const char*>(&jsonSize), 4);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.write(reinterpret_cast<const char*>(t.heights.data()), static_cast<std::streamsize>(t.heights.size() * 2));
        out.write(reinterpret_cast<const char*>(t.splat.data()), static_cast<std::streamsize>(t.splat.size()));
        if (!t.holes.empty())
            out.write(reinterpret_cast<const char*>(t.holes.data()), static_cast<std::streamsize>(t.holes.size()));
        for (const FoliageLayer& f : t.foliage)
            out.write(reinterpret_cast<const char*>(f.densityMap.data()), static_cast<std::streamsize>(f.densityMap.size()));
        if (!out) {
            SetError(error, "write failed: '" + PathToUtf8(temp) + "'");
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, file, ec);
    if (ec) {
        SetError(error, "cannot replace '" + PathToUtf8(file) + "': " + ec.message());
        return false;
    }
    return true;
}

std::optional<TerrainData> LoadTerrain(const fs::path& file, std::string* error)
{
    std::optional<std::vector<std::byte>> bytes;
    try {
        bytes = Vfs::Read(file);
    } catch (const std::exception& e) {
        SetError(error, e.what());
        return std::nullopt;
    }
    if (!bytes) {
        SetError(error, "cannot read '" + PathToUtf8(file) + "'");
        return std::nullopt;
    }
    const std::byte* data   = bytes->data();
    std::size_t      remain = bytes->size();
    const auto       take   = [&](void* out, std::size_t n) {
        if (remain < n)
            return false;
        if (n > 0)
            std::memcpy(out, data, n);
        data += n;
        remain -= n;
        return true;
    };
    char          magic[4];
    std::uint32_t version = 0, jsonSize = 0;
    if (!take(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !take(&version, 4) || !take(&jsonSize, 4)) {
        SetError(error, "'" + PathToUtf8(file) + "' is not a terrain file");
        return std::nullopt;
    }
    if (version > kFileVersion) {
        SetError(error, "'" + PathToUtf8(file) + "' was written by a newer version");
        return std::nullopt;
    }
    std::string text(jsonSize, '\0');
    if (!take(text.data(), jsonSize)) {
        SetError(error, "'" + PathToUtf8(file) + "' is truncated");
        return std::nullopt;
    }
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        SetError(error, "'" + PathToUtf8(file) + "': " + e.what());
        return std::nullopt;
    }
    const fs::path folder = file.parent_path().lexically_normal();
    TerrainData    t;
    Read(j, "resolution", t.resolution);
    Read(j, "size", t.size);
    Read(j, "heightScale", t.heightScale);
    Read(j, "splatResolution", t.splatResolution);
    Read(j, "triplanarSlope", t.triplanarSlope);
    bool holes = false;
    Read(j, "holes", holes);
    if (t.resolution < kMinTerrainResolution || t.resolution > kMaxTerrainResolution || t.splatResolution < 16 ||
        t.splatResolution > 4096) {
        SetError(error, "'" + PathToUtf8(file) + "': invalid resolution");
        return std::nullopt;
    }
    if (const auto it = j.find("layers"); it != j.end() && it->is_array())
        for (const json& l : *it) {
            TerrainLayer layer;
            Read(l, "name", layer.name);
            Read(l, "albedo", layer.albedo);
            Read(l, "normal", layer.normal);
            Read(l, "orm", layer.orm);
            ReadVec3(l, "tint", layer.tint);
            Read(l, "tiling", layer.tiling);
            Read(l, "roughness", layer.roughness);
            Read(l, "metallic", layer.metallic);
            Read(l, "normalStrength", layer.normalStrength);
            for (std::string* p : {&layer.albedo, &layer.normal, &layer.orm})
                *p = LoadPath(*p, folder);
            t.layers.push_back(std::move(layer));
        }
    if (const auto it = j.find("foliage"); it != j.end() && it->is_array())
        for (const json& f : *it) {
            FoliageLayer layer;
            std::string  kind;
            Read(f, "name", layer.name);
            Read(f, "kind", kind);
            layer.kind = kind == "mesh" ? FoliageKind::Mesh : FoliageKind::Grass;
            Read(f, "texture", layer.texture);
            Read(f, "mesh", layer.mesh);
            Read(f, "meshIndex", layer.meshIndex);
            Read(f, "density", layer.density);
            Read(f, "width", layer.width);
            Read(f, "height", layer.height);
            Read(f, "minScale", layer.minScale);
            Read(f, "maxScale", layer.maxScale);
            ReadVec3(f, "baseColor", layer.baseColor);
            ReadVec3(f, "tipColor", layer.tipColor);
            Read(f, "wind", layer.wind);
            Read(f, "viewDistance", layer.viewDistance);
            Read(f, "maxSlope", layer.maxSlope);
            Read(f, "alignToNormal", layer.alignToNormal);
            Read(f, "castShadows", layer.castShadows);
            Read(f, "seed", layer.seed);
            layer.texture = LoadPath(layer.texture, folder);
            layer.mesh    = LoadPath(layer.mesh, folder);
            t.foliage.push_back(std::move(layer));
        }
    if (t.layers.size() > kMaxTerrainLayers)
        t.layers.resize(kMaxTerrainLayers);
    if (t.foliage.size() > kMaxFoliageLayers)
        t.foliage.resize(kMaxFoliageLayers);

    const std::size_t heights = static_cast<std::size_t>(t.resolution) * t.resolution;
    const std::size_t splat   = static_cast<std::size_t>(t.splatResolution) * t.splatResolution;
    t.heights.resize(heights);
    t.splat.resize(splat * kMaxTerrainLayers);
    bool ok = take(t.heights.data(), heights * 2) && take(t.splat.data(), t.splat.size());
    if (ok && holes) {
        t.holes.resize(heights);
        ok = take(t.holes.data(), heights);
    }
    for (FoliageLayer& f : t.foliage) {
        f.densityMap.resize(splat);
        ok = ok && take(f.densityMap.data(), splat);
    }
    if (!ok) {
        SetError(error, "'" + PathToUtf8(file) + "' is truncated");
        return std::nullopt;
    }
    SanitizeTerrain(t);
    return t;
}

bool ImportHeightmap(TerrainData& terrain, const fs::path& file, std::string* error)
{
    std::optional<std::vector<std::byte>> bytes;
    try {
        bytes = Vfs::Read(file);
    } catch (const std::exception& e) {
        SetError(error, e.what());
        return false;
    }
    if (!bytes || bytes->empty()) {
        SetError(error, "cannot read '" + PathToUtf8(file) + "'");
        return false;
    }
    std::vector<std::uint16_t> samples;
    std::uint32_t              size = 0;
    if (LowerExtension(file, {".raw", ".r16"})) {
        const std::size_t count = bytes->size() / 2;
        size                    = static_cast<std::uint32_t>(std::lround(std::sqrt(static_cast<double>(count))));
        if (bytes->size() % 2 != 0 || static_cast<std::size_t>(size) * size != count || size < 2) {
            SetError(error, "'" + PathToUtf8(file) + "': RAW heightmaps must be square 16-bit (little endian)");
            return false;
        }
        samples.resize(count);
        std::memcpy(samples.data(), bytes->data(), count * 2);
    } else {
        int w = 0, h = 0, channels = 0;
        stbi_us* pixels = stbi_load_16_from_memory(reinterpret_cast<const stbi_uc*>(bytes->data()),
                                                   static_cast<int>(bytes->size()), &w, &h, &channels, 1);
        if (!pixels) {
            SetError(error, "'" + PathToUtf8(file) + "': " + (stbi_failure_reason() ? stbi_failure_reason() : "unreadable image"));
            return false;
        }
        if (w != h || w < 2) {
            stbi_image_free(pixels);
            SetError(error, "'" + PathToUtf8(file) + "': heightmaps must be square");
            return false;
        }
        size = static_cast<std::uint32_t>(w);
        samples.assign(pixels, pixels + static_cast<std::size_t>(w) * h);
        stbi_image_free(pixels);
    }
    terrain.heights = size == terrain.resolution ? std::move(samples) : Resample(samples, size, terrain.resolution);
    return true;
}

bool ExportHeightmap(const TerrainData& terrain, const fs::path& file, std::string* error)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        SetError(error, "cannot write '" + PathToUtf8(file) + "'");
        return false;
    }
    if (LowerExtension(file, {".raw", ".r16"})) {
        out.write(reinterpret_cast<const char*>(terrain.heights.data()), static_cast<std::streamsize>(terrain.heights.size() * 2));
    } else {
        const std::vector<std::uint8_t> png = EncodePng16(terrain.heights, terrain.resolution, terrain.resolution);
        out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }
    if (!out) {
        SetError(error, "write failed: '" + PathToUtf8(file) + "'");
        return false;
    }
    return true;
}

// --- TerrainStore ---------------------------------------------------------------------------------

namespace {

double Now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string Key(const std::string& path)
{
    return path.empty() ? path : PathToUtf8(PathFromUtf8(path).lexically_normal());
}

constexpr std::size_t kMaxTrackedEdits = 256;

} // namespace

TerrainStore& TerrainStore::Instance()
{
    static TerrainStore store;
    return store;
}

void TerrainStore::Bump(Entry& entry, bool full, std::uint32_t kinds, glm::vec4 rect)
{
    entry.revision = m_NextRevision++;
    if (full || entry.edits.size() >= kMaxTrackedEdits) {
        entry.fullRevision = entry.revision;
        entry.edits.clear();
        return;
    }
    entry.edits.push_back({entry.revision, kinds, rect});
}

std::shared_ptr<TerrainData> TerrainStore::Get(const std::string& path)
{
    if (path.empty())
        return nullptr;
    const std::string key = Key(path);
    auto it = std::ranges::find(m_Entries, key, &std::pair<std::string, Entry>::first);
    if (it == m_Entries.end()) {
        m_Entries.emplace_back(key, Entry{});
        it = std::prev(m_Entries.end());
        it->second.lastCheck = -1e9;
    }
    Entry&       entry = it->second;
    const double now   = Now();
    if (entry.dirty || now - entry.lastCheck < 1.0)
        return entry.data;
    entry.lastCheck = now;
    const fs::path                  file  = PathFromUtf8(key);
    const fs::file_time_type        mtime = Vfs::ModifiedTime(file);
    if (entry.data && mtime == entry.mtime)
        return entry.data;
    if (mtime == fs::file_time_type::min()) { // missing: keep what we have (unsaved data)
        if (!entry.data && !std::exchange(entry.warned, true))
            ENGINE_WARN("Terrain '{}' not found", key);
        return entry.data;
    }
    std::string error;
    if (auto loaded = LoadTerrain(file, &error)) {
        entry.data   = std::make_shared<TerrainData>(std::move(*loaded));
        entry.mtime  = mtime;
        entry.warned = false;
        Bump(entry, true, TerrainChangeAll, {0.0f, 0.0f, 1.0f, 1.0f});
    } else {
        entry.mtime = mtime; // retried when the file changes again
        if (!std::exchange(entry.warned, true))
            ENGINE_WARN("Terrain: {}", error);
    }
    return entry.data;
}

void TerrainStore::Provide(const std::string& path, std::shared_ptr<TerrainData> data)
{
    const std::string key = Key(path);
    auto it = std::ranges::find(m_Entries, key, &std::pair<std::string, Entry>::first);
    if (it == m_Entries.end()) {
        m_Entries.emplace_back(key, Entry{});
        it = std::prev(m_Entries.end());
    }
    it->second.data      = std::move(data);
    it->second.dirty     = true;
    it->second.lastCheck = Now();
    Bump(it->second, true, TerrainChangeAll, {0.0f, 0.0f, 1.0f, 1.0f});
}

void TerrainStore::MarkDirty(const std::string& path, std::uint32_t kinds, glm::vec4 rect)
{
    const std::string key = Key(path);
    const auto it = std::ranges::find(m_Entries, key, &std::pair<std::string, Entry>::first);
    if (it == m_Entries.end() || !it->second.data)
        return;
    it->second.dirty = true;
    Bump(it->second, (kinds & TerrainChangeSettings) != 0, kinds, glm::clamp(rect, glm::vec4(0.0f), glm::vec4(1.0f)));
}

std::uint64_t TerrainStore::Revision(const std::string& path) const
{
    const auto it = std::ranges::find(m_Entries, Key(path), &std::pair<std::string, Entry>::first);
    return it == m_Entries.end() || !it->second.data ? 0 : it->second.revision;
}

TerrainChanges TerrainStore::ChangesSince(const std::string& path, std::uint64_t revision) const
{
    TerrainChanges changes;
    const auto it = std::ranges::find(m_Entries, Key(path), &std::pair<std::string, Entry>::first);
    if (it == m_Entries.end() || !it->second.data)
        return changes;
    const Entry& entry = it->second;
    if (revision >= entry.revision)
        return changes;
    if (revision < entry.fullRevision) {
        changes.kinds = TerrainChangeAll;
        changes.full  = true;
        return changes;
    }
    bool first = true;
    for (const Edit& e : entry.edits) {
        if (e.revision <= revision)
            continue;
        changes.kinds |= e.kinds;
        changes.rect = first ? e.rect
                             : glm::vec4(glm::min(glm::vec2(changes.rect), glm::vec2(e.rect)),
                                         glm::max(glm::vec2(changes.rect.z, changes.rect.w), glm::vec2(e.rect.z, e.rect.w)));
        first = false;
    }
    return changes;
}

bool TerrainStore::Save(const std::string& path, std::string* error)
{
    const std::string key = Key(path);
    const auto it = std::ranges::find(m_Entries, key, &std::pair<std::string, Entry>::first);
    if (it == m_Entries.end() || !it->second.data) {
        SetError(error, "terrain '" + key + "' is not loaded");
        return false;
    }
    if (!SaveTerrain(PathFromUtf8(key), *it->second.data, error))
        return false;
    it->second.dirty = false;
    it->second.mtime = Vfs::ModifiedTime(PathFromUtf8(key));
    return true;
}

bool TerrainStore::Dirty(const std::string& path) const
{
    const auto it = std::ranges::find(m_Entries, Key(path), &std::pair<std::string, Entry>::first);
    return it != m_Entries.end() && it->second.dirty;
}

void TerrainStore::Reload(const std::string& path)
{
    const auto it = std::ranges::find(m_Entries, Key(path), &std::pair<std::string, Entry>::first);
    if (it == m_Entries.end())
        return;
    it->second.dirty     = false;
    it->second.mtime     = {};
    it->second.lastCheck = -1e9;
    (void)Get(path);
}

void TerrainStore::Clear()
{
    m_Entries.clear();
}

} // namespace Engine
