#version 450 core
// Fullscreen triangle; view ray rebuilt in frag from camera basis + UV.
out vec2 vUv;

void main() {
    vUv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(vUv * 2.0 - 1.0, 0.999, 1.0); // near far plane so geometry wins depth
}
