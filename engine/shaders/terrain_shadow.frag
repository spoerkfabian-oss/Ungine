#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "terrain.glsl"

layout(location = 0) in vec2 inLocal;

void main()
{
    if (TerrainHole(pc.terrain, inLocal))
        discard;
}
