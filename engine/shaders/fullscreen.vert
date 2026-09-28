#version 460
// Single triangle covering the screen at depth 0 (= infinitely far with reverse-Z).

layout(location = 0) out vec2 outNdc;

void main()
{
    const vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    outNdc       = p;
    gl_Position  = vec4(p, 0.0, 1.0);
}
