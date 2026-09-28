#ifndef ENGINE_MESH_COMMON_GLSL
#define ENGINE_MESH_COMMON_GLSL
// Requires: #extension GL_EXT_buffer_reference : require
// Layouts mirror Engine::Vertex, Engine::GpuMaterial, Engine::FrameUniforms, Engine::MeshPush.

struct Vertex {
    vec3  position;
    float uvX;
    vec3  normal;
    float uvY;
    vec4  tangent;
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
    uint  _pad0;
    uint  _pad1;
};

#define MATERIAL_ALPHA_MASK   1u
#define MATERIAL_DOUBLE_SIDED 2u
#define MATERIAL_ALPHA_BLEND  4u

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer VertexBuffer   { Vertex   v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MaterialBuffer { Material m[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer FrameData {
    mat4 viewProj;
    mat4 view;
    mat4 proj;
    vec4 cameraPosition;
    vec4 sunDirection; // xyz: direction the light travels
    vec4 sunColor;     // rgb * intensity
    vec4 ambient;
};

layout(push_constant) uniform MeshPush {
    mat4           model;
    FrameData      frame;
    VertexBuffer   vertices;
    MaterialBuffer materials;
    uint           materialIndex;
} pc;

#endif
