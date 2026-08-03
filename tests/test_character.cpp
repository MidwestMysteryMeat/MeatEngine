// CharacterController speed-scale enforcement, tested in isolation on a flat
// static floor (no game world, no async meshing). The ApplyModifier effect's
// speed half now drives setSpeedScale on the server, so this proves the
// mechanism it relies on: a larger scale moves the character proportionally
// farther over identical forward input.

#include "Harness.h"

#include "engine/physics/CharacterController.h"
#include "engine/physics/PhysicsWorld.h"
#include "engine/platform/Input.h"

#include <cmath>

namespace {

using meattest::check;

constexpr float kDt = 1.0f / 60.0f;

// Distance the character walks forward over a fixed stretch at a given speed
// scale, starting from terminal velocity (so the accel ramp isn't measured).
float forwardDistance(float scale) {
    meat::PhysicsWorld world;
    if (!world.init()) return -1.0f;
    world.addStaticBox({0.0f, -1.0f, 0.0f}, {50.0f, 1.0f, 50.0f}); // floor, top at y=0

    meat::CharacterController cc;
    if (!cc.init(world, {0.0f, 0.5f, 0.0f})) return -1.0f;

    const meat::PlayerCommand idle{};
    meat::PlayerCommand fwd{};
    fwd.move = {0.0f, 1.0f};

    auto tickWith = [&](const meat::PlayerCommand& c) {
        cc.update(c, kDt, world);
        world.step(kDt);
    };

    for (int i = 0; i < 90; ++i) tickWith(idle); // settle onto the floor
    cc.setSpeedScale(scale);
    for (int i = 0; i < 40; ++i) tickWith(fwd); // reach terminal at this scale
    const glm::vec3 start = cc.position();
    for (int i = 0; i < 60; ++i) tickWith(fwd); // measured stretch
    const glm::vec3 end = cc.position();
    const glm::vec3 d = end - start;
    return std::sqrt(d.x * d.x + d.z * d.z);
}

void testSpeedScaleAffectsMovement() {
    std::printf("CharacterController speed scale changes movement distance\n");
    const float base = forwardDistance(1.0f);
    const float fast = forwardDistance(2.0f);
    check(base > 1.0f, "the character moves forward at normal scale");
    check(fast > base * 1.6f, "2x speed scale covers ~2x the distance");
    check(forwardDistance(0.5f) < base * 0.75f, "0.5x speed scale covers less ground");
}

} // namespace

namespace meattest {

void runCharacter() {
    testSpeedScaleAffectsMovement();
}

} // namespace meattest
