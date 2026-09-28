#version 460
#extension GL_EXT_buffer_reference : require
#include "mesh_common.glsl"

layout(location = 0) out vec2 outUV;

// Depth-only cascade rendering (the fragment stage exists only for alpha-masked materials).
void main()
{
    Vertex v    = pc.vertices.v[gl_VertexIndex];
    outUV       = vec2(v.uvX, v.uvY);
    gl_Position = pc.frame.cascadeViewProj[pc.cascade] * (pc.draw.model * vec4(v.position, 1.0));
}
