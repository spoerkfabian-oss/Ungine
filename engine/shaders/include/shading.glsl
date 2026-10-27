#ifndef ENGINE_SHADING_GLSL
#define ENGINE_SHADING_GLSL
// Forward PBR lighting of an opaque or blended surface sample (meshes, terrain): decals (opaque),
// sun with cascaded shadows, clustered punctual lights with their shadows, IBL with screen-space
// reflections (opaque), ambient occlusion, fog. Fragment shaders only (gl_FragCoord, derivatives).
// Requires bindless.glsl + frame.glsl (via mesh_common.glsl or particle.glsl).
#include "decals.glsl"
#include "fog.glsl"
#include "pbr.glsl"
#include "shadow.glsl"

struct SurfaceSample {
    vec3  worldPos;
    vec3  geomNormal; // shadow offsets, decal projection (normal maps would drag them into the surface)
    vec3  N;          // shading normal
    vec3  base;
    float metallic;
    float roughness;
    vec3  emissive;
    float occlusion;  // material AO (indirect light only)
};

// Cook-Torrance (GGX, height-correlated Smith, Schlick) + Lambert, times N.L.
vec3 DirectBrdf(vec3 N, vec3 V, vec3 L, float NdotV, float NdotL, vec3 cDiff, vec3 f0, float a)
{
    const vec3  H     = normalize(V + L);
    const float NdotH = clamp(dot(N, H), 0.0, 1.0);
    const float VdotH = clamp(dot(V, H), 0.0, 1.0);
    const vec3  F     = F_Schlick(f0, VdotH);
    return ((1.0 - F) * cDiff / PI + F * (D_GGX(NdotH, a) * V_SmithGGXCorrelated(NdotV, NdotL, a))) * NdotL;
}

// opaque: decals, screen-space AO and reflections apply (they belong to the opaque surface).
vec3 ShadeSurface(FrameData frame, SurfaceSample s, bool opaque)
{
    const float viewDepth = -(frame.view * vec4(s.worldPos, 1.0)).z;
    vec3        base      = s.base;
    vec3        N         = s.N;
    float       roughness = s.roughness;
    float       metallic  = s.metallic;
    vec3        emissive  = s.emissive;
    if (opaque) // decals project onto opaque surfaces
        ApplyDecals(frame, s.worldPos, s.geomNormal, viewDepth, base, N, roughness, metallic, emissive);
    const float a     = roughness * roughness;
    const vec3  cDiff = mix(base, vec3(0.0), metallic);
    const vec3  f0    = mix(vec3(0.04), base, metallic);

    const vec3  V     = normalize(frame.cameraPosition.xyz - s.worldPos);
    const float NdotV = clamp(dot(N, V), 1e-4, 1.0);

    // --- Sun (analytic directional light) ---
    const vec3  L     = normalize(-frame.sunDirection.xyz);
    const float NdotL = clamp(dot(N, L), 0.0, 1.0);
    uint        cascade;
    const float shadow = NdotL > 0.0 ? SunShadow(frame, s.worldPos, s.geomNormal, viewDepth, gl_FragCoord.xy, cascade)
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
            const vec3     toLight    = light.position - s.worldPos;
            const float    distanceSq = dot(toLight, toLight);
            const vec3     Ll         = toLight * inversesqrt(max(distanceSq, 1e-12));
            const float    NdotLl     = dot(N, Ll);
            if (NdotLl <= 0.0)
                continue;
            float attenuation = DistanceAttenuation(distanceSq, light.range) * SpotAttenuation(-Ll, light);
            if (attenuation <= 0.0)
                continue;
            if (light.shadow != NO_SHADOW)
                attenuation *= LocalShadow(frame, light, s.worldPos, s.geomNormal, gl_FragCoord.xy);
            direct += DirectBrdf(N, V, Ll, NdotV, NdotLl, cDiff, f0, a) * light.color * attenuation;
        }
    }

    // --- Image-based lighting: split sum + multiple-scattering compensation (Fdez-Aguera 2019) ---
    const vec2  lut    = SampleTexture(frame.ibl.z, SAMPLER_LINEAR_CLAMP, vec2(NdotV, roughness)).rg;
    const vec3  Fr     = max(vec3(1.0 - roughness), f0) - f0;
    const vec3  kS     = f0 + Fr * pow(1.0 - NdotV, 5.0);
    const vec3  FssEss = kS * lut.x + lut.y;
    const float Ems    = 1.0 - (lut.x + lut.y);
    const vec3  Favg   = f0 + (1.0 - f0) / 21.0;
    const vec3  FmsEms = Ems * FssEss * Favg / (1.0 - Favg * Ems);
    const vec3  kD     = cDiff * (1.0 - (FssEss + FmsEms));

    const vec3  R        = reflect(-V, N);
    const float maxLod   = float(frame.ibl.w - 1u);
    vec3        radiance = SampleCube(frame.ibl.y, SAMPLER_LINEAR_CLAMP, R, roughness * maxLod).rgb;
    if (opaque && frame.ssrInfo.y != 0u) { // screen-space reflections; the IBL fills where rays miss
        const vec4 ssr = SampleTextureLod(frame.ssrInfo.x, SAMPLER_LINEAR_CLAMP, gl_FragCoord.xy * frame.ssrParams.zw, 0.0);
        radiance = mix(radiance, ssr.rgb, clamp(ssr.a * frame.ssrParams.x, 0.0, 1.0));
    }
    const vec3 irradiance = SampleCube(frame.ibl.x, SAMPLER_LINEAR_CLAMP, N, 0.0).rgb;

    // Ambient occlusion (indirect light only): material AO and screen-space GTAO combined by min.
    float ao = s.occlusion;
    if (opaque && frame.aoInfo.y != 0u)
        ao = min(ao, texelFetch(sampler2D(uTextures[nonuniformEXT(frame.aoInfo.x)], uSamplers[SAMPLER_NEAREST_CLAMP]),
                                ivec2(gl_FragCoord.xy), 0).r);
    // Specular occlusion from AO (Lagarde & de Rousiers 2014).
    const float specAO   = clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
    const vec3  indirect = (FssEss * radiance * specAO + (FmsEms + kD) * irradiance * ao) * frame.sky.y;

    vec3 color = direct + indirect + emissive;
    color      = ApplyFog(frame, color, gl_FragCoord.xy * frame.ssrParams.zw, s.worldPos);
    if (frame.shadowInfo.y != 0u && NdotL > 0.0 && cascade < 4u) { // debug: red, green, blue, yellow
        const vec3 tint[4] = vec3[](vec3(1.0, 0.3, 0.3), vec3(0.3, 1.0, 0.3), vec3(0.3, 0.3, 1.0), vec3(1.0, 1.0, 0.3));
        color *= tint[cascade];
    }
    return color;
}

#endif
