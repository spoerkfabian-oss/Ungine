#ifndef ENGINE_DEPTH_NORMAL_GLSL
#define ENGINE_DEPTH_NORMAL_GLSL
// Body of depth_normal.frag / depth_normal_id.frag (the latter defines WRITE_ENTITY_ID).
#include "bindless.glsl"
#include "mesh_common.glsl"
#include "pbr.glsl"
#include "surface.glsl"
#ifdef CUSTOM_MATERIAL
#include "material_common.glsl"
#include "material.generated.glsl"
#include "material_surface.glsl"
layout(location = 9) flat in vec3 inObjectPosition;
#endif

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) flat in uint inMaterial;
layout(location = 5) flat in uint inEntity;
layout(location = 6) flat in uint inLodFade;
layout(location = 7) in vec4 inCurrentClip;
layout(location = 8) in vec4 inPreviousClip;

layout(location = 0) out vec4 outNormal;   // xyz: view space normal [-1, 1], w: roughness (SSR)
layout(location = 1) out vec2 outVelocity; // UV motion since last frame (TAA, SSR reprojection)
#ifdef WRITE_ENTITY_ID
layout(location = 2) out uint outEntity; // entity slot index + 1, 0 = nothing (editor picking)
#endif

// Depth + normal prepass: everything screen-space effects need before the lighting pass.
void main()
{
    if (LodFadeDiscard(inLodFade, gl_FragCoord.xy)) // LOD cross-fade (the lighting pass drops the same pixels)
        discard;
#ifdef CUSTOM_MATERIAL
    const mat3           tbn = SurfaceFrame(inNormal, inTangent, inWorldPos, inUV);
    const MaterialResult r   = EvaluatePixelMaterial(inWorldPos, tbn[2], inUV, inObjectPosition, inMaterial);
    if (MaterialMaskDiscard(r, inMaterial))
        discard;
    const vec3  N         = MaterialShadingNormal(r, tbn);
    const float roughness = clamp(r.roughness, 0.045, 1.0);
#else
    const Material m = pc.frame.materials.m[inMaterial];
    if ((m.flags & MATERIAL_ALPHA_MASK) != 0u) {
        const float alpha = m.baseColorFactor.a * SampleMaterial(m.baseColorTexture, m.samplerIndex, inUV).a;
        if (alpha < m.alphaCutoff)
            discard;
    }
    const vec3 N = PerturbedNormal(m, SurfaceFrame(inNormal, inTangent, inWorldPos, inUV), inUV);
    const float roughness = clamp(m.roughness * SampleMaterial(m.metallicRoughnessTexture, m.samplerIndex, inUV).g,
                                  0.045, 1.0);
#endif
    outNormal    = vec4(normalize(mat3(pc.frame.view) * N), roughness);
    outVelocity  = MotionVector(inCurrentClip, inPreviousClip);
#ifdef WRITE_ENTITY_ID
    outEntity = inEntity;
#endif
}

#endif
