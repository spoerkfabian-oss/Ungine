#version 460
#extension GL_EXT_buffer_reference : require
#include "mesh_common.glsl"

layout(location = 0) out vec2 outUV;
layout(location = 1) flat out uint outMaterial;

// Depth-only shadow rendering (the fragment stage exists only for alpha-masked materials).
// pc.cascade < 4: sun cascade; otherwise local light shadow view (pc.cascade - 4).
void main()
{
    FrameData         frame    = pc.frame;
    const GpuDraw     draw     = frame.draws.d[pc.visible.v[gl_InstanceIndex] & VISIBLE_RECORD_MASK];
    const GpuInstance inst     = frame.instances.i[draw.instance];
    const Vertex      v        = frame.vertices.v[gl_VertexIndex];
    const mat4        viewProj = pc.cascade < 4u ? frame.cascadeViewProj[pc.cascade]
                                                 : frame.shadowViews.v[pc.cascade - 4u].viewProj;
    outUV       = vec2(v.uvX, v.uvY);
    outMaterial = frame.submeshes.s[draw.submesh].material;
    gl_Position = viewProj * (inst.model * vec4(v.position, 1.0));
}
