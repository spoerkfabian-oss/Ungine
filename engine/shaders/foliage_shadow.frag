#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "include/foliage.glsl"

layout(location = 0) in vec2 inUv;

void main()
{
    if (FoliageAlpha(inUv) < 0.5)
        discard;
}
