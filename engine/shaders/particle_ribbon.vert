#version 460
#extension GL_EXT_buffer_reference : require
// Ribbons: a camera-facing strip through consecutive points of the same ribbon (6 vertices per
// segment; segments between two ribbons collapse).
#define PARTICLE_DATA RibbonBuffer
#include "include/particle.glsl"

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUv;
layout(location = 2) out float outViewDistance;
layout(location = 3) out vec2 outLocal; // y: -1..1 across the ribbon

// Half-width offset at point i, perpendicular to the ribbon and the view direction.
vec3 Side(uint i)
{
    const RibbonPoint point = pc.data.p[i];
    const uint end  = pc.first + pc.count;
    const uint prev = (i > pc.first && pc.data.p[i - 1].ribbon == point.ribbon) ? i - 1 : i;
    const uint next = (i + 1 < end && pc.data.p[i + 1].ribbon == point.ribbon) ? i + 1 : i;
    vec3 tangent = pc.data.p[next].position - pc.data.p[prev].position;
    if (dot(tangent, tangent) < 1.0e-12)
        tangent = vec3(0, 1, 0);
    vec3 side = cross(tangent, pc.frame.cameraPosition.xyz - point.position);
    if (dot(side, side) < 1.0e-12)
        side = cross(tangent, vec3(0, 0, 1));
    return normalize(side) * point.width * 0.5;
}

void main()
{
    const uint segment = pc.first + gl_VertexIndex / 6;
    const uint corner  = gl_VertexIndex % 6;
    const RibbonPoint a = pc.data.p[segment];
    const RibbonPoint b = pc.data.p[segment + 1];
    if (a.ribbon != b.ribbon) { // gap between two ribbons: degenerate, clipped
        gl_Position     = vec4(0.0, 0.0, -1.0, 1.0);
        outColor        = vec4(0.0);
        outUv           = vec2(0.0);
        outViewDistance = 0.0;
        outLocal        = vec2(0.0);
        return;
    }
    // Quad a-left, b-left, b-right / a-left, b-right, a-right.
    const bool atB  = corner == 1 || corner == 2 || corner == 4;
    const bool left = corner == 0 || corner == 1 || corner == 3;
    const uint index = atB ? segment + 1 : segment;
    const RibbonPoint point = atB ? b : a;
    const vec3 position = point.position + (left ? -Side(index) : Side(index));
    gl_Position     = pc.frame.viewProj * vec4(position, 1.0);
    outColor        = point.color;
    outUv           = vec2(point.u, left ? 0.0 : 1.0);
    outViewDistance = -(pc.frame.view * vec4(position, 1.0)).z;
    outLocal        = vec2(0.0, left ? -1.0 : 1.0);
}
