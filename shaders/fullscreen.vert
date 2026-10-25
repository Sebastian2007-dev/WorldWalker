#version 460
layout(location = 0) out vec2 vUv;

void main() {
    vUv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    // z = 0 is the far plane with reversed Z, so the sky only fills untouched pixels.
    gl_Position = vec4(vUv * 2.0 - 1.0, 0.0, 1.0);
}
