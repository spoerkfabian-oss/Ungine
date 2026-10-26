#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "terrain.glsl"

layout(location = 0) out vec2 outLocal;

// Sun cascade depth (same CDLOD vertices as the camera: LODs follow the camera distance).
void main()
{
    vec2       local;
    const vec3 world = TerrainVertex(pc.terrain, pc.nodes.n[gl_InstanceIndex], gl_VertexIndex, local);
    outLocal    = local;
    gl_Position = pc.frame.cascadeViewProj[pc.cascade] * vec4(world, 1.0);
}
