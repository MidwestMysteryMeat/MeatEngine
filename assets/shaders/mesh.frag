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

// Per-draw material params (set via glProgramUniform, not the frame UBO).
uniform vec3 uTint;
uniform float uShininess;
uniform vec3 uEmissive;

in VsOut {
    vec3 worldPos;
    vec3 normal;
    noperspective vec2 uv; // affine — match mesh.vert
    float fog;
} fs;

out vec4 oColor;

const float kSpecStrength = 0.25;

vec3 blinnPhong(vec3 albedo, vec3 n, vec3 v, vec3 l, vec3 lightColor, float atten) {
    float ndl = max(dot(n, l), 0.0);
    float spec = 0.0;
    if (ndl > 0.0) {
        vec3 h = normalize(l + v);
        spec = pow(max(dot(n, h), 0.0), uShininess) * kSpecStrength;
    }
    return (albedo * ndl + vec3(spec)) * lightColor * atten;
}

vec3 ambientTerm(vec3 albedo, vec3 n) {
    float hemi = uAmbientColor.w;
    if (hemi <= 0.0) return albedo * uAmbientColor.rgb;
    float t = clamp(n.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 hemiCol = mix(uHemiGround.rgb, uAmbientColor.rgb, t);
    vec3 flatCol = mix(uHemiGround.rgb, uAmbientColor.rgb, 0.5);
    return albedo * mix(flatCol, hemiCol, hemi);
}

vec3 shade(vec3 albedo, vec3 n, vec3 v, vec3 worldPos) {
    vec3 c = ambientTerm(albedo, n);
    c += blinnPhong(albedo, n, v, normalize(-uDirLightDir.xyz), uDirLightColor.rgb, 1.0);
    for (int i = 0; i < uLightCounts.x; ++i) {
        vec3 toL = uPointLights[i].posRadius.xyz - worldPos;
        float d = length(toL);
        float att = clamp(1.0 - d / uPointLights[i].posRadius.w, 0.0, 1.0);
        att *= att;
        if (att > 0.0) {
            c += blinnPhong(albedo, n, v, toL / max(d, 1e-4), uPointLights[i].color.rgb, att);
        }
    }
    for (int i = 0; i < uLightCounts.y; ++i) {
        vec3 toL = uSpotLights[i].posRadius.xyz - worldPos;
        float d = length(toL);
        vec3 l = toL / max(d, 1e-4);
        float att = clamp(1.0 - d / uSpotLights[i].posRadius.w, 0.0, 1.0);
        att *= att;
        float cosCut = uSpotLights[i].dirCosAngle.w;
        att *= smoothstep(cosCut, mix(cosCut, 1.0, 0.2), dot(-l, uSpotLights[i].dirCosAngle.xyz));
        if (att > 0.0) {
            c += blinnPhong(albedo, n, v, l, uSpotLights[i].color.rgb, att);
        }
    }
    return c;
}

void main() {
    vec4 albedo = texture(uAlbedo, fs.uv);
    vec3 n = normalize(fs.normal);
    vec3 v = normalize(uCamPos.xyz - fs.worldPos);
    vec3 lit = shade(albedo.rgb, n, v, fs.worldPos) * uTint + uEmissive;
    oColor = vec4(mix(lit, uFogColor.rgb, fs.fog), albedo.a);
}
