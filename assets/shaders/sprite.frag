#version 450 core

// FrameData mirrors Renderer::FrameUbo (std140, binding 0). Keep in sync.
struct PointLight { vec4 posRadius; vec4 color; };
struct SpotLight  { vec4 posRadius; vec4 dirCosAngle; vec4 color; };
layout(std140, binding = 0) uniform FrameData {
    mat4 uView;
    mat4 uProj;
    vec4 uCamPos;
    vec4 uFogParams;
    vec4 uFogColor;
    vec4 uDirLightDir;
    vec4 uDirLightColor;
    vec4 uAmbientColor; // rgb sky/flat, w hemi strength
    vec4 uHemiGround;   // rgb ground lobe (A3)
    ivec4 uLightCounts;
    PointLight uPointLights[32];
    SpotLight uSpotLights[8];
};

layout(binding = 0) uniform sampler2D uAlbedo;

uniform vec3 uTint;

in VsOut {
    vec2 uv;
    vec3 light;
    float fog;
} fs;

out vec4 oColor;

void main() {
    vec4 albedo = texture(uAlbedo, fs.uv);
    // Alpha test instead of blend+sort: hard PSX-style cutout keeps depth
    // writes on, so sprite draw order never matters.
    if (albedo.a < 0.5) {
        discard;
    }
    vec3 c = albedo.rgb * fs.light * uTint;
    oColor = vec4(mix(c, uFogColor.rgb, fs.fog), 1.0);
}
