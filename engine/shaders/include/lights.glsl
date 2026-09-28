#ifndef ENGINE_LIGHTS_GLSL
#define ENGINE_LIGHTS_GLSL
// Requires: #extension GL_EXT_buffer_reference : require
// Clustered punctual lights. Mirrors Engine::GpuLight and the cluster constants (SceneRenderer.cpp).

#define CLUSTER_X 16u
#define CLUSTER_Y 9u
#define CLUSTER_Z 24u
#define CLUSTER_COUNT (CLUSTER_X * CLUSTER_Y * CLUSTER_Z)
#define CLUSTER_MAX_LIGHTS 256u // per cluster; the list is truncated beyond

#define LIGHT_POINT 0u
#define LIGHT_SPOT  1u
#define NO_SHADOW   0xFFFFFFFFu

struct GpuLight {
    vec3  position;   // world space
    float range;
    vec3  color;      // color * intensity (candela)
    float spotScale;  // 1 / (cos(inner) - cos(outer)); 0 for point lights
    vec3  direction;  // world space, where the light points (spot)
    float spotOffset; // -cos(outer) * spotScale; 1 for point lights
    float cosOuter;
    float sinOuter;
    uint  type;
    uint  shadow;     // first view in the shadow view buffer (point lights: + cube face); NO_SHADOW
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LightBuffer { GpuLight l[]; };

// One perspective shadow view in the local light shadow atlas. Mirrors Engine::GpuShadowView.
struct GpuShadowView {
    mat4 viewProj; // reverse-Z, infinite far
    vec4 rect;     // atlas UV: xy offset, zw size
    vec4 params;   // x: world size of one texel per unit of distance along the view axis
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ShadowViewBuffer { GpuShadowView v[]; };

// counts[c], then CLUSTER_MAX_LIGHTS indices per cluster.
layout(buffer_reference, std430, buffer_reference_align = 16) buffer ClusterBuffer {
    uint counts[CLUSTER_COUNT];
    uint indices[];
};

// Logarithmic depth slices: slice = log(viewDepth) * scale + bias.
uint ClusterIndex(vec2 fragCoord, float viewDepth, vec2 tileScale, float sliceScale, float sliceBias)
{
    const uvec2 tile  = min(uvec2(fragCoord * tileScale), uvec2(CLUSTER_X - 1u, CLUSTER_Y - 1u));
    const float s     = log(max(viewDepth, 1e-6)) * sliceScale + sliceBias;
    const uint  slice = uint(clamp(s, 0.0, float(CLUSTER_Z - 1u)));
    return (slice * CLUSTER_Y + tile.y) * CLUSTER_X + tile.x;
}

// Smooth window to zero at the range (Karis 2013) times inverse square falloff.
float DistanceAttenuation(float distanceSq, float range)
{
    const float r      = distanceSq / (range * range);
    float       window = clamp(1.0 - r * r, 0.0, 1.0);
    window *= window;
    return window / max(distanceSq, 1e-4);
}

// KHR_lights_punctual spot cone: smooth between the outer and inner angle. Point lights: 1.
float SpotAttenuation(vec3 toSurface, GpuLight light)
{
    const float cd = dot(light.direction, toSurface);
    const float t  = clamp(cd * light.spotScale + light.spotOffset, 0.0, 1.0);
    return t * t;
}

#endif
