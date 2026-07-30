#include "engine/platform/Input.h"

#include "engine/platform/Window.h"

#include <GLFW/glfw3.h>

#include <cmath>

namespace meat {

// kMaxInputCodes is private; 512 mirrors its value in Input.h.
static_assert(GLFW_KEY_LAST < 512 && GLFW_MOUSE_BUTTON_LAST < 512,
              "input code table too small for this GLFW version");

namespace {
constexpr float kMaxPitch = 89.0f * 3.14159265358979323846f / 180.0f;

Input* self(GLFWwindow* window) {
    return static_cast<Input*>(glfwGetWindowUserPointer(window));
}
} // namespace

void Input::attach(Window& window) {
    GLFWwindow* handle = window.handle();
    glfwSetWindowUserPointer(handle, this);
    glfwSetKeyCallback(handle, &Input::keyCallback);
    glfwSetMouseButtonCallback(handle, &Input::mouseButtonCallback);
    glfwSetCursorPosCallback(handle, &Input::cursorPosCallback);
    glfwSetScrollCallback(handle, &Input::scrollCallback);
}

int Input::consumeScrollSteps() {
    const int steps = static_cast<int>(m_scrollAccum);
    m_scrollAccum -= static_cast<float>(steps);
    return steps;
}

void Input::scrollCallback(GLFWwindow* window, [[maybe_unused]] double x, double y) {
    if (Input* input = self(window)) input->m_scrollAccum += static_cast<float>(y);
}

void Input::beginFrame() {
    m_pressed.fill(false);
    m_frameDelta = glm::vec2{0};
}

bool Input::down(int glfwKey) const {
    return glfwKey >= 0 && glfwKey < kMaxInputCodes && m_down[static_cast<std::size_t>(glfwKey)];
}

bool Input::pressed(int glfwKey) const {
    return glfwKey >= 0 && glfwKey < kMaxInputCodes && m_pressed[static_cast<std::size_t>(glfwKey)];
}

glm::vec2 Input::mouseDelta() const {
    return m_frameDelta;
}

PlayerCommand Input::sampleCommand(std::uint64_t tick) {
    // Consume everything accumulated since the last sample, however many
    // frames or ticks that spanned.
    m_yaw -= m_pendingLook.x * sensitivity;
    m_pitch -= m_pendingLook.y * sensitivity;  // cursor y grows downward; mouse-up pitches up
    m_pitch = glm::clamp(m_pitch, -kMaxPitch, kMaxPitch);
    m_pendingLook = glm::vec2{0};

    PlayerCommand cmd;
    cmd.tick = tick;
    cmd.yaw = m_yaw;
    cmd.pitch = m_pitch;

    cmd.move.x = (down(GLFW_KEY_D) ? 1.0f : 0.0f) - (down(GLFW_KEY_A) ? 1.0f : 0.0f);
    cmd.move.y = (down(GLFW_KEY_W) ? 1.0f : 0.0f) - (down(GLFW_KEY_S) ? 1.0f : 0.0f);
    const float len = glm::length(cmd.move);
    if (len > 1.0f) cmd.move /= len;

    cmd.jump = down(GLFW_KEY_SPACE);
    cmd.crouch = down(GLFW_KEY_LEFT_CONTROL);
    cmd.sprint = down(GLFW_KEY_LEFT_SHIFT);
    cmd.fire = down(GLFW_MOUSE_BUTTON_LEFT);
    cmd.use = down(GLFW_KEY_E);
    cmd.reload = down(GLFW_KEY_R);
    cmd.place = down(GLFW_MOUSE_BUTTON_RIGHT);
    return cmd;
}

void Input::keyCallback(GLFWwindow* window, int key, [[maybe_unused]] int scancode, int action,
                        [[maybe_unused]] int mods) {
    if (Input* input = self(window)) input->onButton(key, action);
}

void Input::mouseButtonCallback(GLFWwindow* window, int button, int action,
                                [[maybe_unused]] int mods) {
    if (Input* input = self(window)) input->onButton(button, action);
}

void Input::cursorPosCallback(GLFWwindow* window, double x, double y) {
    if (Input* input = self(window)) input->onCursorPos(x, y);
}

void Input::onButton(int code, int action) {
    if (code < 0 || code >= kMaxInputCodes) return;  // GLFW_KEY_UNKNOWN etc.
    const auto i = static_cast<std::size_t>(code);
    if (action == GLFW_PRESS) {
        m_down[i] = true;
        m_pressed[i] = true;
    } else if (action == GLFW_RELEASE) {
        m_down[i] = false;
    }
    // GLFW_REPEAT is OS key-repeat, not a state change — ignored.
}

void Input::onCursorPos(double x, double y) {
    if (m_hasLastCursor) {
        const glm::vec2 delta{static_cast<float>(x - m_lastCursor.x),
                              static_cast<float>(y - m_lastCursor.y)};
        m_frameDelta += delta;
        m_pendingLook += delta;
    }
    m_lastCursor = glm::dvec2{x, y};
    m_hasLastCursor = true;
}

} // namespace meat
