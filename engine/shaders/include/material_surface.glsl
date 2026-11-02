#ifndef ENGINE_MATERIAL_SURFACE_GLSL
#define ENGINE_MATERIAL_SURFACE_GLSL
// Fragment side of custom materials (after material.generated.glsl): evaluates the graph for a
// pixel and turns its tangent space normal into the shading normal. Requires surface.glsl.

MaterialResult EvaluatePixelMaterial(vec3 worldPos, vec3 normal, vec2 uv, vec3 objectPosition, uint params)
{
    MaterialContext ctx = MaterialContextOf(worldPos, normal, uv, objectPosition, pc.frame.materialTime.x, params);
    ctx.screenUV   = gl_FragCoord.xy * pc.frame.ssrParams.zw;
    ctx.pixelDepth = -(pc.frame.view * vec4(worldPos, 1.0)).z;
    return EvaluateMaterial(ctx);
}

vec3 MaterialShadingNormal(MaterialResult r, mat3 tbn)
{
#ifdef MATERIAL_HAS_NORMAL
    const vec3 n = tbn * r.normal;
    const float lengthSq = dot(n, n);
    return lengthSq > 1e-12 ? n * inversesqrt(lengthSq) : tbn[2];
#else
    return tbn[2];
#endif
}

bool MaterialMaskDiscard(MaterialResult r, uint params)
{
#if MATERIAL_BLEND_MODE == MATERIAL_MASKED
    return r.opacityMask < MaterialParam(params, MATERIAL_ALPHA_CUTOFF_SLOT).x;
#else
    return false;
#endif
}

#endif
