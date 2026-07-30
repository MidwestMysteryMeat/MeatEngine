#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal; // delivered as normalized i8
layout(location = 2) in vec2 aUv;
layout(location = 3) in uint aTex;    // atlas tile index
layout(location = 4) in float aLight; // block-light level 0..15 (torch flood-fill)

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
    vec4 uAmbientColor; // rgb premultiplied by intensity; w unused
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
    float blockLight; // 0..1 torch brightness, gouraud-interpolated across the quad
    flat uint tex;
} vs;

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vs.worldPos = world.xyz;
    vs.normal = aNormal; // chunk model matrix carries no rotation/scale
    vs.uv = aUv;
    vs.tex = aTex;
    vs.blockLight = clamp(aLight / 15.0, 0.0, 1.0);
    // Vertex fog on purpose: the coarse per-vertex gradient is part of the PSX look.
    float dist = distance(world.xyz, uCamPos.xyz);
    vs.fog = clamp((dist - uFogParams.x) / max(uFogParams.y - uFogParams.x, 1e-3), 0.0, 1.0)
             * uFogParams.z;
    gl_Position = uProj * uView * world;
}
