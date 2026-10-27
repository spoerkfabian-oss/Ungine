#ifndef ENGINE_SSR_GLSL
#define ENGINE_SSR_GLSL
// Screen-space reflections: shared push constants of ssr_trace / ssr_resolve / ssr_blur.
// Mirrors SsrPush (SceneRenderer.cpp).

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer SsrDepthPyramid { float d[]; };

layout(push_constant) uniform SsrPush {
    FrameData       frame;
    SsrDepthPyramid pyramid;   // closest depth, level 0 = full resolution
    uvec2           fullSize;  // depth / normals / velocity
    uvec2           size;      // reflection targets (half resolution)
    uint            levels;    // pyramid levels
    uint            depth;     // sampled slots
    uint            normals;   // view normal + roughness
    uint            velocity;
    uint            source;    // trace: last frame's color; resolve: raw trace; blur: input
    uint            history;   // resolve: last resolve (sampled)
    uint            target;    // storage
    uint            mode;      // trace: max steps; resolve: history valid; blur: 0 horizontal, 1 vertical
    float           maxRoughness;
    float           thickness;   // meters behind a surface that still count as a hit
    float           maxDistance; // meters
    float           feedback;    // resolve: history weight
} pc;

// Level sizes / offsets of the pyramid (level 0 = full resolution).
uvec2 SsrLevelSize(uint level)
{
    uvec2 s = pc.fullSize;
    for (uint i = 0u; i < level; ++i)
        s = max((s + 1u) / 2u, uvec2(1u));
    return s;
}

uint SsrLevelOffset(uint level)
{
    uint  offset = 0u;
    uvec2 s      = pc.fullSize;
    for (uint i = 0u; i < level; ++i) {
        offset += s.x * s.y;
        s = max((s + 1u) / 2u, uvec2(1u));
    }
    return offset;
}

#endif
