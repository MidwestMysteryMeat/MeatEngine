#pragma once
#include "engine/core/ViewMath.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace meat {

// Right-handed, Y-up, meters. Look conventions live in core/ViewMath.h — the
// server's hitscan shares them.
struct Camera {
    glm::vec3 pos{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovY = glm::radians(75.0f);

    glm::vec3 forward() const { return viewForward(yaw, pitch); }

    glm::mat4 view() const { return glm::lookAt(pos, pos + forward(), glm::vec3(0.0f, 1.0f, 0.0f)); }

    glm::mat4 proj(float aspect) const { return glm::perspective(fovY, aspect, 0.05f, 300.0f); }
};

} // namespace meat
