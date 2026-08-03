#include "engine/physics/CharacterController.h"

#include "engine/core/Log.h"
#include "engine/physics/PhysicsWorld.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>

JPH_SUPPRESS_WARNINGS

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>

namespace meat {

namespace {

// Capsule with its bottom at local y=0 so character position == feet position.
JPH::RefConst<JPH::Shape> makeCapsule(float totalHeight, float radius) {
    const float cylinderHalfHeight = 0.5f * totalHeight - radius;
    const JPH::RotatedTranslatedShapeSettings settings(
        JPH::Vec3(0.0f, 0.5f * totalHeight, 0.0f), JPH::Quat::sIdentity(),
        new JPH::CapsuleShape(cylinderHalfHeight, radius));
    return settings.Create().Get();
}

} // namespace

struct CharacterController::Impl {
    CharacterTuning tuning;
    JPH::Ref<JPH::CharacterVirtual> character;
    JPH::RefConst<JPH::Shape> standShape;
    JPH::RefConst<JPH::Shape> crouchShape;
    bool crouched = false;
    bool jumpHeld = false; // jump edge-trigger state across updates
    float eyeHeight = CharacterTuning{}.eyeStand;
    bool warnedUninit = false;
    glm::vec3 up{0.0f, 1.0f, 0.0f}; // B3b local-up (matches CharacterVirtual mUp)
    float speedScale = 1.0f;        // gameplay speed modifier (slow/haste effects)
};

CharacterController::CharacterController() : m_impl(std::make_unique<Impl>()) {}

CharacterController::~CharacterController() = default;

bool CharacterController::init(PhysicsWorld& world, glm::vec3 spawnPos) {
    JPH::PhysicsSystem* system = world.joltSystem();
    if (system == nullptr) {
        log::error("CharacterController: PhysicsWorld not initialized");
        return false;
    }
    Impl& im = *m_impl;
    const CharacterTuning& t = im.tuning;
    im.standShape = makeCapsule(t.standHeight, t.radius);
    im.crouchShape = makeCapsule(t.crouchHeight, t.radius);

    JPH::CharacterVirtualSettings settings;
    settings.mShape = im.standShape;
    settings.mUp = JPH::Vec3::sAxisY();
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(t.maxSlopeDeg);
    // Contacts on the bottom hemisphere count as support, so stair edges and
    // ledges hold the character up instead of sliding it off.
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -t.radius);
    im.character = new JPH::CharacterVirtual(
        &settings, JPH::RVec3(spawnPos.x, spawnPos.y, spawnPos.z), JPH::Quat::sIdentity(), 0,
        system);
    im.eyeHeight = t.eyeStand;
    log::info("CharacterController: spawned at ({:.1f},{:.1f},{:.1f})", spawnPos.x, spawnPos.y,
              spawnPos.z);
    return true;
}

