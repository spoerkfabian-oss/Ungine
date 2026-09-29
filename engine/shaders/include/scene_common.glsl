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
    uint  baseColorTexture; // texture table entries
    uint  normalTexture;
    uint  metallicRoughnessTexture;
    uint  emissiveTexture;
    uint  occlusionTexture;
    uint  samplerIndex;
    float normalScale;
    float occlusionStrength;
};

// Material texture fields are texture table entries; the table maps them to bindless slots.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer TextureTable {
    uint slot[];
};

#define MATERIAL_ALPHA_MASK   1u
#define MATERIAL_DOUBLE_SIDED 2u
#define MATERIAL_ALPHA_BLEND  4u

// One submesh of a model; offsets are absolute pool indices.
struct GpuSubmesh {
    uint  firstIndex; // LOD 0
    uint  indexCount;
    int   vertexOffset;
    uint  material;
    vec3  boundsMin; // object space
    uint  flags;     // material flags
    vec3  boundsMax;
    uint  lodCount;
    uvec4 lodFirstIndex; // per LOD (up to 4), absolute
    uvec4 lodIndexCount;
    vec4  lodError;      // object-space deviation from LOD 0
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

// Visible list entries: draw record | LOD << 30 (the LOD picks the batch's index range; the
// shaders only need it for the LOD debug view).
#define VISIBLE_RECORD_MASK 0x3FFFFFFFu
#define VISIBLE_LOD_SHIFT   30u

// LOD whose object-space error, seen from the camera, stays below the pixel threshold.
// lodCamera: xyz camera position, w: pixels per unit of error at distance 1 (0: always LOD 0).
uint SelectLod(GpuSubmesh sm, vec3 center, vec3 extent, float scale, vec4 lodCamera, uint forced)
{
    if (sm.lodCount <= 1u)
        return 0u;
    if (forced > 0u)
        return min(forced - 1u, sm.lodCount - 1u);
    if (lodCamera.w <= 0.0)
        return 0u;
    const float distance = length(lodCamera.xyz - center) - length(extent);
    if (distance <= 0.0)
        return 0u;
    uint lod = 0u;
    for (uint l = 1u; l < sm.lodCount; ++l)
        if (sm.lodError[l] * scale * lodCamera.w <= distance)
            lod = l;
    return lod;
}
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
