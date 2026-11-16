#pragma once
// A tiny glTF with morph targets for the morph tests (CPU and GPU): one mesh "Face" made of a quad
// (two triangles, indexed) in the XY plane with two named targets, default weights and an
// animation "Blink" that drives the weights (linear, 0 -> 1 -> 0 over 2 s).
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace MorphTest {

inline std::string Base64(const std::vector<std::uint8_t>& bytes)
{
    static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string           out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t n = (std::uint32_t{bytes[i]} << 16) | (i + 1 < bytes.size() ? std::uint32_t{bytes[i + 1]} << 8 : 0u) |
                                (i + 2 < bytes.size() ? std::uint32_t{bytes[i + 2]} : 0u);
        out += kDigits[(n >> 18) & 63];
        out += kDigits[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? kDigits[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kDigits[n & 63] : '=';
    }
    return out;
}

// Target 0 "Raise" lifts the top edge (+0.5 y), target 1 "Push" pushes the whole quad (+1 z).
inline std::string FaceGltf(float size = 1.0f)
{
    std::vector<std::uint8_t> buffer;
    const auto put = [&](const void* data, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        buffer.insert(buffer.end(), p, p + n);
    };
    const float s = size;
    const float positions[] = {-s, -s, 0, s, -s, 0, s, s, 0, -s, s, 0};
    const float normals[]   = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const std::uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    const float raise[]  = {0, 0, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0.5f, 0};
    const float push[]   = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const float times[]  = {0.0f, 1.0f, 2.0f};
    const float values[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f}; // per key: Raise, Push
    put(positions, sizeof(positions)); // 0
    put(normals, sizeof(normals));     // 48
    put(indices, sizeof(indices));     // 96
    buffer.resize(108, 0);             // align to 4
    put(raise, sizeof(raise));         // 108
    put(push, sizeof(push));           // 156
    put(times, sizeof(times));         // 204
    put(values, sizeof(values));       // 216
    return std::format(R"({{
  "asset": {{"version": "2.0"}}, "scene": 0, "scenes": [{{"nodes": [0]}}], "nodes": [{{"mesh": 0, "name": "Face"}}],
  "meshes": [{{"name": "Face", "weights": [0.25, 0.0], "extras": {{"targetNames": ["Raise", "Push"]}},
              "primitives": [{{"attributes": {{"POSITION": 0, "NORMAL": 1}}, "indices": 2,
                              "targets": [{{"POSITION": 3}}, {{"POSITION": 4}}]}}]}}],
  "animations": [{{"name": "Blink", "samplers": [{{"input": 5, "output": 6, "interpolation": "LINEAR"}}],
                  "channels": [{{"sampler": 0, "target": {{"node": 0, "path": "weights"}}}}]}}],
  "buffers": [{{"byteLength": {}, "uri": "data:application/octet-stream;base64,{}"}}],
  "bufferViews": [{{"buffer": 0, "byteOffset": 0, "byteLength": 48}}, {{"buffer": 0, "byteOffset": 48, "byteLength": 48}},
                  {{"buffer": 0, "byteOffset": 96, "byteLength": 12}}, {{"buffer": 0, "byteOffset": 108, "byteLength": 48}},
                  {{"buffer": 0, "byteOffset": 156, "byteLength": 48}}, {{"buffer": 0, "byteOffset": 204, "byteLength": 12}},
                  {{"buffer": 0, "byteOffset": 216, "byteLength": 24}}],
  "accessors": [
    {{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [{}, {}, 0], "max": [{}, {}, 0]}},
    {{"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"}},
    {{"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"}},
    {{"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC3", "min": [0, 0, 0], "max": [0, 0.5, 0]}},
    {{"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC3", "min": [0, 0, 1], "max": [0, 0, 1]}},
    {{"bufferView": 5, "componentType": 5126, "count": 3, "type": "SCALAR", "min": [0], "max": [2]}},
    {{"bufferView": 6, "componentType": 5126, "count": 6, "type": "SCALAR"}}]
}})",
                       buffer.size(), Base64(buffer), -s, -s, s, s);
}

} // namespace MorphTest
