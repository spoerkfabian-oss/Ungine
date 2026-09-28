#version 460
#extension GL_EXT_buffer_reference : require
#include "mesh_common.glsl"

layout(location = 0) out vec2 outUV;

// Depth-only shadow rendering (the fragment stage exists only for alpha-masked materials).
// pc.cascade < 4: sun cascade; otherwise local light shadow view (pc.cascade - 4).
void main()
{
    Vertex    v        = pc.vertices.v[gl_VertexIndex];
    FrameData frame    = pc.frame;
    const mat4 viewProj = pc.cascade < 4u ? frame.cascadeViewProj[pc.cascade]
                                          : frame.shadowViews.v[pc.cascade - 4u].viewProj;
    outUV       = vec2(v.uvX, v.uvY);
    gl_Position = viewProj * (pc.draw.model * vec4(v.position, 1.0));
}
