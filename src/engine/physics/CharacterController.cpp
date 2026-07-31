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

    // Input → desired horizontal velocity. -Z is forward at yaw 0 (Y-up,
    // right-handed): forward = (-sin yaw, 0, -cos yaw), right = (cos yaw, 0, -sin yaw).
    glm::vec2 move = cmd.move;
    const float moveLen = glm::length(move);
    if (moveLen > 1.0f)
        move /= moveLen;
    const float sinYaw = std::sin(cmd.yaw);
    const float cosYaw = std::cos(cmd.yaw);
    const glm::vec2 forward(-sinYaw, -cosYaw); // (x, z)
    const glm::vec2 right(cosYaw, -sinYaw);
    const float speed = im.crouched ? t.crouchSpeed : (cmd.sprint ? t.sprintSpeed : t.walkSpeed);
    const glm::vec2 want = (right * move.x + forward * move.y) * speed;

    const JPH::Vec3 curVel = im.character->GetLinearVelocity();
    glm::vec2 horiz(curVel.GetX(), curVel.GetZ());
    // Movement/jump use strictly OnGround; OnSteepGround is treated as air so
    // gravity keeps sliding the character off slopes past maxSlopeDeg.
    const bool grounded =
        im.character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;

    // Ground accel model: horizontal velocity chases the target at
    // groundAccelScale * targetSpeed per second — full walk speed in ~0.1 s,
    // snappy without being an instant teleport. Airborne it keeps momentum:
    // steer at airControl fraction only while there is input, never brake.
    if (grounded || moveLen > 0.0f) {
        const float accelRate = t.groundAccelScale * speed * (grounded ? 1.0f : t.airControl);
        const glm::vec2 delta = want - horiz;
        const float deltaLen = glm::length(delta);
        const float maxStep = accelRate * fixedDt;
        horiz = deltaLen <= maxStep ? want : horiz + delta * (maxStep / deltaLen);
    }

    float vy = curVel.GetY();
    if (grounded && vy <= 0.1f) { // not already moving up (fresh jump keeps its velocity)
        vy = 0.0f;
        if (cmd.jump && !im.jumpHeld)
            vy = t.jumpSpeed;
    } else {
        vy += t.gravity * fixedDt;
    }
    im.jumpHeld = cmd.jump;

    im.character->SetLinearVelocity(JPH::Vec3(horiz.x, vy, horiz.y));

    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
    updateSettings.mWalkStairsStepUp = JPH::Vec3(0.0f, t.stepUp, 0.0f);
    // mStickToFloorStepDown keeps its Jolt default (0,-0.5,0): a little deeper
    // than stepUp so walking down stairs/slopes never enters a fall.
    im.character->ExtendedUpdate(fixedDt, JPH::Vec3(0.0f, t.gravity, 0.0f), updateSettings,
                                 system.GetDefaultBroadPhaseLayerFilter(objlayer::kMoving),
                                 system.GetDefaultLayerFilter(objlayer::kMoving), {}, {},
                                 tempAlloc);

    const float targetEye = im.crouched ? t.eyeCrouch : t.eyeStand;
    im.eyeHeight += (targetEye - im.eyeHeight) * std::min(1.0f, t.eyeLerpRate * fixedDt);
}

void CharacterController::setGravity(float gravityY) { m_impl->tuning.gravity = gravityY; }

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
