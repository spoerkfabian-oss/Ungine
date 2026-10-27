#ifndef ENGINE_FOG_GLSL
#define ENGINE_FOG_GLSL
// Height fog (analytic) + volumetric fog (froxel grid, fog_inject.comp / fog_integrate.comp).
// Requires bindless.glsl + frame.glsl.

#define FOG_TILE_SIZE 16.0 // pixels per froxel, mirrors Engine::kFogTileSize

// Henyey-Greenstein phase function (normalized over the sphere). cosTheta: between the light's
// travel direction and the direction towards the viewer.
float FogPhase(float cosTheta, float g)
{
    const float g2 = g * g;
    return (1.0 - g2) / (12.5663706 * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

vec3 FogForward(FrameData frame)
{
    return -vec3(frame.view[0][2], frame.view[1][2], frame.view[2][2]);
}

// World direction through a screen UV (top-left origin).
vec3 FogRayDirection(FrameData frame, vec2 uv)
{
    const vec4 p = frame.invViewProj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0); // near plane
    return normalize(p.xyz / p.w - frame.cameraPosition.xyz);
}

float HeightFogDensity(FrameData frame, vec3 p)
{
    return frame.fogColor.w * exp(clamp(-frame.fogHeight.y * (p.y - frame.fogHeight.x), -60.0, 20.0));
}

// Integral of the height fog density along origin + dir * t, t in [a, b].
float HeightFogOpticalDepth(FrameData frame, vec3 origin, vec3 dir, float a, float b)
{
    const float base = HeightFogDensity(frame, origin);
    const float k    = frame.fogHeight.y * dir.y;
    if (abs(k) < 1e-5)
        return base * (b - a);
    return base * (exp(min(-k * a, 80.0)) - exp(min(-k * b, 80.0))) / k;
}

// Average sky light arriving from all directions (isotropic in-scattering).
vec3 FogAmbient(FrameData frame)
{
    return 0.5 * (SampleCube(frame.ibl.x, SAMPLER_LINEAR_CLAMP, vec3(0.0, 1.0, 0.0), 0.0).rgb +
                  SampleCube(frame.ibl.x, SAMPLER_LINEAR_CLAMP, vec3(0.0, -1.0, 0.0), 0.0).rgb) * frame.sky.y;
}

// Integrated froxel grid: rgb in-scattered light, a transmittance from the camera to viewDepth.
vec4 SampleFogVolume(FrameData frame, vec2 screenUv, float viewDepth)
{
    const float slices = frame.fogGrid.y;
    const float w      = sqrt(clamp(viewDepth / frame.fogGrid.x, 0.0, 1.0));
    // Slice z stores the integral up to its far end, w = (z + 1) / slices.
    const vec3 uvw = vec3(screenUv * frame.fogGrid.zw, w - 0.5 / slices);
    const vec4 v   = textureLod(sampler3D(uVolumeTextures[nonuniformEXT(frame.fogInfo.y)], uSamplers[SAMPLER_LINEAR_CLAMP]),
                                uvw, 0.0);
    return mix(vec4(0.0, 0.0, 0.0, 1.0), v, clamp(w * slices, 0.0, 1.0)); // in front of the first slice end
}

// Fog between the camera and a point `distance` away along `dir` (sky: sky = true). Returns rgb =
// in-scattered light, a = transmittance: color' = color * a + rgb.
vec4 ComputeFog(FrameData frame, vec2 screenUv, vec3 dir, float distance, bool sky)
{
    if (frame.fogInfo.x == 0u)
        return vec4(0.0, 0.0, 0.0, 1.0);
    const float cosView    = max(dot(dir, FogForward(frame)), 1e-3);
    const bool  volumetric = frame.fogInfo.w != 0u;
    if (sky)
        distance = 1e6;
    const float viewDepth = distance * cosView;

    // Analytic height fog behind the froxel grid (or everywhere without it).
    vec4        result = vec4(0.0, 0.0, 0.0, 1.0);
    const float start  = volumetric ? max(frame.fogHeight.z, frame.fogGrid.x / cosView) : frame.fogHeight.z;
    if (distance > start && frame.fogColor.w > 0.0) {
        const float tau     = HeightFogOpticalDepth(frame, frame.cameraPosition.xyz, dir, start, distance);
        const float opacity = min(1.0 - exp(-max(tau, 0.0)), frame.fogHeight.w);
        const vec3  sun     = frame.sunRadiance.rgb * FogPhase(dot(frame.sunDirection.xyz, -dir), frame.fogLight.x);
        const vec3  light   = frame.fogColor.rgb * (sun * frame.fogLight.y + FogAmbient(frame) * frame.fogLight.z);
        result = vec4(light * opacity, 1.0 - opacity);
    }
    if (volumetric) { // the analytic part lies behind the grid
        const vec4 v = SampleFogVolume(frame, screenUv, viewDepth);
        result = vec4(result.rgb * v.a + v.rgb, result.a * v.a);
    }
    return result;
}

vec3 ApplyFog(FrameData frame, vec3 color, vec2 screenUv, vec3 worldPos)
{
    const vec3  toPoint  = worldPos - frame.cameraPosition.xyz;
    const float distance = length(toPoint);
    const vec4  fog      = ComputeFog(frame, screenUv, toPoint / max(distance, 1e-6), distance, false);
    return color * fog.a + fog.rgb;
}

// For shaders that know the view depth (particles): the ray through the fragment's screen UV.
vec4 ComputeFogAtDepth(FrameData frame, vec2 screenUv, float viewDepth)
{
    const vec3 dir = FogRayDirection(frame, screenUv);
    return ComputeFog(frame, screenUv, dir, viewDepth / max(dot(dir, FogForward(frame)), 1e-3), false);
}

#endif
