#ifndef ENGINE_MESH_COMMON_GLSL
#define ENGINE_MESH_COMMON_GLSL
// Requires: #extension GL_EXT_buffer_reference : require
// Layouts mirror Engine::Vertex, Engine::GpuMaterial, DrawData and MeshPush (SceneRenderer.cpp).
#include "frame.glsl"

struct Vertex {
    vec3  position;
    float uvX;
    vec3  normal;
    float uvY;
    vec4  tangent; // w = bitangent sign; w == 0: no tangent, derive the frame in the shader
};

struct Material {
    vec4  baseColorFactor;
    vec4  emissiveFactor;
    float metallic;
    float roughness;
    float alphaCutoff;
    uint  flags;
    uint  baseColorTexture;
    uint  normalTexture;
    uint  metallicRoughnessTexture;
    uint  emissiveTexture;
    uint  occlusionTexture;
    uint  samplerIndex;
    float normalScale;
    float occlusionStrength;
};

#define MATERIAL_ALPHA_MASK   1u
#define MATERIAL_DOUBLE_SIDED 2u
#define MATERIAL_ALPHA_BLEND  4u

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer VertexBuffer   { Vertex   v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MaterialBuffer { Material m[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer DrawData {
    mat4 model;
    mat4 normalMatrix; // inverse-transpose of the upper 3x3, computed on the CPU
};

layout(push_constant) uniform MeshPush {
    FrameData      frame;
    VertexBuffer   vertices;
    MaterialBuffer materials;
    DrawData       draw;
    uint           materialIndex;
    uint           cascade; // shadow pass only
} pc;

#endif
