#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
// Mesh particles: material base color x particle color, optional simple sun / sky shading.
#include "include/bindless.glsl"
#include "include/particle.glsl"

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in float inViewDistance;
layout(location = 0) out vec4 outColor;

void main()
{
    const Material material = pc.frame.materials.m[pc.material];
    vec4 color = inColor * material.baseColorFactor;
    color *= SampleTexture(pc.frame.textureTable.slot[material.baseColorTexture], material.samplerIndex, inUv);
    color.rgb += material.emissiveFactor.rgb * inColor.rgb;
    if ((pc.flags & PARTICLE_SHADE) != 0u) {
        const vec3  n    = normalize(inNormal);
        const float sun  = max(dot(n, -pc.frame.sunDirection.xyz), 0.0);
        const float sky  = 0.5 + 0.5 * n.y;
        color.rgb *= 0.25 * sky + 0.75 * sun * clamp(length(pc.frame.sunRadiance.rgb) / 3.0, 0.0, 1.0) + 0.15;
    }
    if (pc.axis.w > 0.0)
        color.a *= clamp((inViewDistance - pc.frame.proj[3][2]) / pc.axis.w, 0.0, 1.0);
    color.a = clamp(color.a, 0.0, 1.0);
    if (color.a <= 0.0)
        discard;
    outColor = (pc.flags & PARTICLE_ADDITIVE) != 0u ? vec4(color.rgb * color.a, color.a) : color;
}
