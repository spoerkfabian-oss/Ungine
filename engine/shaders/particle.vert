#version 460
#extension GL_EXT_buffer_reference : require
// Sprite particles: one camera-, velocity- or axis-facing quad per instance (6 vertices).
#include "include/particle.glsl"

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUv;
layout(location = 2) out float outViewDistance;
layout(location = 3) out vec2 outLocal; // quad corner (-1..1): the untextured round sprite

void main()
{
    const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    const vec2 c = corners[gl_VertexIndex % 6];
    const Particle p = pc.data.p[pc.first + gl_InstanceIndex];

    const mat4 view   = pc.frame.view;
    vec3 right = vec3(view[0][0], view[1][0], view[2][0]);
    vec3 up    = vec3(view[0][1], view[1][1], view[2][1]);
    vec2 half_ = p.size.xy * 0.5;
    vec2 q     = c;
    const vec3 toCamera = pc.frame.cameraPosition.xyz - p.position;
    if (pc.facing == FACING_VELOCITY && dot(p.velocity, p.velocity) > 1.0e-8) {
        const float speed = length(p.velocity);
        up    = p.velocity / speed;
        vec3 side = cross(up, toCamera);
        if (dot(side, side) < 1.0e-10)
            side = right;
        right = normalize(side);
        half_.y *= 1.0 + pc.stretch * speed;
    } else {
        if (pc.facing == FACING_AXIS) {
            const vec3 n = normalize(pc.axis.xyz);
            right = normalize(abs(n.y) < 0.99 ? cross(vec3(0, 1, 0), n) : cross(vec3(1, 0, 0), n));
            up    = cross(n, right);
        }
        const float s = sin(p.rotation), co = cos(p.rotation);
        q = vec2(c.x * co - c.y * s, c.x * s + c.y * co);
    }
    const vec3 position = p.position + right * (q.x * half_.x) + up * (q.y * half_.y);
    gl_Position     = pc.frame.viewProj * vec4(position, 1.0);
    outColor        = p.color;
    outUv           = FlipbookUv(vec2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5), p.frame);
    outViewDistance = -(view * vec4(position, 1.0)).z;
    outLocal        = c;
}
