#include "Engine/Terrain/TerrainBrush.h"

#include <algorithm>
#include <cmath>

namespace Engine {

namespace {

// Index ranges [lo, hi) of grid points within `radius` of `center` (point i at i * spacing + offset).
glm::ivec2 Range(float center, float radius, float spacing, float offset, int count)
{
    const int lo = static_cast<int>(std::floor((center - radius - offset) / spacing));
    const int hi = static_cast<int>(std::ceil((center + radius - offset) / spacing)) + 1;
    return {std::clamp(lo, 0, count), std::clamp(hi, 0, count)};
}

} // namespace

const char* ToString(TerrainTool tool)
{
    switch (tool) {
    case TerrainTool::Raise:   return "Raise";
    case TerrainTool::Lower:   return "Lower";
    case TerrainTool::Smooth:  return "Smooth";
    case TerrainTool::Flatten: return "Flatten";
    case TerrainTool::Paint:   return "Paint";
    case TerrainTool::Holes:   return "Holes";
    case TerrainTool::Foliage: return "Foliage";
    default:                   return "?";
    }
}

float TerrainBrushWeight(const TerrainBrush& brush, float distance)
{
    const float r = std::max(brush.radius, 1e-3f);
    if (distance >= r)
        return 0.0f;
    const float inner = r * (1.0f - std::clamp(brush.falloff, 0.0f, 1.0f));
    if (distance <= inner)
        return 1.0f;
    const float t = (distance - inner) / std::max(r - inner, 1e-4f); // 0 at the inner edge, 1 outside
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

TerrainChanges ApplyTerrainBrush(TerrainData& t, const TerrainBrush& brush, glm::vec2 local, float dt)
{
    TerrainChanges changes;
    if (!(brush.radius > 0.0f) || !(dt > 0.0f) || t.heights.empty())
        return changes;
    const float strength = std::clamp(brush.strength, 0.0f, 1.0f);
    const auto  n        = static_cast<int>(t.resolution);
    const float cell     = t.CellSize();
    glm::ivec4  touched(n, n, -1, -1); // grid: x0, z0, x1, z1 (inclusive)
    const auto  touch = [&](int x, int z) { touched = glm::ivec4(glm::min(glm::ivec2(touched), {x, z}), glm::max(glm::ivec2(touched.z, touched.w), {x, z})); };

    switch (brush.tool) {
    case TerrainTool::Raise:
    case TerrainTool::Lower:
    case TerrainTool::Smooth:
    case TerrainTool::Flatten: {
        const glm::ivec2 xs = Range(local.x, brush.radius, cell, 0.0f, n), zs = Range(local.y, brush.radius, cell, 0.0f, n);
        if (xs.x >= xs.y || zs.x >= zs.y)
            return changes;
        // Smoothing reads the unchanged neighbors: a copy of the region with one sample of margin.
        const int                  mx0 = std::max(xs.x - 1, 0), mz0 = std::max(zs.x - 1, 0);
        const int                  mx1 = std::min(xs.y + 1, n), mz1 = std::min(zs.y + 1, n);
        std::vector<std::uint16_t> source;
        if (brush.tool == TerrainTool::Smooth) {
            source.resize(static_cast<std::size_t>(mx1 - mx0) * static_cast<std::size_t>(mz1 - mz0));
            for (int z = mz0; z < mz1; ++z)
                std::copy_n(&t.heights[static_cast<std::size_t>(z) * t.resolution + static_cast<std::size_t>(mx0)], mx1 - mx0,
                            &source[static_cast<std::size_t>(z - mz0) * static_cast<std::size_t>(mx1 - mx0)]);
        }
        const auto  sourceAt = [&](int x, int z) {
            x = std::clamp(x, mx0, mx1 - 1), z = std::clamp(z, mz0, mz1 - 1);
            return static_cast<float>(source[static_cast<std::size_t>(z - mz0) * static_cast<std::size_t>(mx1 - mx0) + static_cast<std::size_t>(x - mx0)]);
        };
        const float toUnits = 65535.0f / std::max(t.heightScale, 1e-3f);
        const float rate    = strength * std::max(brush.radius * 0.5f, 0.25f) * dt * toUnits; // units this dab
        const float blend   = std::min(1.0f, strength * dt * 8.0f);
        const float target  = std::clamp(brush.target, 0.0f, t.heightScale) * toUnits;
        for (int z = zs.x; z < zs.y; ++z)
            for (int x = xs.x; x < xs.y; ++x) {
                const float w = TerrainBrushWeight(brush, glm::distance(local, glm::vec2(static_cast<float>(x), static_cast<float>(z)) * cell));
                if (w <= 0.0f)
                    continue;
                std::uint16_t& h     = t.heights[static_cast<std::size_t>(z) * t.resolution + static_cast<std::size_t>(x)];
                float          value = h;
                switch (brush.tool) {
                case TerrainTool::Raise: value += rate * w; break;
                case TerrainTool::Lower: value -= rate * w; break;
                case TerrainTool::Smooth: {
                    float sum = 0.0f;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx)
                            sum += sourceAt(x + dx, z + dz);
                    value += (sum / 9.0f - value) * blend * w;
                    break;
                }
                default: value += (target - value) * blend * w; break;
                }
                const auto next = static_cast<std::uint16_t>(std::clamp(std::lround(value), 0l, 65535l));
                if (next != h) {
                    h = next;
                    touch(x, z);
                }
            }
        changes.kinds = TerrainChangeHeights;
        break;
    }
    case TerrainTool::Holes: {
        // Cells (to the +x/+z of their sample) whose center is inside the brush's solid part.
        const glm::ivec2 xs = Range(local.x, brush.radius, cell, 0.5f * cell, n - 1), zs = Range(local.y, brush.radius, cell, 0.5f * cell, n - 1);
        if (t.holes.empty()) {
            if (brush.erase)
                return changes;
            t.holes.assign(static_cast<std::size_t>(t.resolution) * t.resolution, 0);
        }
        for (int z = zs.x; z < zs.y; ++z)
            for (int x = xs.x; x < xs.y; ++x) {
                const glm::vec2 center((static_cast<float>(x) + 0.5f) * cell, (static_cast<float>(z) + 0.5f) * cell);
                if (TerrainBrushWeight(brush, glm::distance(local, center)) < 0.5f)
                    continue;
                std::uint8_t& hole = t.holes[static_cast<std::size_t>(z) * t.resolution + static_cast<std::size_t>(x)];
                const std::uint8_t next = brush.erase ? 0 : 1;
                if (hole != next) {
                    hole = next;
                    touch(x, z);
                    touch(x + 1, z + 1);
                }
            }
        changes.kinds = TerrainChangeHoles;
        break;
    }
    case TerrainTool::Paint:
    case TerrainTool::Foliage: {
        const auto  s      = static_cast<int>(t.splatResolution);
        const float texel  = t.size / static_cast<float>(s);
        const glm::ivec2 xs = Range(local.x, brush.radius, texel, 0.5f * texel, s), zs = Range(local.y, brush.radius, texel, 0.5f * texel, s);
        const float amount = std::min(1.0f, strength * dt * 4.0f) * 255.0f;
        FoliageLayer* foliage = nullptr;
        if (brush.tool == TerrainTool::Foliage) {
            if (brush.layer >= t.foliage.size())
                return changes;
            foliage = &t.foliage[brush.layer];
            foliage->densityMap.resize(static_cast<std::size_t>(s) * static_cast<std::size_t>(s), 0);
        } else if (brush.layer >= t.layers.size() || brush.layer >= kMaxTerrainLayers) {
            return changes;
        }
        for (int z = zs.x; z < zs.y; ++z)
            for (int x = xs.x; x < xs.y; ++x) {
                const glm::vec2 center((static_cast<float>(x) + 0.5f) * texel, (static_cast<float>(z) + 0.5f) * texel);
                const float     w = TerrainBrushWeight(brush, glm::distance(local, center));
                if (w <= 0.0f)
                    continue;
                const std::size_t i     = static_cast<std::size_t>(z) * static_cast<std::size_t>(s) + static_cast<std::size_t>(x);
                const float       delta = amount * w;
                bool              changed = false;
                if (foliage) {
                    std::uint8_t& d    = foliage->densityMap[i];
                    const float   next = std::clamp(static_cast<float>(d) + (brush.erase ? -delta : delta), 0.0f, 255.0f);
                    const auto    v    = static_cast<std::uint8_t>(brush.erase ? std::floor(next) : std::ceil(next));
                    changed = v != d;
                    d       = v;
                } else {
                    // Raise the layer's weight, scale the others down: the weights keep summing to 255.
                    std::uint8_t* weights = &t.splat[i * kMaxTerrainLayers];
                    const float   current = weights[brush.layer];
                    const float   raised  = std::min(255.0f, current + delta);
                    float         others  = 0.0f;
                    for (std::uint32_t l = 0; l < kMaxTerrainLayers; ++l)
                        others += l == brush.layer ? 0.0f : static_cast<float>(weights[l]);
                    const float rest  = 255.0f - raised;
                    const float scale = others > 0.0f ? rest / others : 0.0f;
                    std::uint8_t next[kMaxTerrainLayers];
                    int          sum = 0;
                    for (std::uint32_t l = 0; l < kMaxTerrainLayers; ++l) {
                        next[l] = l == brush.layer ? 0 : static_cast<std::uint8_t>(std::floor(static_cast<float>(weights[l]) * scale));
                        sum += next[l];
                    }
                    next[brush.layer] = static_cast<std::uint8_t>(std::clamp(255 - sum, 0, 255)); // rounding goes to the painted layer
                    changed = !std::equal(next, next + kMaxTerrainLayers, weights);
                    std::copy_n(next, kMaxTerrainLayers, weights);
                }
                if (changed)
                    touch(x, z);
            }
        if (touched.z < 0)
            return changes;
        changes.kinds = foliage ? TerrainChangeFoliage : TerrainChangeSplat;
        changes.rect  = glm::vec4(static_cast<float>(touched.x) / static_cast<float>(s), static_cast<float>(touched.y) / static_cast<float>(s),
                                  static_cast<float>(touched.z + 1) / static_cast<float>(s), static_cast<float>(touched.w + 1) / static_cast<float>(s));
        return changes;
    }
    default:
        return changes;
    }
    if (touched.z < 0) {
        changes.kinds = 0;
        return changes;
    }
    const float last = static_cast<float>(n - 1);
    changes.rect = glm::vec4(static_cast<float>(touched.x) / last, static_cast<float>(touched.y) / last,
                             static_cast<float>(touched.z) / last, static_cast<float>(touched.w) / last);
    return changes;
}

TerrainRegion CaptureTerrainRegion(const TerrainData& t, std::uint32_t kinds, glm::vec4 rect, std::uint32_t foliageLayer)
{
    TerrainRegion r;
    r.kinds        = kinds;
    r.rect         = glm::clamp(rect, glm::vec4(0.0f), glm::vec4(1.0f));
    r.foliageLayer = foliageLayer;
    const std::uint32_t n = t.resolution, s = t.splatResolution;
    const auto span = [](float a, float b, std::uint32_t count, float scale) {
        const auto lo = static_cast<std::uint32_t>(std::clamp(std::floor(a * scale) - 1.0f, 0.0f, static_cast<float>(count)));
        const auto hi = static_cast<std::uint32_t>(std::clamp(std::ceil(b * scale) + 2.0f, 0.0f, static_cast<float>(count)));
        return glm::uvec2(lo, std::max(hi, lo));
    };
    const glm::uvec2 sx = span(r.rect.x, r.rect.z, n, static_cast<float>(n - 1)), sz = span(r.rect.y, r.rect.w, n, static_cast<float>(n - 1));
    const glm::uvec2 px = span(r.rect.x, r.rect.z, s, static_cast<float>(s)), pz = span(r.rect.y, r.rect.w, s, static_cast<float>(s));
    r.sampleRect = glm::uvec4(sx.x, sz.x, sx.y, sz.y);
    r.splatRect  = glm::uvec4(px.x, pz.x, px.y, pz.y);
    const auto copy = [](const auto& source, auto& out, std::uint32_t stride, glm::uvec4 box, std::size_t texel) {
        out.clear();
        if (source.empty())
            return;
        for (std::uint32_t z = box.y; z < box.w; ++z)
            out.insert(out.end(), source.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(z) * stride + box.x) * texel),
                       source.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(z) * stride + box.z) * texel));
    };
    if (kinds & TerrainChangeHeights)
        copy(t.heights, r.heights, n, r.sampleRect, 1);
    if (kinds & TerrainChangeHoles) {
        if (t.holes.empty()) // no mask yet: a region of solid ground
            r.holes.assign(static_cast<std::size_t>(r.sampleRect.z - r.sampleRect.x) * (r.sampleRect.w - r.sampleRect.y), 0);
        else
            copy(t.holes, r.holes, n, r.sampleRect, 1);
    }
    if (kinds & TerrainChangeSplat)
        copy(t.splat, r.splat, s, r.splatRect, kMaxTerrainLayers);
    if ((kinds & TerrainChangeFoliage) && foliageLayer < t.foliage.size())
        copy(t.foliage[foliageLayer].densityMap, r.density, s, r.splatRect, 1);
    return r;
}

