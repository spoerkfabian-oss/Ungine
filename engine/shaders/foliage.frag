#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
// Foliage lighting pass (depth EQUAL after the prepass): the shared forward lighting.
#include "include/foliage.glsl"
#include "include/shading.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inUv;
layout(location = 4) in vec4 inCurrentClip;
layout(location = 5) in vec4 inPreviousClip;

layout(location = 0) out vec4 outColor;

void main()
{
    FoliageLayerBuffer layer = pc.layer;
    vec3  base      = inColor.rgb;
    float roughness = 0.85, metallic = 0.0;
    vec3  emissive  = vec3(0.0);
    vec3  N         = normalize(inNormal);
    if (layer.info.x == FOLIAGE_CARDS) {
        if (layer.info.y != TERRAIN_NO_TEXTURE)
            base *= TerrainSample(layer.info.y, inUv).rgb;
    } else if (layer.info.x == FOLIAGE_MESH) {
        const Material m = pc.frame.materials.m[pc.material];
        base *= m.baseColorFactor.rgb * SampleTexture(pc.frame.textureTable.slot[m.baseColorTexture], m.samplerIndex, inUv).rgb;
        roughness = m.roughness;
        metallic  = m.metallic;
        emissive  = m.emissiveFactor.rgb;
        if (!gl_FrontFacing)
            N = -N;
    }
    SurfaceSample surface;
    surface.worldPos   = inWorldPos;
    surface.geomNormal = N;
    surface.N          = N;
    surface.base       = base;
    surface.metallic   = clamp(metallic, 0.0, 1.0);
    surface.roughness  = clamp(roughness, 0.045, 1.0);
    surface.emissive   = emissive;
    surface.occlusion  = inColor.a;
    outColor = vec4(ShadeSurface(pc.frame, surface, true), 1.0);
}
