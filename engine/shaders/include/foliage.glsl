#ifndef ENGINE_FOLIAGE_GLSL
#define ENGINE_FOLIAGE_GLSL
// Terrain foliage: instances scattered on the CPU per chunk (terrain-space position + random bits),
// placed on the height field here (sculpting moves them without a rescatter), dropped on holes and
// steep ground, thinned and shrunk with the distance to the camera, swayed by the wind.
// Mirrors GpuFoliageLayer / GpuFoliageInstance / FoliagePush (TerrainRenderer).
#define TERRAIN_CUSTOM_PUSH
#include "terrain.glsl"
#include "scene_common.glsl"

#define FOLIAGE_BLADES 0u // procedural grass blades (15 vertices)
#define FOLIAGE_CARDS  1u // two crossed textured quads (12 vertices), alpha tested
#define FOLIAGE_MESH   2u // geometry pool submesh (indexed draw)

#define FOLIAGE_ALIGN_TO_NORMAL 1u

struct FoliageInstance {
    vec2 local;  // terrain space (meters from the -X/-Z corner)
    uint bits;   // 8 bit each: yaw, scale, key (position in the shuffled chunk list), tint
    uint unused;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FoliageInstanceBuffer { FoliageInstance i[]; };

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FoliageLayerBuffer {
    vec4  baseColor; // rgb, w: wind amplitude (meters at the tip)
    vec4  tipColor;  // rgb, w: time (seconds)
    vec4  size;      // x width, y height (mesh: 1), z min scale, w max scale
    vec4  fade;      // x view distance, y cos(max slope), z time step (motion vectors)
    uvec4 info;      // x kind, y texture entry (cards), z flags, w entity id (slot + 1)
};

layout(push_constant) uniform FoliagePush {
    FrameData             frame;
    TerrainBuffer         terrain;
    FoliageLayerBuffer    layer;
    FoliageInstanceBuffer instances;
    uint                  cascade;
    uint                  material; // mesh foliage: the submesh's material
} pc;

vec4 TerrainSample(uint entry, vec2 uv)
{
    return SampleTexture(pc.frame.textureTable.slot[entry], SAMPLER_LINEAR_REPEAT, uv);
}

struct FoliageVertex {
    vec3  world;
    vec3  previous; // last frame's position (wind)
    vec3  normal;
    vec3  color;
    float occlusion;
    vec2  uv;
    bool  visible;
};

vec3 FoliageWind(vec2 world, float time, float amplitude)
{
    const vec2  dir   = vec2(0.94, 0.34);
    const float phase = dot(world, dir) * 0.35 + sin(world.y * 0.21 + world.x * 0.13) * 1.7;
    const float sway  = sin(time * 1.9 + phase) * 0.7 + sin(time * 3.7 + phase * 2.3) * 0.3;
    const float gust  = 0.6 + 0.4 * sin(time * 0.43 + dot(world, dir) * 0.05);
    return vec3(dir.x, 0.0, dir.y) * (sway * gust * amplitude);
}

FoliageVertex FoliageVertexOf(uint instanceIndex, uint vertexIndex)
{
    FoliageVertex       o;
    TerrainBuffer       t     = pc.terrain;
    FoliageLayerBuffer  layer = pc.layer;
    const FoliageInstance inst = pc.instances.i[instanceIndex];
    o.visible   = false;
    o.world     = vec3(0.0);
    o.previous  = vec3(0.0);
    o.normal    = vec3(0.0, 1.0, 0.0);
    o.color     = vec3(0.0);
    o.occlusion = 1.0;
    o.uv        = vec2(0.0);

    const float yaw  = float(inst.bits & 255u) * (6.2831853 / 256.0);
    const float rnd  = float((inst.bits >> 8) & 255u) / 255.0;
    const float key  = float((inst.bits >> 16) & 255u) / 255.0;
    const float tint = float(inst.bits >> 24) / 255.0;
    const vec2  local = inst.local;
    if (TerrainHole(t, local))
        return o;
    const vec3 ground = TerrainNormal(t, local);
    if (ground.y < layer.fade.y)
        return o;
    const vec3 root = t.origin.xyz + vec3(local.x, TerrainHeight(t, local), local.y);

    // Distance (LOD camera: every view picks the same instances): thinned beyond 40 % of the view
    // distance down to 30 %, the survivors shrink away over the last 15 %.
    const float range = layer.fade.x;
    const float d     = distance(t.camera.xyz, root);
    const float keep  = d < 0.4 * range ? 1.0 : mix(1.0, 0.3, clamp((d - 0.4 * range) / (0.45 * range), 0.0, 1.0));
    float       scale = mix(layer.size.z, layer.size.w, rnd);
    scale *= clamp((keep - key) / 0.08 + 1.0, 0.0, 1.0) * clamp((range - d) / (0.15 * range), 0.0, 1.0);
    if (scale <= 0.001)
        return o;

    const vec3  up     = (layer.info.z & FOLIAGE_ALIGN_TO_NORMAL) != 0u ? ground : normalize(mix(vec3(0, 1, 0), ground, 0.3));
    const vec3  side0  = vec3(cos(yaw), 0.0, sin(yaw));
    const vec3  right  = normalize(side0 - up * dot(side0, up));
    const vec3  facing = cross(right, up);
    const float time   = layer.tipColor.w;
    const float amp    = layer.baseColor.w;
    float height01 = 0.0; // 0 root .. 1 tip
    vec3  offset   = vec3(0.0);
    vec3  normal   = ground;
    const uint kind = layer.info.x;
    if (kind == FOLIAGE_MESH) {
        const Vertex v = pc.frame.vertices.v[vertexIndex];
        const vec3   p = v.position * scale;
        offset   = right * p.x + up * p.y + facing * p.z; // yaw 0: identity (facing = +Z)
        normal   = normalize(right * v.normal.x + up * v.normal.y + facing * v.normal.z);
        height01 = clamp(p.y / max(layer.size.y * scale, 0.05), 0.0, 1.0);
        o.uv     = vec2(v.uvX, v.uvY);
        o.color  = vec3(0.85 + 0.3 * tint);
    } else if (kind == FOLIAGE_CARDS) {
        // Two quads crossed at 90 degrees; vertex 0-5 first quad, 6-11 second.
        const uint  quad = vertexIndex / 6u;
        const uint  c    = vertexIndex % 6u;
        const vec2  corner = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1))[c];
        const vec3  axis   = quad == 0u ? right : facing;
        offset   = axis * ((corner.x - 0.5) * layer.size.x * scale) + up * (corner.y * layer.size.y * scale);
        height01 = corner.y;
        o.uv     = vec2(corner.x, 1.0 - corner.y);
        normal   = normalize(mix(ground, quad == 0u ? facing : -right, 0.35));
        o.color  = mix(layer.baseColor.rgb, layer.tipColor.rgb, corner.y) * (0.85 + 0.3 * tint);
    } else {
        // Blade: 3 segments narrowing to a tip, curved forward.
        const uint  tri    = vertexIndex / 3u, corner = vertexIndex % 3u;
        // Triangles (level, side): (0L 0R 1L) (0R 1R 1L) (1L 1R 2L) (1R 2R 2L) (2L 2R tip)
        const uvec2 verts[15] = uvec2[](uvec2(0, 0), uvec2(0, 1), uvec2(1, 0), uvec2(0, 1), uvec2(1, 1), uvec2(1, 0),
                                        uvec2(1, 0), uvec2(1, 1), uvec2(2, 0), uvec2(1, 1), uvec2(2, 1), uvec2(2, 0),
                                        uvec2(2, 0), uvec2(2, 1), uvec2(3, 0));
        const uvec2 lv     = verts[tri * 3u + corner];
        const float levels[4] = float[](0.0, 0.45, 0.8, 1.0);
        const float widths[4] = float[](1.0, 0.75, 0.4, 0.0);
        height01  = levels[lv.x];
        const float w = widths[lv.x] * layer.size.x * scale * 0.5;
        const float h = height01 * layer.size.y * scale;
        offset    = right * (lv.y == 0u ? -w : w) + up * h + facing * (height01 * height01 * 0.3 * layer.size.y * scale);
        // Rounded across the blade, leaning towards the ground normal (soft, even lighting).
        normal    = normalize(ground * 1.2 + facing * 0.6 + right * (lv.y == 0u ? -0.25 : 0.25));
        o.color   = mix(layer.baseColor.rgb, layer.tipColor.rgb, height01) * (0.85 + 0.3 * tint);
    }
    const float bend = height01 * height01;
    o.world     = root + offset + FoliageWind(root.xz, time, amp * bend * scale);
    o.previous  = root + offset + FoliageWind(root.xz, time - layer.fade.z, amp * bend * scale);
    o.normal    = normal;
    o.occlusion = mix(0.35, 1.0, height01);
    o.visible   = true;
    return o;
}

// Card / mesh alpha (blades: always opaque).
float FoliageAlpha(vec2 uv)
{
    FoliageLayerBuffer layer = pc.layer;
    if (layer.info.x == FOLIAGE_CARDS)
        return layer.info.y == TERRAIN_NO_TEXTURE ? 1.0 : TerrainSample(layer.info.y, uv).a;
    if (layer.info.x == FOLIAGE_MESH) {
        const Material m = pc.frame.materials.m[pc.material];
        if ((m.flags & MATERIAL_ALPHA_MASK) == 0u)
            return 1.0;
        const float a = m.baseColorFactor.a * SampleTexture(pc.frame.textureTable.slot[m.baseColorTexture], m.samplerIndex, uv).a;
        return a >= m.alphaCutoff ? 1.0 : 0.0;
    }
    return 1.0;
}

#endif
