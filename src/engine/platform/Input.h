#pragma once
#include <glm/glm.hpp>

#include <array>
#include <cstdint>

struct GLFWwindow;

namespace meat {

class Window;

struct PlayerCommand {
    std::uint64_t tick = 0;
    glm::vec2 move{0};         // x strafe, y forward, unit-clamped
    float yaw = 0, pitch = 0;  // absolute radians, pitch clamped ±89°
    bool jump = false, crouch = false, sprint = false, fire = false, use = false, reload = false;
};

// Keyboard/mouse state via GLFW callbacks. The ONE place raw input becomes a
// PlayerCommand — nothing downstream reads GLFW state. Look integration lives
// here so aim latency never depends on tick timing.
class Input {
public:
    void attach(Window& window);          // installs GLFW callbacks
    void beginFrame();                    // clears per-frame deltas/presses
    bool down(int glfwKey) const;
    bool pressed(int glfwKey) const;      // edge-triggered this frame
    glm::vec2 mouseDelta() const;         // raw counts, unscaled
    float sensitivity = 0.0022f;          // radians per count
    PlayerCommand sampleCommand(std::uint64_t tick);

private:
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void cursorPosCallback(GLFWwindow* window, double x, double y);

    void onButton(int code, int action);
    void onCursorPos(double x, double y);

    // Covers GLFW_KEY_LAST (348); mouse buttons (0-7) share the low indices —
    // GLFW keys start at 32, so the ranges never collide.
    static constexpr int kMaxInputCodes = 512;
    std::array<bool, kMaxInputCodes> m_down{};
    std::array<bool, kMaxInputCodes> m_pressed{};

    glm::dvec2 m_lastCursor{0.0};
    bool m_hasLastCursor = false;         // suppresses the spike on the first motion event
    glm::vec2 m_frameDelta{0};            // cleared by beginFrame — what mouseDelta() reports
    glm::vec2 m_pendingLook{0};           // cleared by sampleCommand — never loses motion
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
};

} // namespace meat