bool RestoreTerrainRegion(TerrainData& t, const TerrainRegion& r)
{
    const std::uint32_t n = t.resolution, s = t.splatResolution;
    const auto paste = [](auto& target, const auto& values, std::uint32_t stride, glm::uvec4 box, std::size_t texel) {
        const std::size_t width = static_cast<std::size_t>(box.z - box.x) * texel;
        if (values.size() != width * (box.w - box.y))
            return false;
        if (target.size() < static_cast<std::size_t>(box.w) * stride * texel)
            return false;
        for (std::uint32_t z = box.y; z < box.w; ++z)
            std::copy_n(values.begin() + static_cast<std::ptrdiff_t>((z - box.y) * width), width,
                        target.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(z) * stride + box.x) * texel));
        return true;
    };
    bool ok = true;
    if (r.sampleRect.z > n || r.sampleRect.w > n || r.splatRect.z > s || r.splatRect.w > s)
        return false;
    if (r.kinds & TerrainChangeHeights)
        ok = paste(t.heights, r.heights, n, r.sampleRect, 1) && ok;
    if (r.kinds & TerrainChangeHoles) {
        if (t.holes.empty() && !r.holes.empty())
            t.holes.assign(static_cast<std::size_t>(n) * n, 0);
        if (!r.holes.empty())
            ok = paste(t.holes, r.holes, n, r.sampleRect, 1) && ok;
    }
    if (r.kinds & TerrainChangeSplat)
        ok = paste(t.splat, r.splat, s, r.splatRect, kMaxTerrainLayers) && ok;
    if ((r.kinds & TerrainChangeFoliage) && r.foliageLayer < t.foliage.size())
        ok = paste(t.foliage[r.foliageLayer].densityMap, r.density, s, r.splatRect, 1) && ok;
    return ok;
}

} // namespace Engine
