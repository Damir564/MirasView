#version 460

// One triangle covering the viewport; outputs NDC so fragment shaders can unproject. It lies on the far
// plane, so with a LESS_OR_EQUAL depth test (the sky) it only covers pixels no geometry was drawn on.
layout(location = 0) out vec2 outNdc;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    outNdc = uv * 2.0 - 1.0;
    gl_Position = vec4(outNdc, 1.0, 1.0);
}
