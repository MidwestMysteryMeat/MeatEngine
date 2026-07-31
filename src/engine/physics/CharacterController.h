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
    float gravity = -18.0f;         // legacy Y scalar; kept for defaults / tools
    glm::vec3 gravityVec{0.0f, -18.0f, 0.0f}; // active acceleration (B3b field sample)
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

    // Fall acceleration (m/s²). Scalar form is world-down (0, gY, 0) for env presets.
    // Vector form is B3b field sampling (volumes / orbital bodies). Valid before or after
    // init(); client prediction must apply the same sample the server used.
    void setGravity(float gravityY);
    void setGravity(glm::vec3 gravity);
    // B3b / H4: unit "up" for the capsule (default +Y). When gravity is strongly
    // non-vertical (orbital SOI), pass -normalize(g) so feet plant on planetoids.
    void setUp(glm::vec3 up);
    glm::vec3 up() const;

    glm::vec3 position() const; // feet (capsule bottom), meters
    glm::vec3 velocity() const; // m/s
    // Authoritative correction (net reconciliation / spawn): teleport, no sweep.
    void setState(glm::vec3 feetPos, glm::vec3 vel);
    bool onGround() const;      // ground state supported
    bool crouched() const;
    float eyeHeight() const; // meters along up() from position(), smoothed for the camera

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace meat
