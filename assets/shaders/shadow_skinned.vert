#version 450 core
// A2-s: depth-only sun pass for skinned casters (bone palette matches skinned.vert).
layout(location = 0) in vec3 aPos;
layout(location = 3) in ivec4 aBones;
layout(location = 4) in vec4 aWeights;

uniform mat4 uLightVP;
uniform mat4 uModel;
// 128 mirrors meat::kMaxBones; uploaded per draw like the lit skinned pass.
uniform mat4 uBones[128];

void main() {
    mat4 skin = aWeights.x * uBones[aBones.x]
              + aWeights.y * uBones[aBones.y]
              + aWeights.z * uBones[aBones.z]
              + aWeights.w * uBones[aBones.w];
    vec4 world = uModel * (skin * vec4(aPos, 1.0));
    gl_Position = uLightVP * world;
}
