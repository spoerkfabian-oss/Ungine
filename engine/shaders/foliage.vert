#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "include/foliage.glsl"

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec4 outColor; // rgb, a: occlusion
layout(location = 3) out vec2 outUv;
layout(location = 4) out vec4 outCurrentClip;
layout(location = 5) out vec4 outPreviousClip;

// Prepass and lighting pass (depth EQUAL) must produce bit-identical depth.
invariant gl_Position;

void main()
{
    const FoliageVertex v = FoliageVertexOf(gl_InstanceIndex, gl_VertexIndex);
    if (!v.visible) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0); // whole instance collapses: zero-area triangles
        return;
    }
    outWorldPos     = v.world;
    outNormal       = v.normal;
    outColor        = vec4(v.color, v.occlusion);
    outUv           = v.uv;
    gl_Position     = pc.frame.viewProj * vec4(v.world, 1.0);
    outCurrentClip  = pc.frame.unjitteredViewProj * vec4(v.world, 1.0);
    outPreviousClip = pc.frame.prevViewProj * vec4(v.previous, 1.0);
}
