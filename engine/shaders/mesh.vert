#version 460
#extension GL_EXT_buffer_reference : require
#include "mesh_common.glsl"

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outTangent;

void main()
{
    Vertex v     = pc.vertices.v[gl_VertexIndex]; // vertexOffset already applied by DrawIndexed
    vec4   world = pc.draw.model * vec4(v.position, 1.0);

    outWorldPos = world.xyz;
    outNormal   = mat3(pc.draw.normalMatrix) * v.normal;
    outTangent  = vec4(mat3(pc.draw.model) * v.tangent.xyz, v.tangent.w); // w == 0 passes through
    outUV       = vec2(v.uvX, v.uvY);
    gl_Position = pc.frame.viewProj * world;
}
