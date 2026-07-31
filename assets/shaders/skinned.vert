#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in ivec4 aBones;   // integer attrib (glVertexAttribIPointer path)
layout(location = 4) in vec4 aWeights;  // normalized to sum 1 at import

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
    vec4 uAmbientColor;
    ivec4 uLightCounts;
    PointLight uPointLights[32];
    SpotLight uSpotLights[8];
};

uniform mat4 uModel;
uniform vec2 uPsxJitter; // vertex-snap grid (screen res); 0 disables
// Per-draw skinning palette: skinningMatrix[b] = global(b) * inverseBind(b).
// Uploaded with one glProgramUniformMatrix4fv per draw (count = live bones);
// 128 mirrors meat::kMaxBones.
uniform mat4 uBones[128];

out VsOut {
    vec3 worldPos;
    vec3 normal;
    noperspective vec2 uv; // affine PSX texture warp
    float fog;
} vs;

void main() {
    mat4 skin = aWeights.x * uBones[aBones.x]
              + aWeights.y * uBones[aBones.y]
              + aWeights.z * uBones[aBones.z]
              + aWeights.w * uBones[aBones.w];
    vec4 world = uModel * (skin * vec4(aPos, 1.0));
    vs.worldPos = world.xyz;
    // mat3 is fine for rotation + uniform scale; rigs don't shear. Normalized
    // again in the fragment stage, same as mesh.vert.
    vs.normal = mat3(uModel) * (mat3(skin) * aNormal);
    vs.uv = aUv;
    float dist = distance(world.xyz, uCamPos.xyz);
    vs.fog = clamp((dist - uFogParams.x) / max(uFogParams.y - uFogParams.x, 1e-3), 0.0, 1.0)
             * uFogParams.z;
    vec4 clip = uProj * uView * world;
    if (uPsxJitter.x > 0.0 && clip.w > 0.0) {
        clip.xy = round(clip.xy / clip.w * uPsxJitter) / uPsxJitter * clip.w;
    }
    gl_Position = clip;
}
