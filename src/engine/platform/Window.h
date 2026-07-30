#pragma once
#include <glm/glm.hpp>

struct GLFWwindow;

namespace meat {

struct WindowDesc {
    int width = 1600, height = 900;
    const char* title = "MeatEngine";
    bool vsync = true;
};

// GLFW window + GL 4.5 core context. Single window per process: init() calls
// glfwInit, the destructor terminates GLFW. GL function loading is the
// renderer's job (glad), not ours.
class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool init(const WindowDesc& desc);
    void pollEvents();
    bool shouldClose() const;
    void swap();
    void setRelativeMouse(bool enabled);
    glm::ivec2 framebufferSize() const;
    GLFWwindow* handle() const { return m_window; }

private:
    GLFWwindow* m_window = nullptr;
    bool m_glfwInitialized = false;
};

} // namespace meat
