#version 460
#extension GL_EXT_buffer_reference : require
// Text overlay: ordered text, solid-rectangle and textured-image quads pulled via BDA.

struct OverlayVertex { vec4 positionUv; uvec4 data; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer TextVertices { OverlayVertex v[]; };

layout(push_constant) uniform TextPush {
    TextVertices vertices;
    vec2         screenSize;
    uint         srgbTarget;
} pc;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outUV;
layout(location = 2) flat out uint outTextureEntry;
layout(location = 3) flat out uint outTextured;

void main()
{
    const uint corners[6] = uint[](0, 1, 2, 0, 2, 3);
    const OverlayVertex v = pc.vertices.v[(gl_VertexIndex / 6) * 4 + corners[gl_VertexIndex % 6]];
    // Pixels (y down) -> NDC; the viewport is flipped (+Y up), so the top row is NDC +1.
    gl_Position = vec4(v.positionUv.x / pc.screenSize.x * 2.0 - 1.0,
                       1.0 - v.positionUv.y / pc.screenSize.y * 2.0, 0.0, 1.0);
    vec4 color  = unpackUnorm4x8(v.data.x);
    if (pc.srgbTarget != 0u) // the target encodes: hand it linear values
        color.rgb = pow(color.rgb, vec3(2.2));
    outColor = color;
    outUV = v.positionUv.zw;
    outTextureEntry = v.data.y;
    outTextured = v.data.z;
}
