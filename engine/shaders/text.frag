#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "include/bindless.glsl"

struct OverlayVertex { vec4 positionUv; uvec4 data; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer TextVertices { OverlayVertex v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer TextureTable { uint slot[]; };

layout(push_constant) uniform TextPush {
    TextVertices  vertices;
    vec2          screenSize;
    uint          srgbTarget;
    uint          pad;
    TextureTable  textureTable;
} pc;

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUV;
layout(location = 2) flat in uint inTextureEntry;
layout(location = 3) flat in uint inTextured;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = inColor;
    if (inTextured != 0u)
        outColor *= SampleTexture(pc.textureTable.slot[inTextureEntry], SAMPLER_LINEAR_CLAMP, inUV);
}
