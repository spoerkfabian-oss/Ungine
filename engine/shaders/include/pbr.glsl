#ifndef ENGINE_PBR_GLSL
#define ENGINE_PBR_GLSL
// glTF 2.0 metallic-roughness BRDF: Lambert diffuse + GGX / height-correlated Smith / Schlick.
// `a` is alpha = perceptualRoughness^2 throughout.

const float PI = 3.14159265359;

float D_GGX(float NdotH, float a)
{
    const float a2 = a * a;
    const float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Visibility term V = G / (4 NdotL NdotV).
float V_SmithGGXCorrelated(float NdotV, float NdotL, float a)
{
    const float a2  = a * a;
    const float ggv = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    const float ggl = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(ggv + ggl, 1e-5);
}

vec3 F_Schlick(vec3 f0, float VdotH)
{
    return f0 + (1.0 - f0) * pow(1.0 - VdotH, 5.0);
}

// --- Sampling helpers (IBL precomputation) ---

float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint n)
{
    return vec2(float(i) / float(n), RadicalInverse_VdC(i));
}

// Orthonormal basis with N as z axis.
mat3 BasisFromNormal(vec3 N)
{
    const vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    const vec3 T  = normalize(cross(up, N));
    return mat3(T, cross(N, T), N);
}

// Half vector distributed by D_GGX * NdotH (pdf of L: D * NdotH / (4 VdotH)).
vec3 ImportanceSampleGGX(vec2 Xi, mat3 basis, float a)
{
    const float phi      = 2.0 * PI * Xi.x;
    const float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    return basis * vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

#endif
