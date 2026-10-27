#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#include "bindless.glsl"
#include "frame.glsl"
#include "fog.glsl"
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
    const vec3 color = SkyRadiance(dir, toSun, sun, pc.frame.sky.x) + SunDisk(dir, toSun, sun);
    const vec4 fog   = ComputeFog(pc.frame, vec2(inNdc.x, -inNdc.y) * 0.5 + 0.5, dir, 0.0, true);
    outColor = vec4(color * fog.a + fog.rgb, 1.0);
}
