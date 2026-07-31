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
layout(binding = 1) uniform sampler2D uShadowMap;
uniform mat4 uLightVP;
uniform int uShadows;

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

float sunShadow(vec3 worldPos, vec3 n) {
    if (uShadows == 0) return 1.0;
    vec4 ls = uLightVP * vec4(worldPos, 1.0);
    vec3 p = ls.xyz / max(ls.w, 1e-5);
    p = p * 0.5 + 0.5;
    if (p.z > 1.0 || any(lessThan(p.xy, vec2(0.0))) || any(greaterThan(p.xy, vec2(1.0))))
        return 1.0;
    float ndl = max(dot(n, normalize(-uDirLightDir.xyz)), 0.0);
    float bias = max(0.003 * (1.0 - ndl), 0.0008);
    float shadow = 0.0;
    vec2 ts = 1.0 / vec2(textureSize(uShadowMap, 0));
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float closest = texture(uShadowMap, p.xy + vec2(float(x), float(y)) * ts).r;
            shadow += (p.z - bias > closest) ? 0.40 : 1.0;
        }
    }
    return shadow / 9.0;
}

vec3 shade(vec3 albedo, vec3 n, vec3 v, vec3 worldPos) {
    vec3 c = ambientTerm(albedo, n);
    float sh = sunShadow(worldPos, n);
    c += blinnPhong(albedo, n, v, normalize(-uDirLightDir.xyz), uDirLightColor.rgb, 1.0) * sh;
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
