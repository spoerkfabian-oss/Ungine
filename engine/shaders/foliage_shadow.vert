#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "include/foliage.glsl"

layout(location = 0) out vec2 outUv;

// Sun cascade depth: the same instances as the camera (thinning follows the camera distance).
void main()
{
    const FoliageVertex v = FoliageVertexOf(gl_InstanceIndex, gl_VertexIndex);
    outUv       = v.uv;
    gl_Position = v.visible ? pc.frame.cascadeViewProj[pc.cascade] * vec4(v.world, 1.0) : vec4(0.0, 0.0, 0.0, 1.0);
}
