#ifndef ENGINE_DECALS_GLSL
#define ENGINE_DECALS_GLSL
// Clustered decals, applied to the surface before lighting (mesh_shading.glsl). Fragment shaders
// only (derivatives of the world position, taken before any divergent branch).
// Requires bindless.glsl, mesh_common.glsl.

vec4 SampleDecal(uint entry, vec2 uv, vec2 dx, vec2 dy)
{
    const uint slot = pc.frame.textureTable.slot[entry];
    return textureGrad(sampler2D(uTextures[nonuniformEXT(slot)], uSamplers[SAMPLER_LINEAR_CLAMP]), uv, dx, dy);
}

// Blends every decal of the fragment's cluster (in sort order) into the surface attributes.
void ApplyDecals(FrameData frame, vec3 worldPos, vec3 geometricNormal, float viewDepth, inout vec3 albedo,
                 inout vec3 N, inout float roughness, inout float metallic, inout vec3 emissive)
{
    const vec3 dpdx = dFdx(worldPos);
    const vec3 dpdy = dFdy(worldPos);
    if (frame.decalInfo.x == 0u)
        return;
    const uint cluster = ClusterIndex(gl_FragCoord.xy, viewDepth, frame.clusterParams.zw, frame.clusterParams.x,
                                      frame.clusterParams.y);
    DecalClusterBuffer clusters = frame.decalClusters;
    const uint count = min(clusters.counts[cluster], CLUSTER_MAX_DECALS);
    const uint first = cluster * CLUSTER_MAX_DECALS;
    for (uint i = 0u; i < count; ++i) {
        const GpuDecal d = frame.decals.d[clusters.indices[first + i]];
        const vec3     p = (d.worldToDecal * vec4(worldPos, 1.0)).xyz;
        if (any(greaterThan(abs(p), vec3(1.0))))
            continue;
        // Surfaces turned away from the projector fade out; so do the front and back of the box.
        const float facing = dot(geometricNormal, -d.axis.xyz);
        float       fade   = clamp((facing - d.fade.y) / max(d.fade.x - d.fade.y, 1e-4), 0.0, 1.0);
        if (d.params.w > 0.0)
            fade *= clamp((1.0 - abs(p.z)) / d.params.w, 0.0, 1.0);
        if (fade <= 0.0)
            continue;
        const vec2 uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); // image top at +y
        const mat3 m  = mat3(d.worldToDecal);
        const vec2 dx = (m * dpdx).xy * vec2(0.5, -0.5);
        const vec2 dy = (m * dpdy).xy * vec2(0.5, -0.5);

        vec4 albedoSample = vec4(1.0);
        if (d.textures.x != DECAL_NO_TEXTURE)
            albedoSample = SampleDecal(d.textures.x, uv, dx, dy);
        const float coverage = albedoSample.a * fade;
        if (coverage <= 0.0)
            continue;
        albedo = mix(albedo, albedoSample.rgb * d.color.rgb, clamp(coverage * d.color.a, 0.0, 1.0));

        if (d.textures.y != DECAL_NO_TEXTURE && d.emissive.w > 0.0) {
            const vec2 xy = SampleDecal(d.textures.y, uv, dx, dy).xy * 2.0 - 1.0;
            const vec3 tn = vec3(xy, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0)));
            // The decal's tangent frame laid onto the surface.
            const vec3 t  = normalize(d.tangent.xyz - N * dot(N, d.tangent.xyz) + 1e-6);
            const vec3 b  = cross(N, t);
            const vec3 dn = normalize(t * tn.x + b * tn.y + N * tn.z);
            N = normalize(mix(N, dn, clamp(coverage * d.emissive.w, 0.0, 1.0)));
        }
        if (d.params.z > 0.0) {
            vec2 material = d.params.xy; // roughness, metallic
            if (d.textures.z != DECAL_NO_TEXTURE)
                material *= SampleDecal(d.textures.z, uv, dx, dy).gb;
            const float w = clamp(coverage * d.params.z, 0.0, 1.0);
            roughness = mix(roughness, clamp(material.x, 0.045, 1.0), w);
            metallic  = mix(metallic, clamp(material.y, 0.0, 1.0), w);
        }
        vec3 glow = d.emissive.rgb * coverage;
        if (d.textures.w != DECAL_NO_TEXTURE)
            glow *= SampleDecal(d.textures.w, uv, dx, dy).rgb;
        emissive += glow;
    }
}

#endif
