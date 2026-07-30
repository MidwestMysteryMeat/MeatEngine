#include "engine/platform/Window.h"

#include "engine/core/Log.h"

#include <GLFW/glfw3.h>

namespace meat {

Window::~Window() {
    if (m_window) glfwDestroyWindow(m_window);
    if (m_glfwInitialized) glfwTerminate();
}

bool Window::init(const WindowDesc& desc) {
    glfwSetErrorCallback([](int code, const char* description) {
        log::error("GLFW error {}: {}", code, description ? description : "(no description)");
    });

    if (!glfwInit()) {
        log::error("Window::init: glfwInit failed");
        return false;
    }
    m_glfwInitialized = true;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    m_window = glfwCreateWindow(desc.width, desc.height,
                                desc.title ? desc.title : "MeatEngine", nullptr, nullptr);
    if (!m_window) {
        log::error("Window::init: glfwCreateWindow failed ({}x{})", desc.width, desc.height);
        glfwTerminate();
        m_glfwInitialized = false;
        return false;
    }

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(desc.vsync ? 1 : 0);
    return true;
}

void Window::pollEvents() {
    glfwPollEvents();
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(m_window) != 0;
}

void Window::swap() {
    glfwSwapBuffers(m_window);
}

void Window::setRelativeMouse(bool enabled) {
    glfwSetInputMode(m_window, GLFW_CURSOR, enabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    // Raw motion only applies while the cursor is disabled; GLFW ignores it otherwise.
    if (enabled && glfwRawMouseMotionSupported())
        glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
}

glm::ivec2 Window::framebufferSize() const {
    glm::ivec2 size{0};
    glfwGetFramebufferSize(m_window, &size.x, &size.y);
    return size;
}

} // namespace meat
