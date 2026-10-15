#version 460
#extension GL_EXT_buffer_reference : require
// Mesh particles: the submesh's vertices (geometry pool) per particle instance.
#include "include/particle.glsl"

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUv;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out float outViewDistance;

mat3 AxisAngle(vec3 axis, float angle)
{
    const float s = sin(angle), c = cos(angle), t = 1.0 - c;
    const vec3  a = normalize(axis);
    return mat3(t * a.x * a.x + c,       t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y,
                t * a.x * a.y - s * a.z, t * a.y * a.y + c,       t * a.y * a.z + s * a.x,
                t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c);
}

void main()
{
    const Particle p = pc.data.p[pc.first + gl_InstanceIndex];
    const Vertex   v = pc.frame.vertices.v[gl_VertexIndex];
    mat3 rotation = mat3(1.0);
    if ((pc.flags & PARTICLE_ALIGN) != 0u) {
        if (dot(p.velocity, p.velocity) > 1.0e-10) { // +Z along the velocity, roll about it
            const vec3 z = normalize(p.velocity);
            const vec3 x = normalize(abs(z.y) < 0.99 ? cross(vec3(0, 1, 0), z) : cross(vec3(1, 0, 0), z));
            rotation     = mat3(x, cross(z, x), z) * AxisAngle(vec3(0, 0, 1), p.rotation);
        }
    } else if (dot(p.velocity, p.velocity) > 1.0e-10) {
        rotation = AxisAngle(p.velocity, p.rotation);
    }
    const vec3 position = p.position + rotation * (v.position * p.size);
    gl_Position     = pc.frame.viewProj * vec4(position, 1.0);
    outColor        = p.color;
    outUv           = vec2(v.uvX, v.uvY);
    outNormal       = rotation * normalize(v.normal / max(p.size, vec3(1.0e-6)));
    outViewDistance = -(pc.frame.view * vec4(position, 1.0)).z;
}
