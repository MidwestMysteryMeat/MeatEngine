// Codec tests for the snapshot delta encoder — the wire format two peers must
// agree on byte-for-byte. These are pure round-trips: encode a snapshot against
// a baseline, decode with the SAME baseline, and compare. No server, no sockets.
//
// The position-precision cases are the regression net for the protocol v3 change
// that replaced the 16-bit-over-±2048 m scheme (which silently teleported distant
// players back toward the origin) with signed 24-bit fixed-point at ±32 km.

#include "Harness.h"

#include "engine/net/ByteStream.h"
#include "engine/net/DeltaSnapshot.h"
#include "engine/net/Messages.h"

#include <cmath>
#include <cstdio>

namespace {

using meattest::check;

// Encode `cur` against `baseline`, then decode with that same baseline.
meat::SnapshotMsg roundTrip(const meat::SnapshotMsg& cur, const meat::SnapshotMsg& baseline) {
    meat::ByteWriter w;
    meat::encodeDelta(cur, baseline, w);
    meat::ByteReader r(w.data());
    meat::SnapshotMsg out;
    if (!meat::decodeDelta(out, baseline, r)) {
        check(false, "decodeDelta succeeded");
        return {};
    }
    return out;
}

// Positions survive the fixed-point round-trip within the ~3.9 mm quantization step.
constexpr float kPosTol = 1.0f / 256.0f;

meat::PlayerState player(meat::PeerId id, glm::vec3 pos) {
    meat::PlayerState p;
    p.playerId = id;
    p.pos = pos;
    p.health = 100.0f;
    return p;
}

bool approxEq(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
bool nearPos(const glm::vec3& a, const glm::vec3& b) {
    return approxEq(a.x, b.x, kPosTol) && approxEq(a.y, b.y, kPosTol) &&
           approxEq(a.z, b.z, kPosTol);
}

void testKeyframeRoundTrip() {
    std::printf("a keyframe round-trips every field\n");
    meat::SnapshotMsg snap;
    snap.tick = 42;
    snap.players.push_back(player(1, {1.5f, 2.25f, -3.75f}));
    snap.players.push_back(player(2, {10.0f, 0.0f, 10.0f}));
    meat::EntityState e;
    e.id = 7;
    e.archetype = 3;
    e.pos = {5.0f, 6.0f, 7.0f};
    e.health = 50.0f;
    snap.entities.push_back(e);

    const meat::SnapshotMsg out = roundTrip(snap, meat::SnapshotMsg{});
    check(out.tick == 42, "tick preserved");
    check(out.players.size() == 2, "both players present");
    check(out.entities.size() == 1, "entity present");
    if (out.players.size() == 2) {
        check(nearPos(out.players[0].pos, {1.5f, 2.25f, -3.75f}), "player 1 position");
        check(nearPos(out.players[1].pos, {10.0f, 0.0f, 10.0f}), "player 2 position");
    }
}

void testDistantPositionSurvives() {
    std::printf("a position far past the old ±2048 m clamp survives\n");
    // These all used to fold back toward the origin under the 16-bit scheme.
    const glm::vec3 distant[] = {
        {5000.0f, 800.0f, -9001.0f},
        {-20000.0f, 1234.5f, 20000.0f},
        {31000.0f, -31000.0f, 100.0f},
    };
    for (const glm::vec3& p : distant) {
        meat::SnapshotMsg snap;
        snap.players.push_back(player(1, p));
        const meat::SnapshotMsg out = roundTrip(snap, meat::SnapshotMsg{});
        if (out.players.size() != 1) { check(false, "player present"); continue; }
        check(nearPos(out.players[0].pos, p), "far position round-trips (no ±2048 m clamp)");
    }
}

void testNegativePrecision() {
    std::printf("negative coordinates keep sub-centimeter precision\n");
    meat::SnapshotMsg snap;
    snap.players.push_back(player(1, {-0.5f, -100.125f, -0.00390625f}));
    const meat::SnapshotMsg out = roundTrip(snap, meat::SnapshotMsg{});
    if (out.players.size() == 1)
        check(nearPos(out.players[0].pos, {-0.5f, -100.125f, -0.00390625f}),
              "negative position round-trips within a step");
}

void testDeltaSkipsUnchanged() {
    std::printf("an unchanged player against its baseline shrinks the payload\n");
    meat::SnapshotMsg base;
    base.tick = 1;
    base.players.push_back(player(1, {3.0f, 4.0f, 5.0f}));

    meat::SnapshotMsg moved = base;
    moved.tick = 2;
    moved.players[0].pos = {3.5f, 4.0f, 5.0f}; // only x changed

    meat::ByteWriter full;
    meat::encodeDelta(moved, meat::SnapshotMsg{}, full); // vs empty = keyframe
    meat::ByteWriter delta;
    meat::encodeDelta(moved, base, delta); // vs the near-identical baseline
    check(delta.size() < full.size(), "the delta is smaller than the keyframe");

    const meat::SnapshotMsg out = roundTrip(moved, base);
    if (out.players.size() == 1)
        check(nearPos(out.players[0].pos, {3.5f, 4.0f, 5.0f}), "the moved position decodes");
}

void testRemovalIsReplicated() {
    std::printf("a player absent from the new snapshot is removed against baseline\n");
    meat::SnapshotMsg base;
    base.players.push_back(player(1, {0, 0, 0}));
    base.players.push_back(player(2, {1, 1, 1}));

    meat::SnapshotMsg fewer;
    fewer.tick = 2;
    fewer.players.push_back(player(1, {0, 0, 0})); // player 2 gone

    const meat::SnapshotMsg out = roundTrip(fewer, base);
    check(out.players.size() == 1, "only the surviving player remains");
    if (out.players.size() == 1) check(out.players[0].playerId == 1, "the right player survived");
}

} // namespace

namespace meattest {

void runDeltaSnapshot() {
    testKeyframeRoundTrip();
    testDistantPositionSurvives();
    testNegativePrecision();
    testDeltaSkipsUnchanged();
    testRemovalIsReplicated();
}

} // namespace meattest
