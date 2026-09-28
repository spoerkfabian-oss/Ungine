#ifndef ENGINE_FRAME_GLSL
#define ENGINE_FRAME_GLSL
// Requires: #extension GL_EXT_buffer_reference : require
// Mirrors Engine::FrameUniforms (SceneRenderer.cpp).

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FrameData {
    mat4  viewProj;
    mat4  view;
    mat4  proj;
    mat4  invViewProj;
    vec4  cameraPosition;
    vec4  sunDirection; // xyz: direction the light travels
    vec4  sunRadiance;  // rgb: color * intensity
    vec4  sky;          // x: sky intensity, y: IBL intensity
    uvec4 ibl;          // x: irradiance cube, y: prefiltered cube, z: BRDF LUT, w: prefiltered mip count
};

#endif
