#pragma once
#include "engine/platform/Input.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace meat {

// H4 skeleton: shared thruster integration for server authority and client
// prediction. Intentionally simple (no Jolt body yet) — 6DOF feel with gamey
// damping so Space pilots stay controllable.
struct ShipPose {
    glm::vec3 pos{0.0f};
    glm::vec3 vel{0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
};

inline constexpr float kShipThrust = 18.0f;      // m/s² forward/strafe
inline constexpr float kShipVerticalThrust = 14.0f; // jump=up, crouch=down
inline constexpr float kShipMaxSpeed = 40.0f;    // m/s hard cap
inline constexpr float kShipLinearDamp = 0.92f;  // per-tick multiplicative @ 60 Hz baseline
inline constexpr float kShipBoardRange = 5.5f;   // metres to Use-board
inline constexpr float kShipLeaveOffset = 3.2f;  // metres to the right on EVA
// Local half-extents of the hull box (X right, Y up, Z back — forward is -Z).
inline constexpr glm::vec3 kShipHalfExtents{1.15f, 0.42f, 2.4f};
// Seat offset from hull center (cockpit slightly forward/up).
inline constexpr glm::vec3 kShipSeatOffset{0.0f, 0.15f, -0.35f};
// Twin hardpoints (local space, fractions of half-extents applied by the server).
// X = wing, Y = up, Z = aft (nose is -Z).
inline constexpr glm::vec3 kShipHardpointL{-0.75f, 0.05f, -0.95f};
inline constexpr glm::vec3 kShipHardpointR{0.75f, 0.05f, -0.95f};

// Yaw/pitch → world orientation for the hull (pitch about local right, then yaw).
inline glm::quat shipOrientation(float yaw, float pitch) {
    const glm::quat qYaw = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::quat qPitch = glm::angleAxis(pitch, glm::vec3(1.0f, 0.0f, 0.0f));
    return qYaw * qPitch;
}

inline glm::mat4 shipTransform(glm::vec3 pos, float yaw, float pitch) {
    return glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(shipOrientation(yaw, pitch));
}

// World-space hardpoint from hull pose. `side` -1 left / +1 right.
inline glm::vec3 shipHardpointWorld(glm::vec3 pos, float yaw, float pitch, glm::vec3 halfExtents,
                                    float side) {
    const glm::vec3 local(side * std::abs(kShipHardpointL.x) * halfExtents.x,
                          kShipHardpointL.y * halfExtents.y,
                          kShipHardpointL.z * halfExtents.z);
    return pos + shipOrientation(yaw, pitch) * local;
}

// Look-relative thrust: W/S along view forward (including pitch), A/D strafe on
// horizontal right, jump/crouch for world-up/down relative to gravity "up" (+Y
// for the skeleton — reorient later with B3b local-up).
inline void integrateShip(ShipPose& ship, const PlayerCommand& cmd, float dt,
                          glm::vec3 gravityAccel) {
    ship.yaw = cmd.yaw;
    ship.pitch = glm::clamp(cmd.pitch, -glm::half_pi<float>() + 0.05f,
                            glm::half_pi<float>() - 0.05f);

    const float cy = std::cos(ship.yaw), sy = std::sin(ship.yaw);
    const float cp = std::cos(ship.pitch), sp = std::sin(ship.pitch);
    // -Z forward at yaw 0 (matches viewForward / CharacterController).
    const glm::vec3 forward(-sy * cp, sp, -cy * cp);
    const glm::vec3 right(cy, 0.0f, -sy);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    glm::vec2 move = cmd.move;
    const float mlen = glm::length(move);
    if (mlen > 1.0f) move /= mlen;

    glm::vec3 accel{0.0f};
    accel += forward * (move.y * kShipThrust);
    accel += right * (move.x * kShipThrust);
    if (cmd.jump) accel += up * kShipVerticalThrust;
    if (cmd.crouch) accel -= up * kShipVerticalThrust;
    // Light field pull so free-flight still feels Space (habitat / orbital SOI).
    accel += gravityAccel * 0.35f;

    ship.vel += accel * dt;
    // Frame-rate independent damping toward zero (equivalent ~0.92 at 60 Hz).
    const float damp = std::pow(kShipLinearDamp, dt * 60.0f);
    ship.vel *= damp;
    const float speed = glm::length(ship.vel);
    if (speed > kShipMaxSpeed) ship.vel *= kShipMaxSpeed / speed;

    ship.pos += ship.vel * dt;
}

// Quantize ship pitch into EntityState.data (signed, ~0.01 rad steps).
inline std::uint16_t packShipPitch(float pitch) {
    const float c = glm::clamp(pitch, -1.55f, 1.55f);
    const int q = static_cast<int>(std::lround(c * 1000.0f));
    return static_cast<std::uint16_t>(static_cast<std::int16_t>(q));
}
inline float unpackShipPitch(std::uint16_t data) {
    return static_cast<float>(static_cast<std::int16_t>(data)) / 1000.0f;
}

} // namespace meat
