#ifndef ENGINE_SKY_GLSL
#define ENGINE_SKY_GLSL
// Cheap analytic sky shared by the skybox and the IBL cube generation, so reflections match
// what is visible. The sun disk is NOT part of it: the sun is an analytic light, and a
// sub-texel disk would only add fireflies to the prefiltered maps.
//   dir   : normalized world direction      toSun : normalized direction towards the sun
//   sunRadiance : sun color * intensity     intensity : overall sky scale

vec3 SkyRadiance(vec3 dir, vec3 toSun, vec3 sunRadiance, float intensity)
{
    const vec3 zenith  = vec3(0.10, 0.24, 0.58);
    const vec3 horizon = vec3(0.52, 0.62, 0.78);
    const vec3 ground  = vec3(0.10, 0.09, 0.08);

    // Low sun: warmer horizon and a darker sky overall.
    const float sunHeight = toSun.y;
    const vec3  warm      = mix(vec3(1.0, 0.55, 0.35), vec3(1.0), smoothstep(-0.05, 0.35, sunHeight));
    const float daylight  = clamp(sunHeight * 2.0 + 0.25, 0.03, 1.0);

    const float y   = dir.y;
    vec3        sky = mix(horizon * warm, zenith, sqrt(clamp(y, 0.0, 1.0)));
    vec3        col = mix(sky, ground, 1.0 - smoothstep(-0.06, 0.0, y)) * daylight;

    // Forward scattering glow around the sun (above the horizon only).
    const float mu = max(dot(dir, toSun), 0.0);
    col += sunRadiance * (0.015 * pow(mu, 8.0) + 0.06 * pow(mu, 96.0)) * smoothstep(-0.06, 0.02, y);
    return col * intensity;
}

// Visible sun disk for the skybox (angular radius ~0.27 degrees, slightly enlarged).
vec3 SunDisk(vec3 dir, vec3 toSun, vec3 sunRadiance)
{
    const float cosRadius = 0.99996;
    return sunRadiance * 40.0 * smoothstep(cosRadius, cosRadius + 0.00002, dot(dir, toSun));
}

#endif
