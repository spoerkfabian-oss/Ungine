#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
// Sprites and ribbons: unlit texture x color, soft edges against the scene depth, camera fade.
#include "include/bindless.glsl"
#include "include/particle.glsl"
#include "include/fog.glsl"

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 2) in float inViewDistance;
layout(location = 3) in vec2 inLocal;
layout(location = 4) in vec4 inCurrentClip;
layout(location = 5) in vec4 inPreviousClip;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outVelocity; // blended by coverage (alpha) over the opaque motion

void main()
{
    vec4 color = inColor;
    if ((pc.flags & PARTICLE_TEXTURED) != 0u) {
        const uint sampler_ = (pc.flags & PARTICLE_REPEAT) != 0u ? SAMPLER_LINEAR_REPEAT : SAMPLER_LINEAR_CLAMP;
        color *= SampleTexture(pc.frame.textureTable.slot[pc.texture], sampler_, inUv);
    } else { // no texture: a soft round sprite / soft-edged ribbon
        const float r2 = pc.facing == FACING_RIBBON ? inLocal.y * inLocal.y : dot(inLocal, inLocal);
        const float f  = clamp(1.0 - r2, 0.0, 1.0);
        color.a *= f * f;
    }
    if ((pc.flags & PARTICLE_SOFT) != 0u && pc.softness > 0.0) {
        const float depth = texelFetch(sampler2D(uTextures[nonuniformEXT(pc.depthTexture)], uSamplers[SAMPLER_NEAREST_CLAMP]),
                                       ivec2(gl_FragCoord.xy), 0).r;
        color.a *= clamp((ViewDistanceFromDepth(depth) - inViewDistance) / pc.softness, 0.0, 1.0);
    }
    if (pc.axis.w > 0.0)
        color.a *= clamp((inViewDistance - pc.frame.proj[3][2]) / pc.axis.w, 0.0, 1.0);
    color.a = clamp(color.a, 0.0, 1.0);
    if (color.a <= 0.0)
        discard;
    // Fog: additive light is only dimmed, blended particles also take the in-scattering.
    const vec4 fog = ComputeFogAtDepth(pc.frame, gl_FragCoord.xy * pc.frame.ssrParams.zw, inViewDistance);
    // Additive pipelines blend ONE / ONE: premultiply.
    outColor = (pc.flags & PARTICLE_ADDITIVE) != 0u ? vec4(color.rgb * (color.a * fog.a), color.a)
                                                    : vec4(color.rgb * fog.a + fog.rgb, color.a);
    outVelocity = vec4(ParticleMotion(inCurrentClip, inPreviousClip), 0.0, color.a);
}
