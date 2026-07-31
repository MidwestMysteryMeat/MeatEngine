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

layout(binding = 0) uniform sampler2D uAtlas;

in VsOut {
    vec3 worldPos;
    vec3 normal;
    noperspective vec2 uv; // affine — must match chunk.vert's interpolation qualifier
    float fog;
    float blockLight; // 0..1 torch flood-fill brightness
    float ao;         // 0..1 openness (1 = open face, 0 = fully occluded corner)
    flat uint tex;
} fs;

out vec4 oColor;

const float kShininess = 32.0;
const float kSpecStrength = 0.25;

vec3 blinnPhong(vec3 albedo, vec3 n, vec3 v, vec3 l, vec3 lightColor, float atten) {
    float ndl = max(dot(n, l), 0.0);
    float spec = 0.0;
    if (ndl > 0.0) {
        vec3 h = normalize(l + v);
        spec = pow(max(dot(n, h), 0.0), kShininess) * kSpecStrength;
    }
    return (albedo * ndl + vec3(spec)) * lightColor * atten;
}

// A3: hemisphere ambient. Strength 0 → classic flat ambient (uAmbientColor.rgb).
// Strength 1 → full sky/ground blend by normal.y. NOT multiplied by block-light.
vec3 ambientTerm(vec3 albedo, vec3 n) {
    float hemi = uAmbientColor.w;
    if (hemi <= 0.0) return albedo * uAmbientColor.rgb;
    float t = clamp(n.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 hemiCol = mix(uHemiGround.rgb, uAmbientColor.rgb, t);
    vec3 flatCol = mix(uHemiGround.rgb, uAmbientColor.rgb, 0.5);
    return albedo * mix(flatCol, hemiCol, hemi);
}

// Direct lights only (sun / points / spots) — ambient is separate so chunk
// block-light can gate direct without crushing form-defining hemi fill.
vec3 shadeDirect(vec3 albedo, vec3 n, vec3 v, vec3 worldPos) {
    vec3 c = blinnPhong(albedo, n, v, normalize(-uDirLightDir.xyz), uDirLightColor.rgb, 1.0);
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
    // 16x16 tile atlas: integer tile from the vertex attrib, uv folded into the
    // tile so greedy quads with uv > 1 repeat correctly. Row counts from the TOP
    // (15 - row): textures load vertically-flipped for GL, and the atlas is
    // authored with tile 0 at the top-left (see tools/gen_atlas.py).
    vec2 tile = vec2(float(fs.tex % 16u), 15.0 - float(fs.tex / 16u));
    vec2 atlasUV = (tile + fract(fs.uv)) / 16.0;
    vec4 albedo = texture(uAtlas, atlasUV);

    vec3 n = normalize(fs.normal);
    vec3 v = normalize(uCamPos.xyz - fs.worldPos);
    // Ambient / hemi is form-defining fill and is NOT gated by torch flood-fill.
    vec3 ambient = ambientTerm(albedo.rgb, n);
    // Direct lights still fall off away from torches so night caves go dark.
    const float kMinLight = 0.10;
    vec3 direct = shadeDirect(albedo.rgb, n, v, fs.worldPos) * max(fs.blockLight, kMinLight);
    vec3 lit = ambient + direct;
    // Per-vertex voxel ambient occlusion (0fps): concave corners/edges darken.
    lit *= mix(0.45, 1.0, fs.ao);
    oColor = vec4(mix(lit, uFogColor.rgb, fs.fog), albedo.a);
}
