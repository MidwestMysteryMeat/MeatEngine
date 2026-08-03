#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal; // delivered as normalized i8
layout(location = 2) in vec2 aUv;
layout(location = 3) in uint aTex;    // atlas tile index
layout(location = 4) in float aLight; // block-light level 0..15 (torch flood-fill)
layout(location = 5) in float aAo;    // per-vertex ambient occlusion 0..3 (0 = darkest corner)

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
    vec4 uAmbientColor; // rgb premultiplied by intensity; w hemi strength
    vec4 uHemiGround;   // rgb ground lobe (A3); unused here but part of the shared UBO layout
    ivec4 uLightCounts; // x point, y spot
    PointLight uPointLights[32];
    SpotLight uSpotLights[8];
};

uniform mat4 uModel;      // translation only for chunks
uniform vec2 uPsxJitter;  // vertex-snap grid (screen res); 0 disables. PSX vertex wobble.

out VsOut {
    vec3 worldPos;
    vec3 normal;
    noperspective vec2 uv; // affine (no perspective correction) → the PSX texture warp
    float fog;
    float blockLight; // 0..1 torch brightness, gouraud-interpolated across the quad
    float ao;         // 0..1 openness (1 = open face, 0 = fully occluded corner)
    flat uint tex;
} vs;

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vs.worldPos = world.xyz;
    vs.normal = aNormal; // chunk model matrix carries no rotation/scale
    vs.uv = aUv;
    vs.tex = aTex;
    vs.blockLight = clamp(aLight / 15.0, 0.0, 1.0);
    vs.ao = aAo / 3.0; // 0..3 packed corner AO -> 0..1 openness, gouraud-interpolated
    // Vertex fog on purpose: the coarse per-vertex gradient is part of the PSX look.
    float dist = distance(world.xyz, uCamPos.xyz);
    vs.fog = clamp((dist - uFogParams.x) / max(uFogParams.y - uFogParams.x, 1e-3), 0.0, 1.0)
             * uFogParams.z;
    vec4 clip = uProj * uView * world;
    // PSX vertex snapping: quantize the projected XY to the low-res pixel grid (the console had
    // no sub-pixel rasterization), producing the signature vertex jitter. Skip behind the camera.
    if (uPsxJitter.x > 0.0 && clip.w > 0.0) {
        clip.xy = round(clip.xy / clip.w * uPsxJitter) / uPsxJitter * clip.w;
    }
    gl_Position = clip;
}
