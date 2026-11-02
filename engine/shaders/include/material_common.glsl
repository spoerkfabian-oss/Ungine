#ifndef ENGINE_MATERIAL_COMMON_GLSL
#define ENGINE_MATERIAL_COMMON_GLSL
// Custom (node graph) materials: the context the generated code reads, its result and the helpers
// it calls (GenerateMaterialCode, material.generated.glsl). Requires bindless.glsl and
// mesh_common.glsl (pc.frame). Every template defines CUSTOM_MATERIAL and MATERIAL_BLEND_MODE.

#define MATERIAL_OPAQUE      0
#define MATERIAL_MASKED      1
#define MATERIAL_TRANSLUCENT 2
#define MATERIAL_ADDITIVE    3

struct MaterialContext {
    vec3  worldPos;       // pixel: the (offset) surface; vertex: before the offset
    vec3  normal;         // world space vertex normal
    vec2  uv;
    vec3  cameraPosition;
    vec3  cameraVector;   // surface -> camera, normalized
    vec3  objectPosition; // instance origin
    float time;
    vec2  screenUV;       // pixel stage
    float pixelDepth;     // pixel stage: view depth
    uint  params;         // parameter block (vec4 index)
};

struct MaterialResult {
    vec3  baseColor;
    float metallic;
    float roughness;
    vec3  normal;      // tangent space
    vec3  emissive;
    float opacity;
    float opacityMask;
    float occlusion;
};

vec4 MaterialParam(uint params, uint slot)
{
    return pc.frame.materialParams.v[params + slot];
}

// Texture slots hold a texture table entry in x.
vec4 MaterialTexture(uint params, uint slot, vec2 uv)
{
    const uint entry = floatBitsToUint(MaterialParam(params, slot).x);
    return SampleTexture(pc.frame.textureTable.slot[entry], SAMPLER_LINEAR_REPEAT, uv);
}

vec4 MaterialTextureLod(uint params, uint slot, vec2 uv)
{
    const uint entry = floatBitsToUint(MaterialParam(params, slot).x);
    return SampleTextureLod(pc.frame.textureTable.slot[entry], SAMPLER_LINEAR_REPEAT, uv, 0.0);
}

// Normal maps keep XY (BC5); Z is reconstructed.
vec3 MaterialDecodeNormal(vec4 s)
{
    const vec2 xy = s.xy * 2.0 - 1.0;
    return vec3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0)));
}

#define MaterialSaturate(x)  clamp((x), 0.0, 1.0)
#define MaterialPow(a, b)    pow(max((a), 0.0), (b))
#define MaterialSqrt(x)      sqrt(max((x), 0.0))
#define MaterialNormalize(x) ((x) * inversesqrt(max(dot((x), (x)), 1e-12)))

float MaterialFresnel(float exponent, float baseReflect, vec3 normal, vec3 cameraVector)
{
    const float NdotV = clamp(dot(normalize(normal), cameraVector), 0.0, 1.0);
    return baseReflect + (1.0 - baseReflect) * pow(1.0 - NdotV, max(exponent, 0.0));
}

vec2 MaterialRotate(vec2 uv, vec2 center, float angle)
{
    const float c = cos(angle), s = sin(angle);
    const vec2  d = uv - center;
    return center + vec2(c * d.x - s * d.y, s * d.x + c * d.y);
}

vec3 MaterialDesaturate(vec3 color, float fraction)
{
    return mix(color, vec3(dot(color, vec3(0.2126, 0.7152, 0.0722))), fraction);
}

float MaterialHash(vec3 p)
{
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Value noise (0..1), smooth between lattice points.
float MaterialNoise(vec3 x)
{
    const vec3 i = floor(x);
    const vec3 f = fract(x);
    const vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(MaterialHash(i + vec3(0, 0, 0)), MaterialHash(i + vec3(1, 0, 0)), u.x),
                   mix(MaterialHash(i + vec3(0, 1, 0)), MaterialHash(i + vec3(1, 1, 0)), u.x), u.y),
               mix(mix(MaterialHash(i + vec3(0, 0, 1)), MaterialHash(i + vec3(1, 0, 1)), u.x),
                   mix(MaterialHash(i + vec3(0, 1, 1)), MaterialHash(i + vec3(1, 1, 1)), u.x), u.y),
               u.z);
}

float MaterialSphereMask(float distance, float radius, float hardness)
{
    const float h = clamp(hardness, 0.0, 0.9999);
    return clamp((1.0 - distance / max(radius, 1e-6)) / (1.0 - h), 0.0, 1.0);
}

MaterialContext MaterialContextOf(vec3 worldPos, vec3 normal, vec2 uv, vec3 objectPosition, float time, uint params)
{
    MaterialContext ctx;
    ctx.worldPos       = worldPos;
    ctx.normal         = normalize(normal);
    ctx.uv             = uv;
    ctx.cameraPosition = pc.frame.cameraPosition.xyz;
    ctx.cameraVector   = normalize(pc.frame.cameraPosition.xyz - worldPos);
    ctx.objectPosition = objectPosition;
    ctx.time           = time;
    ctx.screenUV       = vec2(0.0);
    ctx.pixelDepth     = 0.0;
    ctx.params         = params;
    return ctx;
}

#endif
