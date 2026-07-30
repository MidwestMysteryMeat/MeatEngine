#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace meat {

// Right-handed, Y-up, meters. -Z is forward at yaw 0; positive yaw turns left
// (counter-clockwise seen from above), positive pitch looks up.
struct Camera {
    glm::vec3 pos{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovY = glm::radians(75.0f);

    glm::vec3 forward() const {
        const float cp = std::cos(pitch);
        return {-std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp};
    }

    glm::mat4 view() const { return glm::lookAt(pos, pos + forward(), glm::vec3(0.0f, 1.0f, 0.0f)); }

    glm::mat4 proj(float aspect) const { return glm::perspective(fovY, aspect, 0.05f, 300.0f); }
};

} // namespace meat
