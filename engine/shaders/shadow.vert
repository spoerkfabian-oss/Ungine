#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "mesh_common.glsl"
#ifdef CUSTOM_MATERIAL // generated material: world position offset, opacity mask
#include "bindless.glsl"
#include "material_common.glsl"
#include "material.generated.glsl"
layout(location = 2) out vec3 outWorldPos;
layout(location = 3) out vec3 outNormal;
layout(location = 4) flat out vec3 outObjectPosition;
#endif

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
    const mat4        skin     = SkinMatrix(inst, v);
    outUV       = vec2(v.uvX, v.uvY);
    vec4 world  = inst.model * skin * vec4(v.position, 1.0);
#ifdef CUSTOM_MATERIAL
    outMaterial       = draw.params;
    outNormal         = NormalMatrix(inst) * mat3(skin) * v.normal;
    outObjectPosition = inst.model[3].xyz;
#ifdef MATERIAL_HAS_WPO
    world.xyz += EvaluateMaterialOffset(MaterialContextOf(world.xyz, outNormal, outUV, inst.model[3].xyz,
                                                          frame.materialTime.x, draw.params));
#endif
    outWorldPos = world.xyz;
#else
    outMaterial = frame.submeshes.s[draw.submesh].material;
#endif
    gl_Position = viewProj * world;
}
