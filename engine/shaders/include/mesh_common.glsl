#ifndef ENGINE_MESH_COMMON_GLSL
#define ENGINE_MESH_COMMON_GLSL
// Requires: #extension GL_EXT_buffer_reference : require
// Mesh passes (prepass, lighting, shadows). Mirrors MeshPush (SceneRenderer.cpp). Every draw -
// indirect from the GPU culling pass or direct from the CPU path - finds its draw record through
// pc.visible[gl_InstanceIndex] (firstInstance selects the slice of the list).
#include "frame.glsl"

#define MESH_TINT_LATE 1u // culling debug view: draws found visible by the late (occlusion) pass
#define MESH_TINT_LOD  2u // LOD debug view: tint by the draw's LOD

// LOD cross-fade (screen-door): during a transition the finer level drops the pixels where the
// noise is below `fade`, the coarser one keeps exactly those -> every pixel is drawn once.
// lodFade = visible entry >> 22: LOD (2 bits) | fade (7 bits, 0 = none) << 2 | fading in << 9.
bool LodFadeDiscard(uint lodFade, vec2 fragCoord)
{
    const uint fade = (lodFade >> 2u) & 127u;
    if (fade == 0u)
        return false;
    const float noise = fract(52.9829189 * fract(dot(fragCoord, vec2(0.06711056, 0.00583715)))); // IGN
    const float t     = float(fade) / 128.0;
    return (lodFade & 512u) != 0u ? noise >= t : noise < t;
}

layout(push_constant) uniform MeshPush {
    FrameData     frame;
    VisibleBuffer visible;
    uint          cascade; // shadow passes: < 4 sun cascade, else local shadow view (cascade - 4)
    uint          flags;   // MESH_TINT_LATE
} pc;

#ifdef ENGINE_BINDLESS_GLSL
// Material textures go through the texture table: a reload swaps the entry, not the material.
vec4 SampleMaterial(uint entry, uint samplerIndex, vec2 uv)
{
    return SampleTexture(pc.frame.textureTable.slot[entry], samplerIndex, uv);
}
#endif

#endif
