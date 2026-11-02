#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "bindless.glsl"
#include "mesh_common.glsl"
#ifdef CUSTOM_MATERIAL
#include "material_common.glsl"
#include "material.generated.glsl"
layout(location = 2) in vec3 inWorldPos;
layout(location = 3) in vec3 inNormal;
layout(location = 4) flat in vec3 inObjectPosition;
#endif

layout(location = 0) in vec2 inUV;
layout(location = 1) flat in uint inMaterial;

// Alpha-tested casters (foliage, fences).
void main()
{
#ifdef CUSTOM_MATERIAL
    const MaterialResult r = EvaluateMaterial(MaterialContextOf(inWorldPos, inNormal, inUV, inObjectPosition,
                                                                pc.frame.materialTime.x, inMaterial));
    if (r.opacityMask < MaterialParam(inMaterial, MATERIAL_ALPHA_CUTOFF_SLOT).x)
        discard;
#else
    const Material m     = pc.frame.materials.m[inMaterial];
    const float    alpha = m.baseColorFactor.a * SampleMaterial(m.baseColorTexture, m.samplerIndex, inUV).a;
    if (alpha < m.alphaCutoff)
        discard;
#endif
}
