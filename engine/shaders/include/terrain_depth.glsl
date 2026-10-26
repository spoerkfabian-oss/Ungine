#ifndef ENGINE_TERRAIN_DEPTH_GLSL
#define ENGINE_TERRAIN_DEPTH_GLSL
// Body of terrain_depth.frag / terrain_depth_id.frag (the latter defines TERRAIN_ENTITY_ID):
// depth + normal prepass of the terrain.
#include "terrain.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec2 inLocal;
layout(location = 2) in vec4 inCurrentClip;
layout(location = 3) in vec4 inPreviousClip;

layout(location = 0) out vec4 outNormal;   // view space normal, w: roughness (SSR)
layout(location = 1) out vec2 outVelocity;
#ifdef TERRAIN_ENTITY_ID
layout(location = 2) out uint outEntity;
#endif

void main()
{
    TerrainBuffer t = pc.terrain;
    if (TerrainHole(t, inLocal))
        discard;
    const vec3 N = TerrainNormal(t, inLocal);
    // Roughness of the splat mix (layer constants; textures only in the lighting pass).
    const vec2 splatUv = inLocal / t.origin.w;
    const vec4 s0 = textureLod(sampler2D(uTextures[nonuniformEXT(t.textures.y)], uSamplers[SAMPLER_LINEAR_CLAMP]), splatUv, 0.0);
    const vec4 s1 = textureLod(sampler2D(uTextures[nonuniformEXT(t.textures.z)], uSamplers[SAMPLER_LINEAR_CLAMP]), splatUv, 0.0);
    const float w[8] = float[](s0.x, s0.y, s0.z, s0.w, s1.x, s1.y, s1.z, s1.w);
    float roughness = 0.0, total = 0.0;
    for (uint i = 0u; i < t.info.x; ++i) {
        roughness += w[i] * t.layers[i].material.x;
        total += w[i];
    }
    roughness  = total > 1e-4 ? roughness / total : t.layers[0].material.x;
    outNormal   = vec4(normalize(mat3(pc.frame.view) * N), clamp(roughness, 0.045, 1.0));
    outVelocity = TerrainMotionVector(inCurrentClip, inPreviousClip);
#ifdef TERRAIN_ENTITY_ID
    outEntity = t.info.y;
#endif
}

#endif
