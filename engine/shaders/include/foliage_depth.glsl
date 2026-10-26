#ifndef ENGINE_FOLIAGE_DEPTH_GLSL
#define ENGINE_FOLIAGE_DEPTH_GLSL
// Body of foliage_depth.frag / foliage_depth_id.frag (the latter defines FOLIAGE_ENTITY_ID).
#include "foliage.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inUv;
layout(location = 4) in vec4 inCurrentClip;
layout(location = 5) in vec4 inPreviousClip;

layout(location = 0) out vec4 outNormal;   // view space normal, w: roughness (SSR)
layout(location = 1) out vec2 outVelocity;
#ifdef FOLIAGE_ENTITY_ID
layout(location = 2) out uint outEntity;   // the terrain's entity
#endif

void main()
{
    if (FoliageAlpha(inUv) < 0.5)
        discard;
    vec3 N = normalize(inNormal);
    if (!gl_FrontFacing && pc.layer.info.x == FOLIAGE_MESH)
        N = -N;
    outNormal   = vec4(normalize(mat3(pc.frame.view) * N), 0.85);
    outVelocity = TerrainMotionVector(inCurrentClip, inPreviousClip);
#ifdef FOLIAGE_ENTITY_ID
    outEntity = pc.layer.info.w;
#endif
}

#endif
