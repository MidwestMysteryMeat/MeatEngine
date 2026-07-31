#version 450 core
// B3 water plane: alpha-blended tint; double-sided (renderer disables cull).

layout(std140, binding = 0) uniform FrameData {
    mat4 uView;
    mat4 uProj;
    vec4 uCamPos;
    vec4 uFogParams;
    vec4 uFogColor;
    vec4 uDirLightDir;
    vec4 uDirLightColor;
    vec4 uAmbientColor;
    vec4 uHemiGround;
    ivec4 uLightCounts;
};

uniform vec3 uWaterColor;
uniform float uWaterAlpha;

in VsOut {
    vec3 worldPos;
    float fog;
    float wave;
} fs;

out vec4 oColor;

void main() {
    // Slightly brighter crests so the plane reads as water, not solid fog.
    vec3 col = uWaterColor * mix(0.75, 1.15, fs.wave);
    // Soft sun glint on the up-facing normal.
    vec3 n = vec3(0.0, 1.0, 0.0);
    vec3 l = normalize(-uDirLightDir.xyz);
    float glint = pow(max(dot(n, l), 0.0), 24.0) * 0.35;
    col += uDirLightColor.rgb * glint;
    col = mix(col, uFogColor.rgb, fs.fog);
    // Fade alpha toward the horizon fog so the edge of the huge plane softens.
    float a = uWaterAlpha * (1.0 - fs.fog * 0.65);
    oColor = vec4(col, clamp(a, 0.0, 1.0));
}
