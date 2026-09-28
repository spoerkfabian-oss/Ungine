#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "bindless.glsl"
#include "mesh_common.glsl"
#include "pbr.glsl"
#include "shadow.glsl"
#include "surface.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec4 outColor;

// Cook-Torrance (GGX, height-correlated Smith, Schlick) + Lambert, times N.L.
vec3 DirectBrdf(vec3 N, vec3 V, vec3 L, float NdotV, float NdotL, vec3 cDiff, vec3 f0, float a)
{
    const vec3  H     = normalize(V + L);
    const float NdotH = clamp(dot(N, H), 0.0, 1.0);
    const float VdotH = clamp(dot(V, H), 0.0, 1.0);
    const vec3  F     = F_Schlick(f0, VdotH);
    return ((1.0 - F) * cDiff / PI + F * (D_GGX(NdotH, a) * V_SmithGGXCorrelated(NdotV, NdotL, a))) * NdotL;
}

void main()
{
    const Material m    = pc.materials.m[pc.materialIndex];
    const vec4     base = m.baseColorFactor * SampleTexture(m.baseColorTexture, m.samplerIndex, inUV);

    if ((m.flags & MATERIAL_ALPHA_MASK) != 0u && base.a < m.alphaCutoff)
        discard;

    // --- Normal ---
    const mat3 tbn = SurfaceFrame(inNormal, inTangent, inWorldPos, inUV);
    const vec3 N   = PerturbedNormal(m, tbn, inUV);

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
    const float NdotL = clamp(dot(N, L), 0.0, 1.0);

    // Shadows use the geometric normal (normal maps would drag the offset into the surface).
    const float viewDepth = -(frame.view * vec4(inWorldPos, 1.0)).z;
    uint        cascade;
    const float shadow = NdotL > 0.0 ? SunShadow(frame, inWorldPos, tbn[2], viewDepth, gl_FragCoord.xy, cascade)
                                     : 0.0;

    vec3 direct = NdotL > 0.0 ? DirectBrdf(N, V, L, NdotV, NdotL, cDiff, f0, a) * frame.sunRadiance.rgb * shadow
                              : vec3(0.0);

    // --- Punctual lights of this fragment's cluster ---
    if (frame.lightInfo.x > 0u) {
        const uint cluster = ClusterIndex(gl_FragCoord.xy, viewDepth, frame.clusterParams.zw, frame.clusterParams.x,
                                          frame.clusterParams.y);
        ClusterBuffer clusters = frame.clusters;
        LightBuffer   lights   = frame.lights;
        const uint    count    = min(clusters.counts[cluster], CLUSTER_MAX_LIGHTS);
        const uint    first    = cluster * CLUSTER_MAX_LIGHTS;
        for (uint i = 0u; i < count; ++i) {
            const GpuLight light      = lights.l[clusters.indices[first + i]];
            const vec3     toLight    = light.position - inWorldPos;
            const float    distanceSq = dot(toLight, toLight);
            const vec3     Ll         = toLight * inversesqrt(max(distanceSq, 1e-12));
            const float    NdotLl     = dot(N, Ll);
            if (NdotLl <= 0.0)
                continue;
            const float attenuation = DistanceAttenuation(distanceSq, light.range) * SpotAttenuation(-Ll, light);
            if (attenuation > 0.0)
                direct += DirectBrdf(N, V, Ll, NdotV, NdotLl, cDiff, f0, a) * light.color * attenuation;
        }
    }

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

    // Ambient occlusion (indirect light only): material AO and screen-space GTAO combined by min.
    const float occlusion = SampleTexture(m.occlusionTexture, m.samplerIndex, inUV).r;
    float       ao        = 1.0 + m.occlusionStrength * (occlusion - 1.0);
    if (frame.aoInfo.y != 0u)
        ao = min(ao, texelFetch(sampler2D(uTextures[nonuniformEXT(frame.aoInfo.x)], uSamplers[SAMPLER_NEAREST_CLAMP]),
                                ivec2(gl_FragCoord.xy), 0).r);
    // Specular occlusion from AO (Lagarde & de Rousiers 2014).
    const float specAO   = clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
    const vec3  indirect = (FssEss * radiance * specAO + (FmsEms + kD) * irradiance * ao) * frame.sky.y;

    const vec3 emissive = m.emissiveFactor.rgb * SampleTexture(m.emissiveTexture, m.samplerIndex, inUV).rgb;
    vec3       color    = direct + indirect + emissive;

    if (frame.shadowInfo.y != 0u && NdotL > 0.0 && cascade < 4u) { // debug: red, green, blue, yellow
        const vec3 tint[4] = vec3[](vec3(1.0, 0.3, 0.3), vec3(0.3, 1.0, 0.3), vec3(0.3, 0.3, 1.0), vec3(1.0, 1.0, 0.3));
        color *= tint[cascade];
    }
    outColor = vec4(color, 1.0);
}
