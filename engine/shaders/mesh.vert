#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "mesh_common.glsl"
#ifdef CUSTOM_MATERIAL // generated material: world position offset (also last frame's for motion vectors)
#include "bindless.glsl"
#include "material_common.glsl"
#include "material.generated.glsl"
layout(location = 9) flat out vec3 outObjectPosition;
#endif

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outTangent;
layout(location = 4) flat out uint outMaterial;
layout(location = 5) flat out uint outEntity;
layout(location = 6) flat out uint outLodFade;
layout(location = 7) out vec4 outCurrentClip;  // motion vectors (prepass): without jitter
layout(location = 8) out vec4 outPreviousClip;

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
    vec4              world = inst.model * local;

    outNormal   = NormalMatrix(inst) * mat3(skin) * v.normal;
    // Last frame's position: previous model matrix and skin palette, previous camera.
    vec4 previousWorld = inst.prevModel * (PrevSkinMatrix(inst, v, skin) * vec4(v.position, 1.0));
#ifdef CUSTOM_MATERIAL
    outObjectPosition = inst.model[3].xyz;
#ifdef MATERIAL_HAS_WPO
    const vec2 uv = vec2(v.uvX, v.uvY);
    world.xyz += EvaluateMaterialOffset(MaterialContextOf(world.xyz, outNormal, uv, inst.model[3].xyz,
                                                          frame.materialTime.x, draw.params));
    previousWorld.xyz += EvaluateMaterialOffset(MaterialContextOf(previousWorld.xyz, outNormal, uv, inst.prevModel[3].xyz,
                                                                  frame.materialTime.y, draw.params));
#endif
#endif
    outWorldPos = world.xyz;
    outTangent  = vec4(mat3(inst.model) * mat3(skin) * v.tangent.xyz, v.tangent.w); // w == 0 passes through
    outUV       = vec2(v.uvX, v.uvY);
#ifdef CUSTOM_MATERIAL
    outMaterial = draw.params;
#else
    outMaterial = frame.submeshes.s[draw.submesh].material;
#endif
    outEntity   = inst.entityId;
    outLodFade  = entry >> VISIBLE_LOD_SHIFT;
    gl_Position = frame.viewProj * world;
    outCurrentClip  = frame.unjitteredViewProj * world;
    outPreviousClip = frame.prevViewProj * previousWorld;
}
