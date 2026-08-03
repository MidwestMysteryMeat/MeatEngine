// Dungeon determinism: like terrain, the dungeon layout is a pure function of
// (seed, params) that every peer computes locally — no dungeon data crosses the
// wire. If two peers derive different rooms, players fall through floors that
// aren't there on someone else's machine. These tests pin that invariant plus
// the basic carve/solid queries the chunk generator relies on.

#include "Harness.h"

#include "game/DungeonGen.h"

#include <cstdio>

namespace {

using meattest::check;

bool layoutsEqual(const meat::DungeonLayout& a, const meat::DungeonLayout& b) {
    if (a.rooms().size() != b.rooms().size()) return false;
    if (a.entranceTop() != b.entranceTop()) return false;
    for (std::size_t i = 0; i < a.rooms().size(); ++i) {
        if (a.rooms()[i].min != b.rooms()[i].min) return false;
        if (a.rooms()[i].max != b.rooms()[i].max) return false;
    }
    return true;
}

void testDeterministic() {
    std::printf("same seed reproduces the dungeon layout exactly\n");
    meat::DungeonParams p;
    const auto a = meat::DungeonLayout::generate(1337u, p);
    const auto b = meat::DungeonLayout::generate(1337u, p);
    check(layoutsEqual(a, b), "identical rooms + entrance from the same seed");
    check(!a.rooms().empty(), "the dungeon actually has rooms (not vacuous)");
}

void testSeedMatters() {
    std::printf("a different seed yields a different dungeon\n");
    meat::DungeonParams p;
    const auto a = meat::DungeonLayout::generate(1u, p);
    const auto b = meat::DungeonLayout::generate(2u, p);
    check(!layoutsEqual(a, b), "the seed actually drives layout");
}

void testRoomsWithinBounds() {
    std::printf("every room stays inside the configured area\n");
    meat::DungeonParams p;
    const auto d = meat::DungeonLayout::generate(1337u, p);
    bool inBounds = true;
    for (const auto& r : d.rooms()) {
        if (r.min.x < p.areaMin.x || r.max.x > p.areaMax.x || r.min.y < p.areaMin.y ||
            r.max.y > p.areaMax.y || r.min.z < p.areaMin.z || r.max.z > p.areaMax.z)
            inBounds = false;
    }
    check(inBounds, "no room escapes areaMin..areaMax");
}

void testCarveQueries() {
    std::printf("room interiors are carved air; the outside is solid\n");
    meat::DungeonParams p;
    const auto d = meat::DungeonLayout::generate(1337u, p);
    if (d.rooms().empty()) { check(false, "dungeon has rooms"); return; }

    const auto& r0 = d.rooms()[0];
    const glm::ivec3 center = (r0.min + r0.max) / 2;
    const auto here = d.boxesIntersecting(center, center);
    check(d.isAir(center, here), "the center of a room is carved air");

    const glm::ivec3 outside{100000, 0, 100000}; // far beyond any room
    const auto none = d.boxesIntersecting(outside, outside);
    check(!d.isAir(outside, none), "a point outside every carve is solid");
}

} // namespace

namespace meattest {

void runDungeon() {
    testDeterministic();
    testSeedMatters();
    testRoomsWithinBounds();
    testCarveQueries();
}

} // namespace meattest
