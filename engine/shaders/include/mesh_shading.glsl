#ifndef ENGINE_MESH_SHADING_GLSL
#define ENGINE_MESH_SHADING_GLSL
// Forward PBR shading of mesh.frag (opaque, depth EQUAL after the prepass) and mesh_blend.frag
// (MESH_BLEND: alpha-blended, sorted back to front, no screen-space AO).
#include "bindless.glsl"
#include "mesh_common.glsl"
#include "shading.glsl"
#include "surface.glsl"

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;
layout(location = 4) flat in uint inMaterial;
layout(location = 6) flat in uint inLodFade; // LOD | fade << 2 | fading in << 9 (VISIBLE_* >> 22)
#ifdef MESH_BLEND
layout(location = 7) in vec4 inCurrentClip;
layout(location = 8) in vec4 inPreviousClip;
#endif

layout(location = 0) out vec4 outColor;
#ifdef MESH_BLEND
layout(location = 1) out vec4 outVelocity; // blended by coverage (alpha) over the opaque motion
#endif

void main()
{
    if (LodFadeDiscard(inLodFade, gl_FragCoord.xy)) // same pixels as in the prepass
        discard;
    const Material m    = pc.frame.materials.m[inMaterial];
    vec4           base = m.baseColorFactor * SampleMaterial(m.baseColorTexture, m.samplerIndex, inUV);

#ifndef MESH_BLEND
    if ((m.flags & MATERIAL_ALPHA_MASK) != 0u && base.a < m.alphaCutoff)
        discard;
#endif

    // --- Normal ---
    const mat3 tbn = SurfaceFrame(inNormal, inTangent, inWorldPos, inUV);
    vec3       N   = PerturbedNormal(m, tbn, inUV);

    // --- Material ---
    const vec4    mr = SampleMaterial(m.metallicRoughnessTexture, m.samplerIndex, inUV); // G rough, B metal
    SurfaceSample surface;
    surface.worldPos   = inWorldPos;
    surface.geomNormal = tbn[2];
    surface.N          = N;
    surface.base       = base.rgb;
    surface.metallic   = clamp(m.metallic * mr.b, 0.0, 1.0);
    surface.roughness  = clamp(m.roughness * mr.g, 0.045, 1.0); // floor avoids aliasing sun highlights
    surface.emissive   = m.emissiveFactor.rgb * SampleMaterial(m.emissiveTexture, m.samplerIndex, inUV).rgb;
    surface.occlusion  = 1.0 + m.occlusionStrength * (SampleMaterial(m.occlusionTexture, m.samplerIndex, inUV).r - 1.0);

    FrameData frame = pc.frame;
#ifdef MESH_BLEND
    vec3 color = ShadeSurface(frame, surface, false);
#else
    vec3 color = ShadeSurface(frame, surface, true);
#endif
    if ((pc.flags & MESH_TINT_LATE) != 0u) // culling debug view: found by the occlusion (late) pass
        color = color * vec3(0.4, 0.25, 0.1) + vec3(0.6, 0.25, 0.02) * frame.sky.x;
    if ((pc.flags & MESH_TINT_LOD) != 0u) { // LOD debug view: 0 as is, 1 green, 2 yellow, 3 red
        const vec3 tint[4] = vec3[](vec3(1.0), vec3(0.2, 1.0, 0.2), vec3(1.0, 0.9, 0.1), vec3(1.0, 0.15, 0.1));
        const uint lod     = inLodFade & 3u;
        const vec3 t       = tint[lod];
        color = lod == 0u ? color : color * t + t * 0.08 * frame.sky.x;
    }
#ifdef MESH_BLEND
    outColor    = vec4(color, base.a); // straight alpha, blended over the opaque image
    outVelocity = vec4(MotionVector(inCurrentClip, inPreviousClip), 0.0, base.a);
#else
    outColor = vec4(color, 1.0);
#endif
}

#endif
