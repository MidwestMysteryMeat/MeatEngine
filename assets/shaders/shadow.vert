#version 450 core
// A2: depth-only pass from the sun. Position attribute matches chunk/mesh layout (loc 0).
layout(location = 0) in vec3 aPos;

uniform mat4 uLightVP;
uniform mat4 uModel;

void main() {
    gl_Position = uLightVP * uModel * vec4(aPos, 1.0);
}
