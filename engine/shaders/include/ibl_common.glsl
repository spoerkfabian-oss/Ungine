#ifndef ENGINE_IBL_COMMON_GLSL
#define ENGINE_IBL_COMMON_GLSL
// Shared push block of the IBL compute shaders. Mirrors IblPush in Environment.cpp.

layout(push_constant) uniform IblPush {
    uint  dst;       // storage slot (image2DArray for cubes, image2D for the LUT)
    uint  size;      // destination face size in texels
    uint  src;       // source cube slot
    uint  srcSize;   // source cube mip 0 size
    float roughness; // prefilter
    uint  sampleCount;
    uint  _pad0;
    uint  _pad1;
    vec4  toSun;       // sky generation
    vec4  sunRadiance;
    vec4  skyParams;   // x: sky intensity
} pc;

// Direction through texel `texel` of cube face `face` (Vulkan face order +X,-X,+Y,-Y,+Z,-Z).
vec3 CubeDirection(uint face, uvec2 texel, uint size)
{
    const vec2 uv = (vec2(texel) + 0.5) / float(size) * 2.0 - 1.0;
    switch (face) {
    case 0u: return normalize(vec3(1.0, -uv.y, -uv.x));
    case 1u: return normalize(vec3(-1.0, -uv.y, uv.x));
    case 2u: return normalize(vec3(uv.x, 1.0, uv.y));
    case 3u: return normalize(vec3(uv.x, -1.0, -uv.y));
    case 4u: return normalize(vec3(uv.x, -uv.y, 1.0));
    default: return normalize(vec3(-uv.x, -uv.y, -1.0));
    }
}

// Filtered importance sampling (GPU Gems 3, ch. 20): read a coarser mip for low-pdf samples.
float SourceLod(float pdf, uint sampleCount, uint srcSize)
{
    const float solidAngleSample = 1.0 / (float(sampleCount) * max(pdf, 1e-6));
    const float solidAngleTexel  = 4.0 * 3.14159265359 / (6.0 * float(srcSize) * float(srcSize));
    return max(0.5 * log2(solidAngleSample / solidAngleTexel) + 1.0, 0.0);
}

#endif
