#version 450 core
// B3-sky: procedural horizon/zenith/ground gradient (no cubemap). PSX-friendly.
uniform vec3 uCamForward;
uniform vec3 uCamRight;
uniform vec3 uCamUp;
uniform float uTanHalfFov; // tan(vFov/2); horizontal from aspect
uniform float uAspect;
uniform vec3 uZenith;
uniform vec3 uHorizon;
uniform vec3 uGround;
uniform int uStars; // 1 = space-ish sparse stars

in vec2 vUv;
out vec4 oColor;

// Cheap hash for star field (no textures).
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    // NDC from UV; y flips ImGui/OpenGL convention for view ray.
    vec2 ndc = vUv * 2.0 - 1.0;
    vec3 dir = normalize(uCamForward + uCamRight * (ndc.x * uTanHalfFov * uAspect) +
                         uCamUp * (ndc.y * uTanHalfFov));
    float h = dir.y; // -1..1
    vec3 col;
    if (h > 0.0) {
        float t = pow(clamp(h, 0.0, 1.0), 0.65);
        col = mix(uHorizon, uZenith, t);
    } else {
        float t = pow(clamp(-h, 0.0, 1.0), 0.55);
        col = mix(uHorizon, uGround, t);
    }
    if (uStars != 0 && h > 0.05) {
        // Stable-ish stars in view direction (low density).
        vec2 sp = dir.xz * 40.0 + dir.y * 17.0;
        float s = hash12(floor(sp * 8.0));
        if (s > 0.992) {
            float tw = 0.5 + 0.5 * hash12(sp);
            col += vec3(0.85, 0.9, 1.0) * tw * (0.4 + 0.6 * h);
        }
    }
    oColor = vec4(col, 1.0);
}
