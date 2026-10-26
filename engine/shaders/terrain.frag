#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
// Terrain lighting pass (depth EQUAL after the prepass): splat-mapped layers, triplanar on steep
// slopes, the shared forward lighting (decals, shadows, lights, IBL, SSR, AO, fog).
#include "terrain.glsl"
#include "shading.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inLocal;
layout(location = 2) in vec4 inCurrentClip;
layout(location = 3) in vec4 inPreviousClip;

layout(location = 0) out vec4 outColor;

// Tangent-space normal sample -> world, for a projection with axes a (u), b (v), n (normal).
vec3 ProjectedNormal(uint entry, vec2 uv, float strength, vec3 a, vec3 b, vec3 n)
{
    if (entry == TERRAIN_NO_TEXTURE)
        return n;
    vec2 xy = TerrainSample(entry, uv).xy * 2.0 - 1.0; // BC5: z reconstructed
    xy *= strength;
    const float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
    return normalize(a * xy.x + b * xy.y + n * z);
}

void main()
{
    TerrainBuffer t = pc.terrain;
    if (TerrainHole(t, inLocal))
        discard;
    const vec3 geom = TerrainNormal(t, inLocal);

    // Splat weights (two RGBA8 maps), normalized.
    const vec2 splatUv = inLocal / t.origin.w;
    const vec4 s0 = SampleTexture(t.textures.y, SAMPLER_LINEAR_CLAMP, splatUv);
    const vec4 s1 = SampleTexture(t.textures.z, SAMPLER_LINEAR_CLAMP, splatUv);
    float      w[8] = float[](s0.x, s0.y, s0.z, s0.w, s1.x, s1.y, s1.z, s1.w);
    float      total = 0.0;
    for (uint i = 0u; i < t.info.x; ++i)
        total += w[i];
    if (total < 1e-4) {
        w[0]  = 1.0;
        total = 1.0;
    }

    // Triplanar on steep ground: side projections fade in below cos(triplanar start).
    const float side    = 1.0 - smoothstep(t.params.w - 0.15, t.params.w, geom.y);
    vec3        tri     = pow(abs(geom), vec3(4.0));
    tri                /= max(tri.x + tri.y + tri.z, 1e-4);
    const vec3  axisW   = side > 0.0 ? mix(vec3(0.0, 1.0, 0.0), tri, side) : vec3(0.0, 1.0, 0.0);
    const vec3  p       = inWorldPos;

    vec3  base = vec3(0.0), N = vec3(0.0);
    float roughness = 0.0, metallic = 0.0, occlusion = 0.0;
    for (uint i = 0u; i < t.info.x; ++i) {
        const float wi = w[i] / total;
        if (wi < 0.003)
            continue;
        const GpuTerrainLayer layer = t.layers[i];
        const float           scale = layer.tint.w;
        // Projections: top (x, z), side X (z, y), side Z (x, y).
        const vec2 uvs[3] = vec2[](p.xz * scale, p.zy * scale, p.xy * scale);
        const vec3 axes   = axisW;
        vec4       albedo = vec4(0.0), orm = vec4(0.0);
        vec3       n      = vec3(0.0);
        for (int k = 0; k < 3; ++k) {
            const float pw = k == 0 ? axes.y : (k == 1 ? axes.x : axes.z);
            if (pw < 0.01)
                continue;
            const vec2 uv = uvs[k];
            albedo += pw * (layer.textures.x != TERRAIN_NO_TEXTURE ? TerrainSample(layer.textures.x, uv) : vec4(1.0));
            orm    += pw * (layer.textures.z != TERRAIN_NO_TEXTURE ? TerrainSample(layer.textures.z, uv) : vec4(1.0));
            const float sgn = k == 0 ? sign(geom.y) : (k == 1 ? sign(geom.x) : sign(geom.z));
            const vec3  an  = k == 0 ? vec3(0.0, sgn, 0.0) : (k == 1 ? vec3(sgn, 0.0, 0.0) : vec3(0.0, 0.0, sgn));
            const vec3  aa  = k == 0 ? vec3(1.0, 0.0, 0.0) : (k == 1 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0));
            const vec3  ab  = k == 0 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
            n += pw * ProjectedNormal(layer.textures.y, uv, layer.material.z, aa, ab, an);
        }
        base += wi * albedo.rgb * layer.tint.rgb;
        // Detail normals bend the height-field normal: the top projection was built around +Y.
        N += wi * (n + (geom - vec3(0.0, 1.0, 0.0)) * axes.y);
        roughness += wi * layer.material.x * orm.g;
        metallic  += wi * layer.material.y * orm.b;
        occlusion += wi * orm.r;
    }

    SurfaceSample surface;
    surface.worldPos   = inWorldPos;
    surface.geomNormal = geom;
    surface.N          = normalize(dot(N, N) > 1e-6 ? N : geom);
    surface.base       = base;
    surface.metallic   = clamp(metallic, 0.0, 1.0);
    surface.roughness  = clamp(roughness, 0.045, 1.0);
    surface.emissive   = vec3(0.0);
    surface.occlusion  = clamp(occlusion, 0.0, 1.0);
    outColor = vec4(ShadeSurface(pc.frame, surface, true), 1.0);
}
