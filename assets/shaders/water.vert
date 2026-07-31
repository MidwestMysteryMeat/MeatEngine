#version 450 core
// B3 water plane: large horizontal quad recentred on camera XZ at uWaterY.
// Attribute-less triangle strip (gl_VertexID 0..3), empty VAO like sprites.

// FrameData mirror (prefix only — must match Renderer::FrameUbo std140 offsets).
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

uniform float uWaterY;
uniform float uExtent;
uniform float uTime;

out VsOut {
    vec3 worldPos;
    float fog;
    float wave;
} vs;

void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    vec3 world = vec3(uCamPos.x, uWaterY, uCamPos.z) +
                 vec3(corner.x * uExtent, 0.0, corner.y * uExtent);
    // Cheap travelling ripple for tint modulation (no displacement — PSX flat surface).
    float w = sin(world.x * 0.12 + uTime * 1.4) * cos(world.z * 0.09 + uTime * 1.1);
    vs.worldPos = world;
    vs.wave = w * 0.5 + 0.5;
    float dist = distance(world, uCamPos.xyz);
    vs.fog = clamp((dist - uFogParams.x) / max(uFogParams.y - uFogParams.x, 1e-3), 0.0, 1.0) *
             uFogParams.z;
    gl_Position = uProj * uView * vec4(world, 1.0);
}
