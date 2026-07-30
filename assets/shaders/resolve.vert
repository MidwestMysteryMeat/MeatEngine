#version 450 core

// Attribute-less fullscreen triangle: gl_VertexID 0..2 covers the screen.
out vec2 vUv;

void main() {
    vUv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(vUv * 2.0 - 1.0, 0.0, 1.0);
}
