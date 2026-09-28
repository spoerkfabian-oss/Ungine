#ifndef ENGINE_SHADOW_GLSL
#define ENGINE_SHADOW_GLSL
// Cascaded shadow map lookup. Requires bindless.glsl + frame.glsl.

const vec2 kPoisson16[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870),
    vec2(0.34495938, 0.29387760),   vec2(-0.91588581, 0.45771432),  vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845),  vec2(0.97484398, 0.75648379),   vec2(0.44323325, -0.97511554),
    vec2(0.53742981, -0.47373420),  vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507),  vec2(-0.81409955, 0.91437590),  vec2(0.19984126, 0.78641367),
    vec2(0.14383161, -0.14100790));

// Per-pixel rotation of the Poisson disk (Jimenez 2014): turns banding into fine noise.
float InterleavedGradientNoise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// 16 rotated Poisson taps, each a 2x2 hardware comparison. 1 = lit.
float SampleCascade(FrameData frame, uint cascade, vec3 worldPos, vec3 normal, float rotation)
{
    // Normal offset: move the receiver out of the surface by a few texels of this cascade.
    const vec3 offsetPos = worldPos + normal * (frame.shadowParams.y * frame.cascadeTexel[cascade]);
    const vec4 clip      = frame.cascadeViewProj[cascade] * vec4(offsetPos, 1.0);
    const vec3 coord     = vec3(clip.xy * 0.5 + 0.5, clip.z); // orthographic: w == 1

    const float texel  = 1.0 / float(frame.shadowInfo.x);
    const float radius = frame.shadowParams.z * texel;
    const mat2  rot    = mat2(cos(rotation), sin(rotation), -sin(rotation), cos(rotation));

    float lit = 0.0;
    for (int i = 0; i < 16; ++i) {
        const vec2 uv = coord.xy + rot * kPoisson16[i] * radius;
        lit += texture(sampler2DShadow(uTextures[nonuniformEXT(frame.shadowMaps[cascade])], uSamplers[SAMPLER_SHADOW]),
                       vec3(uv, coord.z));
    }
    return lit / 16.0;
}

// viewDepth: positive distance along the camera's view axis. Blends into the next cascade near
// each split and fades out at the end of the last one.
float SunShadow(FrameData frame, vec3 worldPos, vec3 normal, float viewDepth, vec2 pixel, out uint cascadeOut)
{
    cascadeOut = 0xFFFFFFFFu;
    const uint count = uint(frame.shadowParams.x);
    if (count == 0u)
        return 1.0;

    uint cascade = 0u;
    while (cascade < count && viewDepth > frame.cascadeSplits[cascade])
        ++cascade;
    if (cascade == count)
        return 1.0; // beyond the shadow distance
    cascadeOut = cascade;

    const float rotation = 6.2831853 * InterleavedGradientNoise(pixel);
    float       shadow   = SampleCascade(frame, cascade, worldPos, normal, rotation);

    const float sliceStart = cascade == 0u ? 0.0 : frame.cascadeSplits[cascade - 1u];
    const float sliceEnd   = frame.cascadeSplits[cascade];
    const float blendStart = sliceEnd - (sliceEnd - sliceStart) * frame.shadowParams.w;
    if (viewDepth > blendStart) {
        const float t    = smoothstep(blendStart, sliceEnd, viewDepth);
        const float next = cascade + 1u < count ? SampleCascade(frame, cascade + 1u, worldPos, normal, rotation) : 1.0;
        shadow           = mix(shadow, next, t);
    }
    return shadow;
}

// Local light shadow from the atlas: the light's view (point lights: the cube face of the major
// axis), normal offset scaled with the texel size at this distance, 16 rotated Poisson taps kept
// inside the tile. 1 = lit.
float LocalShadow(FrameData frame, GpuLight light, vec3 worldPos, vec3 normal, vec2 pixel)
{
    const vec3 d     = worldPos - light.position;
    uint       index = light.shadow;
    float      axisDistance;
    if (light.type == LIGHT_POINT) {
        const vec3 a = abs(d);
        if (a.x >= a.y && a.x >= a.z) {
            index += d.x > 0.0 ? 0u : 1u;
            axisDistance = a.x;
        } else if (a.y >= a.z) {
            index += d.y > 0.0 ? 2u : 3u;
            axisDistance = a.y;
        } else {
            index += d.z > 0.0 ? 4u : 5u;
            axisDistance = a.z;
        }
    } else {
        axisDistance = dot(d, light.direction);
    }

    ShadowViewBuffer    views = frame.shadowViews;
    const GpuShadowView view  = views.v[index];
    const vec3 offsetPos = worldPos + normal * (frame.localShadowParams.x * view.params.x * max(axisDistance, 0.0));
    const vec4 clip      = view.viewProj * vec4(offsetPos, 1.0);
    if (clip.w <= 0.0)
        return 1.0;
    const vec3 ndc = clip.xyz / clip.w;
    const vec2 uv  = ndc.xy * 0.5 + 0.5; // shadow views are rendered without the Y flip
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;

    const float texel    = 1.0 / float(frame.localShadowInfo.y);
    const vec2  lo       = view.rect.xy + 0.5 * texel;
    const vec2  hi       = view.rect.xy + view.rect.zw - 0.5 * texel;
    const vec2  center   = view.rect.xy + uv * view.rect.zw;
    const float radius   = frame.localShadowParams.y * texel;
    const float rotation = 6.2831853 * InterleavedGradientNoise(pixel);
    const mat2  rot      = mat2(cos(rotation), sin(rotation), -sin(rotation), cos(rotation));

    float lit = 0.0;
    for (int i = 0; i < 16; ++i) {
        const vec2 tap = clamp(center + rot * kPoisson16[i] * radius, lo, hi);
        lit += texture(sampler2DShadow(uTextures[nonuniformEXT(frame.localShadowInfo.x)], uSamplers[SAMPLER_SHADOW]),
                       vec3(tap, ndc.z));
    }
    return lit / 16.0;
}

#endif
