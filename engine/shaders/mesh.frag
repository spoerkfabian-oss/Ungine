#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "bindless.glsl"
#include "mesh_common.glsl"
#include "pbr.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec4 outColor;

// Tangent frame from screen-space derivatives (Schueler, "Normal Mapping Without Precomputed
// Tangents"). Scaled by sign(det) so it is independent of screen handedness (flipped viewport).
mat3 CotangentFrame(vec3 N, vec3 p, vec2 uv)
{
    const vec3 dp1 = dFdx(p);
    const vec3 dp2 = dFdy(p);
    const vec2 duv1 = dFdx(uv);
    const vec2 duv2 = dFdy(uv);

    const vec3 dp2perp = cross(dp2, N);
    const vec3 dp1perp = cross(N, dp1);
    vec3       T       = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3       B       = dp2perp * duv1.y + dp1perp * duv2.y;

    const float lenSq = max(dot(T, T), dot(B, B));
    if (lenSq < 1e-20) // no UVs: any frame works, the normal map is flat anyway
        return BasisFromNormal(N);
    const float s = dot(N, cross(dp1, dp2)) < 0.0 ? -1.0 : 1.0;
    return mat3(T * (s * inversesqrt(lenSq)), B * (s * inversesqrt(lenSq)), N);
}

void main()
{
    const Material m    = pc.materials.m[pc.materialIndex];
    const vec4     base = m.baseColorFactor * SampleTexture(m.baseColorTexture, m.samplerIndex, inUV);

    if ((m.flags & MATERIAL_ALPHA_MASK) != 0u && base.a < m.alphaCutoff)
        discard;

    // --- Normal ---
    const vec3 ng = normalize(inNormal);
    mat3       tbn;
    if (abs(inTangent.w) > 0.5) {
        const vec3 t = normalize(inTangent.xyz - ng * dot(ng, inTangent.xyz));
        tbn          = mat3(t, cross(ng, t) * inTangent.w, ng);
    } else {
        tbn = CotangentFrame(ng, inWorldPos, inUV);
    }
    if (!gl_FrontFacing)
        tbn = -tbn; // back side of a double-sided surface: mirror the whole frame

    vec3 tn = SampleTexture(m.normalTexture, m.samplerIndex, inUV).xyz * 2.0 - 1.0;
    tn.xy *= m.normalScale;
    const vec3 N = normalize(tbn * tn);

    // --- Material ---
    const vec4  mr        = SampleTexture(m.metallicRoughnessTexture, m.samplerIndex, inUV); // G rough, B metal
    const float metallic  = clamp(m.metallic * mr.b, 0.0, 1.0);
    const float roughness = clamp(m.roughness * mr.g, 0.045, 1.0); // floor avoids aliasing sun highlights
    const float a         = roughness * roughness;
    const vec3  cDiff     = mix(base.rgb, vec3(0.0), metallic);
    const vec3  f0        = mix(vec3(0.04), base.rgb, metallic);

    FrameData       frame = pc.frame;
    const vec3      V     = normalize(frame.cameraPosition.xyz - inWorldPos);
    const float     NdotV = clamp(dot(N, V), 1e-4, 1.0);

    // --- Sun (analytic directional light) ---
    const vec3  L     = normalize(-frame.sunDirection.xyz);
    const vec3  H     = normalize(V + L);
    const float NdotL = clamp(dot(N, L), 0.0, 1.0);
    const float NdotH = clamp(dot(N, H), 0.0, 1.0);
    const float VdotH = clamp(dot(V, H), 0.0, 1.0);

    const vec3 F      = F_Schlick(f0, VdotH);
    const vec3 direct = ((1.0 - F) * cDiff / PI + F * (D_GGX(NdotH, a) * V_SmithGGXCorrelated(NdotV, NdotL, a))) *
                        frame.sunRadiance.rgb * NdotL;

    // --- Image-based lighting: split sum + multiple-scattering compensation (Fdez-Aguera 2019) ---
    const vec2  lut      = SampleTexture(frame.ibl.z, SAMPLER_LINEAR_CLAMP, vec2(NdotV, roughness)).rg;
    const vec3  Fr       = max(vec3(1.0 - roughness), f0) - f0;
    const vec3  kS       = f0 + Fr * pow(1.0 - NdotV, 5.0);
    const vec3  FssEss   = kS * lut.x + lut.y;
    const float Ems      = 1.0 - (lut.x + lut.y);
    const vec3  Favg     = f0 + (1.0 - f0) / 21.0;
    const vec3  FmsEms   = Ems * FssEss * Favg / (1.0 - Favg * Ems);
    const vec3  kD       = cDiff * (1.0 - (FssEss + FmsEms));

    const vec3  R          = reflect(-V, N);
    const float maxLod     = float(frame.ibl.w - 1u);
    const vec3  radiance   = SampleCube(frame.ibl.y, SAMPLER_LINEAR_CLAMP, R, roughness * maxLod).rgb;
    const vec3  irradiance = SampleCube(frame.ibl.x, SAMPLER_LINEAR_CLAMP, N, 0.0).rgb;

    const float occlusion = SampleTexture(m.occlusionTexture, m.samplerIndex, inUV).r;
    const float ao        = 1.0 + m.occlusionStrength * (occlusion - 1.0);
    const vec3  indirect = (FssEss * radiance + (FmsEms + kD) * irradiance) * ao * frame.sky.y;

    const vec3 emissive = m.emissiveFactor.rgb * SampleTexture(m.emissiveTexture, m.samplerIndex, inUV).rgb;
    outColor = vec4(direct + indirect + emissive, 1.0);
}
