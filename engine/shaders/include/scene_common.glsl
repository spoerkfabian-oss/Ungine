#ifndef ENGINE_SCENE_COMMON_GLSL
#define ENGINE_SCENE_COMMON_GLSL
// Requires: #extension GL_EXT_buffer_reference : require
// Geometry pool and GPU scene records. Mirrors Engine::Vertex, GpuMaterial, GpuSubmesh (Model.h)
// and GpuInstance, GpuDraw, GpuBatch (GpuScene.h).

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

// One submesh of a model; offsets are absolute pool indices.
struct GpuSubmesh {
    uint firstIndex;
    uint indexCount;
    int  vertexOffset;
    uint material;
    vec3 boundsMin; // object space
    uint flags;     // material flags
    vec3 boundsMax;
    uint pad;
};

// One MeshRenderer entity.
struct GpuInstance {
    mat4 model;
    vec4 normal0; // inverse-transpose of the upper 3x3 (columns)
    vec4 normal1;
    vec4 normal2;
    uint entityId;  // entity slot index + 1 (picking)
    uint flags;     // INSTANCE_MIRRORED
    uint firstDraw;
    uint drawCount;
};
#define INSTANCE_MIRRORED 1u

// Instance x submesh: the unit of culling. instance == ~0u: free slot.
struct GpuDraw {
    uint instance;
    uint submesh;
    uint batch;
    uint pad;
};
#define DRAW_FREE 0xFFFFFFFFu

// Draws of one submesh with the same winding: one indirect command with instanceCount = number
// of visible draws. instanceBase: offset of its slice in a view's visible list.
struct GpuBatch {
    uint indexCount;
    uint firstIndex;
    int  vertexOffset;
    uint instanceBase;
    uint cameraBucket; // bit 0: double-sided (no culling), bit 1: mirrored (clockwise front faces)
    uint shadowBucket; // 1: alpha-masked
    uint pad0;
    uint pad1;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer VertexBuffer   { Vertex      v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer MaterialBuffer { Material    m[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SubmeshBuffer  { GpuSubmesh  s[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer InstanceBuffer { GpuInstance i[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer DrawBuffer     { GpuDraw     d[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer BatchBuffer    { GpuBatch    b[]; };
// Draw record per drawn instance (indexed by gl_InstanceIndex: firstInstance selects the slice).
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer VisibleBuffer   { uint        v[]; };
// Hi-Z pyramid (min depth, reverse-Z = farthest occluder), all levels packed one after another.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer HiZBuffer       { float       d[]; };

mat3 NormalMatrix(GpuInstance inst)
{
    return mat3(inst.normal0.xyz, inst.normal1.xyz, inst.normal2.xyz);
}

// Level sizes: level 0 = ceil(depth / 2), then halved (rounded up) down to 1 x 1.
uvec2 HiZLevelSize(uvec2 size0, uint level)
{
    uvec2 s = size0;
    for (uint i = 0u; i < level; ++i)
        s = max((s + 1u) / 2u, uvec2(1u));
    return s;
}

uint HiZLevelOffset(uvec2 size0, uint level)
{
    uint  offset = 0u;
    uvec2 s      = size0;
    for (uint i = 0u; i < level; ++i) {
        offset += s.x * s.y;
        s = max((s + 1u) / 2u, uvec2(1u));
    }
    return offset;
}

#endif
