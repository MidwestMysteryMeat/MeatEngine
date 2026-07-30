#pragma once

#include "engine/platform/Input.h" // PlayerCommand

#include <glm/vec3.hpp>

#include <memory>

namespace meat {

class PhysicsWorld;

// All movement tuning in one place; values are the ARCHITECTURE.md contract.
struct CharacterTuning {
    float walkSpeed = 4.5f;         // m/s
    float sprintSpeed = 7.0f;       // m/s
    float crouchSpeed = 2.2f;       // m/s
    float jumpSpeed = 4.6f;         // m/s straight up, edge-triggered
    float gravity = -18.0f;         // m/s², matches PhysicsWorld gravity
    float groundAccelScale = 10.0f; // accel = scale * targetSpeed → full speed in ~0.1 s
    float airControl = 0.3f;        // fraction of ground accel while airborne
    float stepUp = 0.35f;           // m, walk-stairs step height
    float maxSlopeDeg = 46.0f;
    float radius = 0.35f;           // m, capsule radius
    float standHeight = 1.80f;      // m, capsule total height, bottom at feet
    float crouchHeight = 0.95f;     // m
    float eyeStand = 1.62f;         // m above feet
    float eyeCrouch = 0.82f;        // m above feet
    float eyeLerpRate = 10.0f;      // 1/s, camera-height smoothing
};

// Jolt CharacterVirtual wrapper. Kinematic: not a body in the world; collides
// against chunk colliders through PhysicsWorld each fixed tick.
class CharacterController {
public:
    CharacterController();
    ~CharacterController();
    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    // Requires world.init() to have succeeded. spawnPos = feet position.
    bool init(PhysicsWorld& world, glm::vec3 spawnPos);

    void update(const PlayerCommand& cmd, float fixedDt, PhysicsWorld& world);

    glm::vec3 position() const; // feet (capsule bottom), meters
    glm::vec3 velocity() const; // m/s
    bool onGround() const;      // ground state supported
    bool crouched() const;
    float eyeHeight() const; // meters above position(), smoothed for the camera

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace meat
