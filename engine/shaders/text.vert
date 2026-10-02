#version 460
#extension GL_EXT_buffer_reference : require
// Text overlay: quads from stb_easy_font (4 vertices each: pixel position + RGBA8), pulled via BDA.

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer TextVertices { vec4 v[]; };

layout(push_constant) uniform TextPush {
    TextVertices vertices;
    vec2         screenSize;
    uint         srgbTarget;
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    const uint corners[6] = uint[](0, 1, 2, 0, 2, 3);
    const vec4 v          = pc.vertices.v[(gl_VertexIndex / 6) * 4 + corners[gl_VertexIndex % 6]];
    // Pixels (y down) -> NDC; the viewport is flipped (+Y up), so the top row is NDC +1.
    gl_Position = vec4(v.x / pc.screenSize.x * 2.0 - 1.0, 1.0 - v.y / pc.screenSize.y * 2.0, 0.0, 1.0);
    vec4 color  = unpackUnorm4x8(floatBitsToUint(v.w));
    if (pc.srgbTarget != 0u) // the target encodes: hand it linear values
        color.rgb = pow(color.rgb, vec3(2.2));
    outColor = color;
}
