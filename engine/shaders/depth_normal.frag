#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "bindless.glsl"
#include "mesh_common.glsl"
#include "pbr.glsl"
#include "surface.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec4 outNormal; // view space, [-1, 1]

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
}
