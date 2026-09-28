#version 460
#extension GL_EXT_buffer_reference : require
#include "frame.glsl"
#include "sky.glsl"

layout(push_constant) uniform SkyPush {
    FrameData frame;
} pc;

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

void main()
{
    // Unproject onto the near plane (depth 1 with reverse-Z; depth 0 is at infinity: w = 0).
    const vec4 p   = pc.frame.invViewProj * vec4(inNdc, 1.0, 1.0);
    const vec3 dir = normalize(p.xyz / p.w - pc.frame.cameraPosition.xyz);

    const vec3 toSun = normalize(-pc.frame.sunDirection.xyz);
    const vec3 sun   = pc.frame.sunRadiance.rgb;
    outColor = vec4(SkyRadiance(dir, toSun, sun, pc.frame.sky.x) + SunDisk(dir, toSun, sun), 1.0);
}
