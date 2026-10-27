#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "terrain.glsl"

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec2 outLocal;
layout(location = 2) out vec4 outCurrentClip;  // motion vectors (prepass)
layout(location = 3) out vec4 outPreviousClip;

// Prepass and lighting pass (depth EQUAL) must produce bit-identical depth.
invariant gl_Position;

void main()
{
    FrameData   frame = pc.frame;
    vec2        local;
    const vec3  world = TerrainVertex(pc.terrain, pc.nodes.n[gl_InstanceIndex], gl_VertexIndex, local);
    outWorldPos     = world;
    outLocal        = local;
    gl_Position     = frame.viewProj * vec4(world, 1.0);
    outCurrentClip  = frame.unjitteredViewProj * vec4(world, 1.0);
    outPreviousClip = frame.prevViewProj * vec4(world, 1.0); // static ground (LOD morphs ignored)
}
