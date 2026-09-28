#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "bindless.glsl"
#include "mesh_common.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec4 outColor;

// Phase 4 shading: Lambert + ambient. Full PBR (Cook-Torrance, IBL, normal maps) comes next.
void main()
{
    Material m    = pc.materials.m[pc.materialIndex];
    vec4     base = m.baseColorFactor * SampleTexture(m.baseColorTexture, m.samplerIndex, inUV);

    if ((m.flags & MATERIAL_ALPHA_MASK) != 0u && base.a < m.alphaCutoff)
        discard;

    vec3 N = normalize(inNormal);
    if (!gl_FrontFacing)
        N = -N; // double-sided materials

    vec3  L   = normalize(-pc.frame.sunDirection.xyz);
    float NdL = max(dot(N, L), 0.0);
    float ao  = SampleTexture(m.occlusionTexture, m.samplerIndex, inUV).r;

    vec3 color    = base.rgb * (pc.frame.ambient.rgb * ao + pc.frame.sunColor.rgb * NdL);
    vec3 emissive = m.emissiveFactor.rgb * SampleTexture(m.emissiveTexture, m.samplerIndex, inUV).rgb;
    outColor      = vec4(color + emissive, 1.0);
}
