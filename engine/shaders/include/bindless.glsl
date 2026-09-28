#ifndef ENGINE_BINDLESS_GLSL
#define ENGINE_BINDLESS_GLSL
// Global descriptor set 0 - mirrors Engine::BindlessRegistry.
// Requires (declare in the main shader, right after #version):
//   #extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D uTextures[];
layout(set = 0, binding = 1) uniform sampler   uSamplers[];
layout(set = 0, binding = 2, rgba16f) uniform image2D uStorageImages[]; // HDR targets
layout(set = 0, binding = 3) uniform textureCube uCubeTextures[];
layout(set = 0, binding = 4, rgba16f) uniform image2DArray uStorageArrays[]; // cube faces as layers

// Must match Engine::DefaultSampler
#define SAMPLER_LINEAR_REPEAT 0u
#define SAMPLER_LINEAR_CLAMP  1u
#define SAMPLER_NEAREST_CLAMP 2u
#define SAMPLER_SHADOW        3u // comparison sampler (reverse-Z: GREATER_OR_EQUAL)

// nonuniformEXT is required when indices can diverge within a draw (e.g. per-material).
vec4 SampleTexture(uint texIndex, uint samplerIndex, vec2 uv)
{
    return texture(sampler2D(uTextures[nonuniformEXT(texIndex)], uSamplers[nonuniformEXT(samplerIndex)]), uv);
}

// Explicit LOD: required outside fragment shaders (no implicit derivatives in compute).
vec4 SampleTextureLod(uint texIndex, uint samplerIndex, vec2 uv, float lod)
{
    return textureLod(sampler2D(uTextures[nonuniformEXT(texIndex)], uSamplers[nonuniformEXT(samplerIndex)]), uv, lod);
}

vec4 SampleCube(uint cubeIndex, uint samplerIndex, vec3 dir, float lod)
{
    return textureLod(samplerCube(uCubeTextures[nonuniformEXT(cubeIndex)], uSamplers[nonuniformEXT(samplerIndex)]),
                      dir, lod);
}

#endif
