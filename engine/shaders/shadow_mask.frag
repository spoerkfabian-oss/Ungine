#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "bindless.glsl"
#include "mesh_common.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 1) flat in uint inMaterial;

// Alpha-tested casters (foliage, fences).
void main()
{
    const Material m     = pc.frame.materials.m[inMaterial];
    const float    alpha = m.baseColorFactor.a * SampleMaterial(m.baseColorTexture, m.samplerIndex, inUV).a;
    if (alpha < m.alphaCutoff)
        discard;
}