void CharacterController::update(const PlayerCommand& cmd, float fixedDt, PhysicsWorld& world) {
    Impl& im = *m_impl;
    if (!im.character) {
        if (!im.warnedUninit) {
            log::error("CharacterController: update() before init()");
            im.warnedUninit = true;
        }
        return;
    }
    JPH::PhysicsSystem& system = *world.joltSystem();
    JPH::TempAllocator& tempAlloc = *world.joltTempAllocator();
    const CharacterTuning& t = im.tuning;

    // Crouch swaps the capsule. Shrinking always succeeds; standing back up is
    // rejected by SetShape while geometry overlaps and retries next tick.
    if (cmd.crouch != im.crouched) {
        const JPH::Shape* target = cmd.crouch ? im.crouchShape.GetPtr() : im.standShape.GetPtr();
        const float maxPenetration = 1.5f * system.GetPhysicsSettings().mPenetrationSlop;
        if (im.character->SetShape(target, maxPenetration,
                                   system.GetDefaultBroadPhaseLayerFilter(objlayer::kMoving),
                                   system.GetDefaultLayerFilter(objlayer::kMoving), {}, {},
                                   tempAlloc))
            im.crouched = cmd.crouch;
    }

    // Local-up frame: movement is on the plane perpendicular to `up`, so planetoids
    // (B3b) and habitats work without rewriting the capsule each tick.
    const glm::vec3 up = im.up;
    glm::vec3 worldForward(-std::sin(cmd.yaw), 0.0f, -std::cos(cmd.yaw));
    // Project look-forward onto the ground plane of `up`.
    worldForward = worldForward - up * glm::dot(worldForward, up);
    if (glm::dot(worldForward, worldForward) < 1e-6f)
        worldForward = glm::vec3(0.0f, 0.0f, -1.0f) - up * glm::dot(glm::vec3(0, 0, -1), up);
    worldForward = glm::normalize(worldForward);
    glm::vec3 worldRight = glm::cross(worldForward, up);
    if (glm::dot(worldRight, worldRight) < 1e-6f) worldRight = glm::vec3(1, 0, 0);
    else worldRight = glm::normalize(worldRight);

    glm::vec2 move = cmd.move;
    const float moveLen = glm::length(move);
    if (moveLen > 1.0f) move /= moveLen;
    const float speed =
        (im.crouched ? t.crouchSpeed : (cmd.sprint ? t.sprintSpeed : t.walkSpeed)) * im.speedScale;
    const glm::vec3 want = (worldRight * move.x + worldForward * move.y) * speed;

    const JPH::Vec3 curVel = im.character->GetLinearVelocity();
    glm::vec3 vel(curVel.GetX(), curVel.GetY(), curVel.GetZ());
    const float velAlongUp = glm::dot(vel, up);
    glm::vec3 horiz = vel - up * velAlongUp;

    const bool grounded =
        im.character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;

    if (grounded || moveLen > 0.0f) {
        const float accelRate = t.groundAccelScale * speed * (grounded ? 1.0f : t.airControl);
        const glm::vec3 delta = want - horiz;
        const float deltaLen = glm::length(delta);
        const float maxStep = accelRate * fixedDt;
        horiz = deltaLen <= maxStep ? want : horiz + delta * (maxStep / deltaLen);
    }

    const glm::vec3 g = t.gravityVec;
    const float gMag = glm::length(g);
    // B3b / H4 EVA: when the field is near free-fall, jump/crouch become RCS thrusters
    // and air control is full so spacewalks stay steerable without a ship.
    const bool eva = gMag < 2.0f;
    float vUp = velAlongUp;
    if (!eva && grounded && vUp <= 0.1f) {
        vUp = 0.0f;
        if (cmd.jump && !im.jumpHeld) vUp = t.jumpSpeed;
    } else if (eva) {
        constexpr float kEvaThrust = 8.0f; // m/s² RCS along local-up
        if (cmd.jump) vUp += kEvaThrust * fixedDt;
        if (cmd.crouch) vUp -= kEvaThrust * fixedDt;
        // Lighter damping so you coast between burns.
        vUp *= std::pow(0.98f, fixedDt * 60.0f);
        horiz *= std::pow(0.98f, fixedDt * 60.0f);
        // Stronger steer while EVA (move already mixed into want above).
        if (moveLen > 0.0f) {
            const float evaAccel = t.walkSpeed * 6.0f * fixedDt;
            const glm::vec3 delta = want - horiz;
            const float dLen = glm::length(delta);
            horiz = dLen <= evaAccel ? want : horiz + delta * (evaAccel / dLen);
        }
    } else {
        // Gravity fully in 3D; project residual onto up for the jump axis.
        vel = horiz + up * vUp + g * fixedDt;
        vUp = glm::dot(vel, up);
        horiz = vel - up * vUp;
    }
    im.jumpHeld = cmd.jump;

    // Still apply residual gravity in EVA (near-zero field) and normal air/ground.
    if (eva) {
        vel = horiz + up * vUp + g * fixedDt;
    } else {
        vel = horiz + up * vUp;
    }
    im.character->SetLinearVelocity(JPH::Vec3(vel.x, vel.y, vel.z));

    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
    updateSettings.mWalkStairsStepUp = JPH::Vec3(up.x, up.y, up.z) * t.stepUp;
    im.character->ExtendedUpdate(fixedDt, JPH::Vec3(g.x, g.y, g.z), updateSettings,
                                 system.GetDefaultBroadPhaseLayerFilter(objlayer::kMoving),
                                 system.GetDefaultLayerFilter(objlayer::kMoving), {}, {},
                                 tempAlloc);

    const float targetEye = im.crouched ? t.eyeCrouch : t.eyeStand;
    im.eyeHeight += (targetEye - im.eyeHeight) * std::min(1.0f, t.eyeLerpRate * fixedDt);
}

void CharacterController::setGravity(float gravityY) {
    m_impl->tuning.gravity = gravityY;
    m_impl->tuning.gravityVec = glm::vec3(0.0f, gravityY, 0.0f);
}

void CharacterController::setGravity(glm::vec3 gravity) {
    m_impl->tuning.gravityVec = gravity;
    m_impl->tuning.gravity = gravity.y; // keep scalar in sync for any Y-only readers
}

void CharacterController::setSpeedScale(float scale) {
    m_impl->speedScale = scale > 0.0f ? scale : 0.0f; // clamp; 0 = rooted
}

void CharacterController::setUp(glm::vec3 up) {
    const float len = glm::length(up);
    if (len < 1e-4f) return;
    m_impl->up = up / len;
    if (m_impl->character) {
        // Jolt CharacterVirtual::SetUp reorients ground detection for planetoids / habitats.
        // Supporting volume is fixed at construction (settings.mSupportingVolume).
        m_impl->character->SetUp(JPH::Vec3(m_impl->up.x, m_impl->up.y, m_impl->up.z));
    }
}

glm::vec3 CharacterController::up() const { return m_impl->up; }

glm::vec3 CharacterController::position() const {
    if (!m_impl->character)
        return glm::vec3(0.0f);
    const JPH::RVec3 p = m_impl->character->GetPosition();
    return glm::vec3(p.GetX(), p.GetY(), p.GetZ());
}

glm::vec3 CharacterController::velocity() const {
    if (!m_impl->character)
        return glm::vec3(0.0f);
    const JPH::Vec3 v = m_impl->character->GetLinearVelocity();
    return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
}

void CharacterController::setState(glm::vec3 feetPos, glm::vec3 vel) {
    if (!m_impl->character) return;
    m_impl->character->SetPosition(JPH::RVec3(feetPos.x, feetPos.y, feetPos.z));
    m_impl->character->SetLinearVelocity(JPH::Vec3(vel.x, vel.y, vel.z));
}

bool CharacterController::onGround() const {
    return m_impl->character && m_impl->character->IsSupported();
}

bool CharacterController::crouched() const { return m_impl->crouched; }

float CharacterController::eyeHeight() const { return m_impl->eyeHeight; }

} // namespace meat
