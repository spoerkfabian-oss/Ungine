#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "bindless.glsl"
#include "mesh_common.glsl"

layout(location = 0) in vec2 inUV;

// Alpha-tested casters (foliage, fences).
void main()
{
    const Material m     = pc.materials.m[pc.materialIndex];
    const float    alpha = m.baseColorFactor.a * SampleTexture(m.baseColorTexture, m.samplerIndex, inUV).a;
    if (alpha < m.alphaCutoff)
        discard;
}
