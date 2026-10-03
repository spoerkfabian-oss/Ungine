#version 460
#extension GL_EXT_buffer_reference : require
#include "mesh_common.glsl"

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outTangent;
layout(location = 4) flat out uint outMaterial;
layout(location = 5) flat out uint outEntity;
layout(location = 6) flat out uint outLodFade;

// The depth prepass and the main pass (depth test EQUAL) must produce bit-identical depth.
invariant gl_Position;

void main()
{
    FrameData         frame = pc.frame;
    const uint        entry = pc.visible.v[gl_InstanceIndex];
    const GpuDraw     draw  = frame.draws.d[entry & VISIBLE_RECORD_MASK];
    const GpuInstance inst  = frame.instances.i[draw.instance];
    const Vertex      v     = frame.vertices.v[gl_VertexIndex]; // vertexOffset (pool) applied by the draw
    const mat4        skin  = SkinMatrix(inst, v);
    const vec4        local = skin * vec4(v.position, 1.0);
    const vec4        world = inst.model * local;

    outWorldPos = world.xyz;
    outNormal   = NormalMatrix(inst) * mat3(skin) * v.normal;
    outTangent  = vec4(mat3(inst.model) * mat3(skin) * v.tangent.xyz, v.tangent.w); // w == 0 passes through
    outUV       = vec2(v.uvX, v.uvY);
    outMaterial = frame.submeshes.s[draw.submesh].material;
    outEntity   = inst.entityId;
    outLodFade  = entry >> VISIBLE_LOD_SHIFT;
    gl_Position = frame.viewProj * world;
}
