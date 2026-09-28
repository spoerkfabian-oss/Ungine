#ifndef ENGINE_DEPTH_NORMAL_GLSL
#define ENGINE_DEPTH_NORMAL_GLSL
// Body of depth_normal.frag / depth_normal_id.frag (the latter defines WRITE_ENTITY_ID).
#include "bindless.glsl"
#include "mesh_common.glsl"
#include "pbr.glsl"
#include "surface.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec4 outNormal; // view space, [-1, 1]
#ifdef WRITE_ENTITY_ID
layout(location = 1) out uint outEntity; // entity slot index + 1, 0 = nothing (editor picking)
#endif

// Depth + normal prepass: everything screen-space effects need before the lighting pass.
void main()
{
    const Material m = pc.materials.m[pc.materialIndex];
    if ((m.flags & MATERIAL_ALPHA_MASK) != 0u) {
        const float alpha = m.baseColorFactor.a * SampleTexture(m.baseColorTexture, m.samplerIndex, inUV).a;
        if (alpha < m.alphaCutoff)
            discard;
    }
    const vec3 N = PerturbedNormal(m, SurfaceFrame(inNormal, inTangent, inWorldPos, inUV), inUV);
    outNormal    = vec4(normalize(mat3(pc.frame.view) * N), 0.0);
#ifdef WRITE_ENTITY_ID
    outEntity = pc.draw.entityId;
#endif
}

#endif
