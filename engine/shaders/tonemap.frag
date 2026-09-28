#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "bindless.glsl"
#include "exposure.glsl"
#include "frame.glsl"

layout(push_constant) uniform TonemapPush {
    ExposureState state;         // auto exposure result of this frame
    uint          hdrTexture;
    uint          tonemapper;    // Engine::Tonemapper: 0 PBR Neutral, 1 ACES (fitted), 2 none (clamp)
    float         exposure;      // manual exposure, or compensation already folded into `state`
    uint          bloomTexture;  // half-resolution bloom result (mip 0 of the chain)
    float         bloomStrength; // 0 = bloom off (the texture is not read)
    uint          autoExposure;
    uint          debugView;     // Engine::DebugView: 0 off, 1 ambient occlusion, 2 normals, 3 light clusters
    uint          debugTexture;  // slot shown by the debug view (light clusters: depth)
    FrameData     frame;         // light clusters view
} pc;

layout(location = 0) out vec4 outColor;

// Khronos PBR Neutral (https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral).
vec3 PbrNeutral(vec3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation     = 0.15;

    const float x      = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;

    const float d       = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

// ACES RRT + ODT fit by Stephen Hill (sRGB/Rec.709 in and out). GLSL matrices are column-major.
vec3 AcesFitted(vec3 color)
{
    const mat3 inputMat  = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const mat3 outputMat = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    color            = inputMat * color;
    const vec3 a     = color * (color + 0.0245786) - 0.000090537;
    const vec3 b     = color * (0.983729 * color + 0.4329510) + 0.238081;
    return clamp(outputMat * (a / b), 0.0, 1.0);
}

// Blue -> cyan -> green -> yellow -> red for t in [0, 1].
vec3 Heat(float t)
{
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}

// Lights per cluster over a gray version of the image; white = cluster list full.
vec3 LightClusterOverlay(vec3 ldr, ivec2 pixel)
{
    const vec3  gray  = vec3(dot(ldr, vec3(0.2126, 0.7152, 0.0722)) * 0.5);
    const float depth = texelFetch(sampler2D(uTextures[nonuniformEXT(pc.debugTexture)], uSamplers[SAMPLER_NEAREST_CLAMP]),
                                   pixel, 0).r;
    FrameData frame = pc.frame;
    if (depth <= 0.0 || frame.lightInfo.x == 0u)
        return gray;
    const float viewDepth = frame.clusterDepth.x / depth; // reverse-Z, infinite far
    const uint  cluster   = ClusterIndex(gl_FragCoord.xy, viewDepth, frame.clusterParams.zw, frame.clusterParams.x,
                                         frame.clusterParams.y);
    const uint  count     = frame.clusters.counts[cluster];
    vec3        color     = count == 0u ? gray
                          : count >= CLUSTER_MAX_LIGHTS ? vec3(1.0)
                                                         : mix(gray, Heat(min(float(count) / 32.0, 1.0)), 0.75);
    // Tile borders.
    const vec2 f = fract(gl_FragCoord.xy * frame.clusterParams.zw);
    const vec2 w = frame.clusterParams.zw * 1.5;
    if (f.x < w.x || f.y < w.y)
        color *= 0.5;
    return color;
}

void main()
{
    // Same size as the swapchain: fetch the texel under this pixel.
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 hdr = texelFetch(sampler2D(uTextures[nonuniformEXT(pc.hdrTexture)], uSamplers[SAMPLER_NEAREST_CLAMP]),
                          pixel, 0).rgb;
    if (pc.bloomStrength > 0.0) {
        const vec2 size  = vec2(textureSize(sampler2D(uTextures[nonuniformEXT(pc.hdrTexture)],
                                                      uSamplers[SAMPLER_NEAREST_CLAMP]), 0));
        const vec3 bloom = SampleTexture(pc.bloomTexture, SAMPLER_LINEAR_CLAMP, gl_FragCoord.xy / size).rgb;
        hdr              = mix(hdr, bloom, pc.bloomStrength);
    }
    if (pc.debugView == 1u || pc.debugView == 2u) {
        const vec3 v = texelFetch(sampler2D(uTextures[nonuniformEXT(pc.debugTexture)], uSamplers[SAMPLER_NEAREST_CLAMP]),
                                  pixel, 0).rgb;
        outColor = vec4(pc.debugView == 1u ? v.rrr : v * 0.5 + 0.5, 1.0);
        return;
    }
    hdr *= pc.autoExposure != 0u ? pc.state.exposure : pc.exposure;
    vec3 ldr;
    switch (pc.tonemapper) {
    case 0u: ldr = PbrNeutral(hdr); break;
    case 1u: ldr = AcesFitted(hdr); break;
    default: ldr = clamp(hdr, 0.0, 1.0); break;
    }
    if (pc.debugView == 3u)
        ldr = LightClusterOverlay(ldr, pixel);
    outColor = vec4(ldr, 1.0); // sRGB swapchain encodes
}
