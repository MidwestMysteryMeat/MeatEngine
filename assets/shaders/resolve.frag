#version 450 core

layout(binding = 0) uniform sampler2D uScene; // PSX color target, NEAREST-sampled

uniform int uDither;      // PsxOptions::dither
uniform vec2 uSourceSize; // internal (pre-upscale) resolution in pixels

in vec2 vUv;
out vec4 oColor;

// 4x4 ordered Bayer matrix. Thresholds are recentered to [-0.5, 0.5) and scaled
// to one 5-bit quantization step — the classic PSX 15-bit framebuffer dither.
const float kBayer[16] = float[16](
     0.0,  8.0,  2.0, 10.0,
    12.0,  4.0, 14.0,  6.0,
     3.0, 11.0,  1.0,  9.0,
    15.0,  7.0, 13.0,  5.0);

void main() {
    vec3 c = texture(uScene, vUv).rgb;
    if (uDither != 0) {
        // Index the pattern in internal-resolution pixels so each upscaled
        // block shares one dither cell instead of dithering inside the block.
        ivec2 p = ivec2(vUv * uSourceSize) & 3;
        float t = (kBayer[p.y * 4 + p.x] + 0.5) / 16.0 - 0.5;
        c = floor(clamp(c + t / 31.0, 0.0, 1.0) * 31.0 + 0.5) / 31.0;
    }
    oColor = vec4(c, 1.0);
}
