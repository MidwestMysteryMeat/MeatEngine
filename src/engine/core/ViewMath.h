#pragma once
#include <glm/glm.hpp>

#include <cmath>

namespace meat {

// The one definition of look direction: -Z forward at yaw 0, positive yaw
// turns left (CCW from above), positive pitch up. Server hitscan and client
// camera must agree exactly, so both call this.
inline glm::vec3 viewForward(float yaw, float pitch) {
    const float cp = std::cos(pitch);
    return {-std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp};
}

} // namespace meat
