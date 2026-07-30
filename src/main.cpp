// Temporary bootstrap: proves window + GL 4.5 context + toolchain. Replaced by
// engine/core/Engine once the platform/render/voxel/physics modules land.
#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include "engine/core/Log.h"

int main() {
    if (!glfwInit()) {
        meat::log::error("glfwInit failed");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1600, 900, "MeatEngine", nullptr, nullptr);
    if (!window) {
        meat::log::error("window creation failed (GL 4.5 core unavailable?)");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    if (!gladLoadGL(glfwGetProcAddress)) {
        meat::log::error("gladLoadGL failed");
        return 1;
    }
    meat::log::info("GL {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        glClearColor(0.10f, 0.09f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(window);
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
