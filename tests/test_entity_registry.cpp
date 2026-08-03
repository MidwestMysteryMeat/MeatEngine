// Unit tests for EntityRegistry — the project's hand-rolled generational-handle
// + sparse-set component store. It is not yet the live entity system (ServerSim
// still uses a plain id counter), so these tests are what keep it correct and
// adoption-ready rather than rotting as unexercised infrastructure.

#include "Harness.h"

#include "engine/core/EntityRegistry.h"

#include <cstdio>

namespace {

using meattest::check;

struct Position {
    float x = 0, y = 0, z = 0;
};
struct Velocity {
    float dx = 0;
};

void testCreateAndAlive() {
    std::printf("fresh handles are alive and distinct\n");
    meat::EntityRegistry reg;
    const meat::EntityId a = reg.create();
    const meat::EntityId b = reg.create();
    check(a != meat::kInvalidEntity && b != meat::kInvalidEntity, "ids are non-zero");
    check(a != b, "two creates yield distinct ids");
    check(reg.alive(a) && reg.alive(b), "both are alive");
    check(!reg.alive(meat::kInvalidEntity), "the invalid id is never alive");
}

void testStaleHandleAfterReuse() {
    std::printf("a destroyed handle stays dead even when its slot is reused\n");
    meat::EntityRegistry reg;
    const meat::EntityId first = reg.create();
    reg.destroy(first);
    check(!reg.alive(first), "the destroyed handle is dead");
    // The next create reuses the freed slot but bumps its generation, so the old
    // handle must not resurrect — this is the whole point of the generation bits.
    const meat::EntityId second = reg.create();
    check(reg.alive(second), "the reused slot's new handle is alive");
    check(second != first, "the new handle differs from the stale one");
    check(!reg.alive(first), "the stale handle is still dead after slot reuse");
}

void testComponents() {
    std::printf("components attach, read back, and detach per entity\n");
    meat::EntityRegistry reg;
    const meat::EntityId e = reg.create();
    reg.add<Position>(e, {1.0f, 2.0f, 3.0f});
    Position* p = reg.get<Position>(e);
    check(p != nullptr && p->y == 2.0f, "an added component reads back");
    check(reg.get<Velocity>(e) == nullptr, "an unattached component type is null");

    reg.remove<Position>(e);
    check(reg.get<Position>(e) == nullptr, "a removed component is gone");

    // A component on a destroyed entity must not survive into a reused slot.
    reg.add<Position>(e, {9.0f, 9.0f, 9.0f});
    reg.destroy(e);
    const meat::EntityId reused = reg.create();
    check(reg.get<Position>(reused) == nullptr, "destroy() drops the entity's components");
}

void testEachIteratesMatching() {
    std::printf("each() visits exactly the entities holding all listed components\n");
    meat::EntityRegistry reg;
    const meat::EntityId ab = reg.create(); // Position + Velocity
    const meat::EntityId aOnly = reg.create(); // Position only
    reg.add<Position>(ab, {0, 0, 0});
    reg.add<Velocity>(ab, {5.0f});
    reg.add<Position>(aOnly, {0, 0, 0});

    int bothCount = 0;
    meat::EntityId seen = meat::kInvalidEntity;
    reg.each<Velocity, Position>([&](meat::EntityId id, Velocity& v, Position&) {
        ++bothCount;
        seen = id;
        v.dx += 1.0f;
    });
    check(bothCount == 1, "only the entity with both components is visited");
    check(seen == ab, "and it is the right one");
    check(reg.get<Velocity>(ab)->dx == 6.0f, "the callback mutates through the reference");

    int posCount = 0;
    reg.each<Position>([&](meat::EntityId, Position&) { ++posCount; });
    check(posCount == 2, "single-component iteration sees both position holders");
}

} // namespace

namespace meattest {

void runEntityRegistry() {
    testCreateAndAlive();
    testStaleHandleAfterReuse();
    testComponents();
    testEachIteratesMatching();
}

} // namespace meattest
