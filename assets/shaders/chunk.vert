#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal; // delivered as normalized i8
layout(location = 2) in vec2 aUv;
layout(location = 3) in uint aTex;    // atlas tile index

// FrameData mirrors Renderer::FrameUbo (std140, binding 0). Keep in sync.
struct PointLight { vec4 posRadius; vec4 color; };
struct SpotLight  { vec4 posRadius; vec4 dirCosAngle; vec4 color; };
layout(std140, binding = 0) uniform FrameData {
    mat4 uView;
    mat4 uProj;
    vec4 uCamPos;
    vec4 uFogParams;   // x fogStart, y fogEnd, z fog enabled
    vec4 uFogColor;
    vec4 uDirLightDir; // direction the light travels, normalized
    vec4 uDirLightColor;
    ivec4 uLightCounts; // x point, y spot
    PointLight uPointLights[32];
    SpotLight uSpotLights[8];
};

uniform mat4 uModel; // translation only for chunks

out VsOut {
    vec3 worldPos;
    vec3 normal;
    vec2 uv;
    float fog;
    flat uint tex;
} vs;

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vs.worldPos = world.xyz;
    vs.normal = aNormal; // chunk model matrix carries no rotation/scale
    vs.uv = aUv;
    vs.tex = aTex;
    // Vertex fog on purpose: the coarse per-vertex gradient is part of the PSX look.
    float dist = distance(world.xyz, uCamPos.xyz);
    vs.fog = clamp((dist - uFogParams.x) / max(uFogParams.y - uFogParams.x, 1e-3), 0.0, 1.0)
             * uFogParams.z;
    gl_Position = uProj * uView * world;
}
