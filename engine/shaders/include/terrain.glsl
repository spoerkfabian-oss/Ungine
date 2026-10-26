#ifndef ENGINE_TERRAIN_GLSL
#define ENGINE_TERRAIN_GLSL
// Heightmap terrain (CDLOD). Mirrors Engine::GpuTerrain / GpuTerrainNode (TerrainRenderer.h) and
// TerrainPush (TerrainRenderer.cpp). Requires bindless.glsl + frame.glsl.
#include "bindless.glsl"
#include "frame.glsl"

#define TERRAIN_MAX_LAYERS 8u
#define TERRAIN_MAX_LODS   16u
#define TERRAIN_NO_TEXTURE 0xFFFFFFFFu

struct GpuTerrainLayer {
    uvec4 textures; // texture table entries: albedo, normal, orm (TERRAIN_NO_TEXTURE: none)
    vec4  tint;     // rgb tint, w: 1 / tiling (repeats per meter)
    vec4  material; // x roughness, y metallic, z normal strength
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer TerrainBuffer {
    vec4  origin;   // xyz: world position of the -X/-Z corner at height 0, w: size (meters)
    vec4  params;   // x: height scale, y: resolution, z: cell size, w: cos(triplanar start angle)
    uvec4 textures; // bindless slots: height (R16), splat 0-3, splat 4-7, holes (TERRAIN_NO_TEXTURE: none)
    uvec4 info;     // x: layer count, y: entity id (slot + 1), z: LOD count, w: patch quads per side
    vec4  camera;   // xyz: LOD camera (world), w: splat resolution
    vec4  morph[TERRAIN_MAX_LODS]; // per LOD: x morph start, y morph end (distance)
    GpuTerrainLayer layers[TERRAIN_MAX_LAYERS];
};

struct TerrainNode {
    vec2  origin; // terrain space (meters from the corner)
    float size;
    uint  lod;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer TerrainNodeBuffer { TerrainNode n[]; };

layout(push_constant) uniform TerrainPush {
    FrameData         frame;
    TerrainBuffer     terrain;
    TerrainNodeBuffer nodes;
    uint              cascade; // shadow passes
    uint              flags;
} pc;

vec2 TerrainMotionVector(vec4 currentClip, vec4 previousClip)
{
    return (currentClip.xy / currentClip.w - previousClip.xy / previousClip.w) * vec2(0.5, -0.5);
}

vec4 TerrainSample(uint entry, vec2 uv)
{
    return SampleTexture(pc.frame.textureTable.slot[entry], SAMPLER_LINEAR_REPEAT, uv);
}

// Height texture UV of a terrain-space point (sample centers on the grid points).
vec2 TerrainHeightUv(TerrainBuffer t, vec2 local)
{
    return (local / t.params.z + 0.5) / t.params.y;
}

float TerrainHeight(TerrainBuffer t, vec2 local)
{
    return textureLod(sampler2D(uTextures[nonuniformEXT(t.textures.x)], uSamplers[SAMPLER_LINEAR_CLAMP]),
                      TerrainHeightUv(t, local), 0.0).r * t.params.x;
}

// Central differences of the height field (one cell apart).
vec3 TerrainNormal(TerrainBuffer t, vec2 local)
{
    const float c  = t.params.z;
    const float hl = TerrainHeight(t, local - vec2(c, 0.0)), hr = TerrainHeight(t, local + vec2(c, 0.0));
    const float hd = TerrainHeight(t, local - vec2(0.0, c)), hu = TerrainHeight(t, local + vec2(0.0, c));
    return normalize(vec3(hl - hr, 2.0 * c, hd - hu));
}

bool TerrainHole(TerrainBuffer t, vec2 local)
{
    if (t.textures.w == TERRAIN_NO_TEXTURE)
        return false;
    const ivec2 cell = clamp(ivec2(floor(local / t.params.z)), ivec2(0), ivec2(int(t.params.y) - 2));
    return texelFetch(sampler2D(uTextures[nonuniformEXT(t.textures.w)], uSamplers[SAMPLER_NEAREST_CLAMP]), cell, 0).r > 0.001; // any non-zero byte
}

// CDLOD vertex: grid point of the node, morphed towards the next coarser grid with the distance to
// the LOD camera, displaced by the height field. Returns the world position.
vec3 TerrainVertex(TerrainBuffer t, TerrainNode node, uint vertexIndex, out vec2 local)
{
    const uint  quads = t.info.w;
    vec2        grid  = vec2(float(vertexIndex % (quads + 1u)), float(vertexIndex / (quads + 1u)));
    const float step  = node.size / float(quads);
    vec2        pos   = clamp(node.origin + grid * step, vec2(0.0), vec2(t.origin.w));
    const vec3  world = t.origin.xyz + vec3(pos.x, TerrainHeight(t, pos), pos.y);
    const vec2  morph = t.morph[node.lod].xy;
    const float k     = clamp((distance(t.camera.xyz, world) - morph.x) / max(morph.y - morph.x, 1e-4), 0.0, 1.0);
    grid -= fract(grid * 0.5) * 2.0 * k;
    pos   = clamp(node.origin + grid * step, vec2(0.0), vec2(t.origin.w));
    local = pos;
    return t.origin.xyz + vec3(pos.x, TerrainHeight(t, pos), pos.y);
}

#endif
