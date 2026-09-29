#ifndef ENGINE_SURFACE_GLSL
#define ENGINE_SURFACE_GLSL
// Shading normal of a mesh surface (fragment shaders only: derivatives, gl_FrontFacing).
// Requires bindless.glsl, mesh_common.glsl and pbr.glsl.

// Tangent frame from screen-space derivatives (Schueler, "Normal Mapping Without Precomputed
// Tangents"). Scaled by sign(det) so it is independent of screen handedness (flipped viewport).
// Returns (grad u, -grad v, N): glTF's bitangent cross(N, T) * w points towards decreasing v
// (image up), for mirrored UVs as well.
mat3 CotangentFrame(vec3 N, vec3 p, vec2 uv)
{
    const vec3 dp1 = dFdx(p);
    const vec3 dp2 = dFdy(p);
    const vec2 duv1 = dFdx(uv);
    const vec2 duv2 = dFdy(uv);

    const vec3 dp2perp = cross(dp2, N);
    const vec3 dp1perp = cross(N, dp1);
    vec3       T       = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3       B       = dp2perp * duv1.y + dp1perp * duv2.y;

    const float lenSq = max(dot(T, T), dot(B, B));
    if (lenSq < 1e-20) // no UVs: any frame works, the normal map is flat anyway
        return BasisFromNormal(N);
    const float s = dot(N, cross(dp1, dp2)) < 0.0 ? -1.0 : 1.0;
    const float k = s * inversesqrt(lenSq);
    return mat3(T * k, -B * k, N);
}

// Tangent frame from the vertex tangent, or derived when the mesh has none (w == 0).
// Mirrored for back faces of double-sided materials.
mat3 SurfaceFrame(vec3 vertexNormal, vec4 vertexTangent, vec3 worldPos, vec2 uv)
{
    const vec3 ng = normalize(vertexNormal);
    mat3       tbn;
    if (abs(vertexTangent.w) > 0.5) {
        const vec3 t = normalize(vertexTangent.xyz - ng * dot(ng, vertexTangent.xyz));
        tbn          = mat3(t, cross(ng, t) * vertexTangent.w, ng);
    } else {
        tbn = CotangentFrame(ng, worldPos, uv);
    }
    return gl_FrontFacing ? tbn : -tbn; // back side of a double-sided surface: mirror the whole frame
}

vec3 PerturbedNormal(Material m, mat3 tbn, vec2 uv)
{
    // XY only (BC5 stores two channels): Z is reconstructed, the stored Z of RGBA maps ignored.
    const vec2 xy = SampleMaterial(m.normalTexture, m.samplerIndex, uv).xy * 2.0 - 1.0;
    const vec3 tn = vec3(xy * m.normalScale, sqrt(clamp(1.0 - dot(xy, xy), 0.0, 1.0)));
    return normalize(tbn * tn);
}

#endif
