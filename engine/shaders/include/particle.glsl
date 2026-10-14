#ifndef ENGINE_PARTICLE_GLSL
#define ENGINE_PARTICLE_GLSL
// Requires: #extension GL_EXT_buffer_reference : require
// Particles (ParticleWorld render data). Mirrors Engine::GpuParticle / GpuRibbonPoint
// (ParticleWorld.h) and ParticlePush (ParticleRenderer.cpp).
#include "frame.glsl"

struct Particle {
    vec3  position; // world
    float rotation; // radians
    vec4  color;    // HDR, a = opacity
    vec3  velocity; // meshes not aligned to the velocity: spin axis
    float frame;    // sprite sheet frame
    vec3  size;
    float pad;
};

struct RibbonPoint {
    vec3  position;
    float width;
    vec4  color;
    float u;
    uint  ribbon;
    float pad0;
    float pad1;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ParticleBuffer { Particle p[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RibbonBuffer { RibbonPoint p[]; };

#define PARTICLE_SOFT         1u
#define PARTICLE_TEXTURED     2u
#define PARTICLE_ADDITIVE     4u
#define PARTICLE_SHADE        8u  // meshes
#define PARTICLE_ALIGN        16u // meshes: +Z along the velocity
#define PARTICLE_REPEAT       32u // ribbons tiled along their length

#define FACING_CAMERA   0u
#define FACING_VELOCITY 1u
#define FACING_AXIS     2u
#define FACING_RIBBON   3u // ribbon draws

#ifndef PARTICLE_DATA
#define PARTICLE_DATA ParticleBuffer
#endif

layout(push_constant) uniform ParticlePush {
    FrameData     frame;
    PARTICLE_DATA data;      // particles or ribbon points
    uint          first;     // first element of this batch
    uint          texture;   // texture table entry
    uint          columns;   // sprite sheet
    uint          rows;
    uint          facing;
    uint          flags;
    float         stretch;   // velocity facing: extra length per m/s
    float         softness;  // meters
    vec4          axis;      // xyz: axis facing normal, w: camera fade distance
    uint          depthTexture;
    uint          material;  // meshes: absolute material index
    uint          count;     // elements in this batch
    float         pad0;
} pc;

// View distance of the reverse-Z, infinite-far depth buffer value.
float ViewDistanceFromDepth(float depth)
{
    return pc.frame.proj[3][2] / max(depth, 1.0e-7);
}

vec2 FlipbookUv(vec2 uv, float frame)
{
    const uint  columns = max(pc.columns, 1u);
    const uint  rows    = max(pc.rows, 1u);
    const uint  index   = uint(max(frame, 0.0)) % (columns * rows);
    return (uv + vec2(float(index % columns), float(index / columns))) / vec2(float(columns), float(rows));
}

#endif
