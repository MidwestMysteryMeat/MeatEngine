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
    vec4 uAmbientColor;
    ivec4 uLightCounts;
    PointLight uPointLights[32];
    SpotLight uSpotLights[8];
};

uniform vec3 uCenter;
uniform vec2 uSize;   // world-space width/height (m)
uniform vec4 uUvRect; // xy offset, zw size within the texture
uniform int uFullbright;

out VsOut {
    vec2 uv;
    vec3 light;
    float fog;
} vs;

void main() {
    // Attribute-less unit quad: gl_VertexID 0..3 -> (-1,-1)(1,-1)(-1,1)(1,1),
    // drawn as a triangle strip. One shared empty VAO serves every sprite.
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    // The view matrix's rows 0/1 are the camera's right/up axes in world space.
    vec3 right = vec3(uView[0][0], uView[1][0], uView[2][0]);
    vec3 up    = vec3(uView[0][1], uView[1][1], uView[2][1]);
    vec3 world = uCenter + right * (corner.x * 0.5 * uSize.x) + up * (corner.y * 0.5 * uSize.y);

    vs.uv = uUvRect.xy + (corner * 0.5 + 0.5) * uUvRect.zw;

    // Cheap billboard lighting: ambient + directional against a camera-facing
    // normal, computed once per quad. fullbright skips lighting entirely.
    vec3 n = normalize(uCamPos.xyz - uCenter);
    vec3 lit = uAmbientColor.rgb + uDirLightColor.rgb * max(dot(n, -uDirLightDir.xyz), 0.0);
    vs.light = (uFullbright != 0) ? vec3(1.0) : lit;

    // Vertex fog, same gradient as the chunk/mesh passes.
    float dist = distance(world, uCamPos.xyz);
    vs.fog = clamp((dist - uFogParams.x) / max(uFogParams.y - uFogParams.x, 1e-3), 0.0, 1.0)
             * uFogParams.z;
    gl_Position = uProj * uView * vec4(world, 1.0);
}
