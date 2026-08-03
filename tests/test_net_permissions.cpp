// Headless tests for the multiplayer trust boundary.
//
// These run without a GPU, a window or a real socket: ServerSim talks to a
// LoopbackPair, and the test plays the part of the client by writing packets
// straight onto the wire. That matters here — the point is to send packets a
// legitimate client would never send, which is exactly what an attacker does.
//
// The assertion in every case is about the authoritative world, not about a
// return value. A permission check that returns "denied" while the block still
// changed would pass a mock-based test and fail reality.

#include "Harness.h"

#include "engine/net/DeltaSnapshot.h"
#include "engine/net/LoopbackTransport.h"
#include "engine/net/Messages.h"
#include "game/ServerSim.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using meattest::check;

// A server plus the wire its one peer talks over. The peer is connected from
// construction, which mirrors how single-player boots.
struct Fixture {
    meat::LoopbackPair wire;
    meat::ServerSim server;
    const meat::PeerId peer = 1;

    explicit Fixture(bool allowRemoteEditing = true,
                     std::string token = "test-editor-token") {
        meat::NetPolicy policy;
        policy.allowRemoteEditing = allowRemoteEditing;
        policy.editorToken = std::move(token);
        server.setNetPolicy(policy);
    }

    bool boot() { return server.init(1234u); }

    // Deliver a packet to the server as if this peer had sent it, then let the
    // server drain its queue.
    template <typename Msg>
    void sendToServer(const Msg& msg) {
        wire.clientEnd().send(1, meat::pack(msg), true);
        server.pump(wire.serverEnd());
    }

    void pump() { server.pump(wire.serverEnd()); }
    void tick() { server.tick(wire.serverEnd()); }

    // Say hello as an ordinary player, or as the session owner.
    void hello(const std::string& editorToken) {
        pump(); // consume the Connected event so the peer exists server-side
        sendToServer(meat::HelloMsg{meat::kProtocolVersion, "tester", editorToken});
    }

    meat::BlockId blockAt(glm::ivec3 v) const { return server.voxels().blockAt(v); }
};

// A voxel far from spawn, in a chunk the server has generated. Chosen well away
// from the player capsule so nothing but the edit under test can change it.
constexpr glm::ivec3 kTarget{40, 40, 40};

void testPlayerCannotEditVoxels() {
    std::printf("server rejects a voxel edit from an ordinary player\n");
    Fixture f;
    if (!f.boot()) { check(false, "server booted"); return; }
    f.hello(""); // no token: an ordinary player

    const meat::BlockId before = f.blockAt(kTarget);
    f.sendToServer(meat::VoxelOpMsg{kTarget, 1});
    check(f.blockAt(kTarget) == before,
          "world is unchanged after an unauthorised VoxelOp");
}

void testEditorCanEditVoxels() {
    std::printf("server accepts a voxel edit from the token holder\n");
    Fixture f;
    if (!f.boot()) { check(false, "server booted"); return; }
    f.hello("test-editor-token");

    const meat::BlockId before = f.blockAt(kTarget);
    const meat::BlockId placed = (before == 1) ? meat::BlockId{2} : meat::BlockId{1};
    f.sendToServer(meat::VoxelOpMsg{kTarget, placed});
    check(f.blockAt(kTarget) == placed,
          "world changes after an authorised VoxelOp");
}

void testWrongTokenIsStillAPlayer() {
    std::printf("a wrong token grants nothing\n");
    Fixture f;
    if (!f.boot()) { check(false, "server booted"); return; }
    f.hello("not-the-token");

    const meat::BlockId before = f.blockAt(kTarget);
    f.sendToServer(meat::VoxelOpMsg{kTarget, 1});
    check(f.blockAt(kTarget) == before, "world is unchanged with a wrong token");
}

void testPolicyOffRefusesTheRightToken() {
    std::printf("allowRemoteEditing=false refuses even the correct token\n");
    Fixture f(false); // a dedicated server's default posture
    if (!f.boot()) { check(false, "server booted"); return; }
    f.hello("test-editor-token");

    const meat::BlockId before = f.blockAt(kTarget);
    f.sendToServer(meat::VoxelOpMsg{kTarget, 1});
    check(f.blockAt(kTarget) == before,
          "world is unchanged when the policy disables editing");
}

void testPropEditsNeedPermission() {
    std::printf("prop authoring needs the same permission\n");
    Fixture f;
    if (!f.boot()) { check(false, "server booted"); return; }
    f.hello("");

    const std::size_t before = f.server.propCount();
    f.sendToServer(meat::PlacePropMsg{"assets/models/prop_crate.obj", glm::mat4(1.0f)});
    check(f.server.propCount() == before,
          "no prop is created by an unauthorised PlaceProp");
}

// The control for every "no prop was created" assertion above and below. If the
// asset stopped loading, those would all pass for the wrong reason and this one
// would fail — which is exactly what it is here to catch.
void testEditorCanPlaceProp() {
    std::printf("a valid prop from the token holder is created\n");
    Fixture f;
    if (!f.boot()) { check(false, "server booted"); return; }
    f.hello("test-editor-token");

    const std::size_t before = f.server.propCount();
    f.sendToServer(meat::PlacePropMsg{"assets/models/prop_crate.obj", glm::mat4(1.0f)});
    check(f.server.propCount() == before + 1,
          "an authorised PlaceProp with a sane transform creates a prop");
}

void testMalformedTransformsAreRefused() {
    std::printf("prop transforms are validated before they reach physics\n");
    Fixture f;
    if (!f.boot()) { check(false, "server booted"); return; }
    f.hello("test-editor-token");

    const std::size_t before = f.server.propCount();

    glm::mat4 nan(1.0f);
    nan[3][0] = std::numeric_limits<float>::quiet_NaN();
    f.sendToServer(meat::PlacePropMsg{"assets/models/prop_crate.obj", nan});
    check(f.server.propCount() == before, "a NaN position is refused");

    glm::mat4 inf(1.0f);
    inf[3][1] = std::numeric_limits<float>::infinity();
    f.sendToServer(meat::PlacePropMsg{"assets/models/prop_crate.obj", inf});
    check(f.server.propCount() == before, "an infinite position is refused");

    glm::mat4 zeroScale(1.0f);
    zeroScale[0][0] = 0.0f;
    zeroScale[1][1] = 0.0f;
    zeroScale[2][2] = 0.0f;
    f.sendToServer(meat::PlacePropMsg{"assets/models/prop_crate.obj", zeroScale});
    check(f.server.propCount() == before, "a degenerate scale is refused");

    glm::mat4 huge(1.0f);
    huge[0][0] = 1.0e9f;
    f.sendToServer(meat::PlacePropMsg{"assets/models/prop_crate.obj", huge});
    check(f.server.propCount() == before, "an enormous scale is refused");

    glm::mat4 far(1.0f);
    far[3][2] = 1.0e9f;
    f.sendToServer(meat::PlacePropMsg{"assets/models/prop_crate.obj", far});
    check(f.server.propCount() == before, "a position far outside the world is refused");
}

void testAssetPathTraversalIsRefused() {
    std::printf("asset paths cannot escape the assets directory\n");
    Fixture f;
    if (!f.boot()) { check(false, "server booted"); return; }
    f.hello("test-editor-token");

    const std::size_t before = f.server.propCount();
    f.sendToServer(meat::PlacePropMsg{"assets/../../../etc/passwd", glm::mat4(1.0f)});
    check(f.server.propCount() == before, "a traversing path is refused");
    f.sendToServer(meat::PlacePropMsg{"/etc/passwd", glm::mat4(1.0f)});
    check(f.server.propCount() == before, "an absolute path is refused");
}

void testVoxelEditsAreRateLimited() {
    std::printf("an authorised peer cannot edit without limit\n");
    Fixture f;
    if (!f.boot()) { check(false, "server booted"); return; }
    f.hello("test-editor-token");

    // Far more edits than the bucket holds, with no tick in between to refill
    // it. Each targets its own voxel so the count of changes is the count of
    // edits that were actually accepted.
    constexpr int kAttempts = 400;
    int applied = 0;
    for (int i = 0; i < kAttempts; ++i) {
        const glm::ivec3 v{100 + i, 60, 60};
        f.sendToServer(meat::VoxelOpMsg{v, 1});
        if (f.blockAt(v) == 1) ++applied;
    }
    check(applied > 0, "some edits are accepted");
    check(applied < kAttempts, "the burst is cut off by the rate limiter");
    std::printf("        (%d of %d accepted)\n", applied, kAttempts);
}

void testProtocolMismatchIsRefused() {
    std::printf("a client on the wrong protocol cannot author the world\n");
    Fixture f;
    if (!f.boot()) { check(false, "server booted"); return; }
    f.pump();
    // Right token, wrong protocol: the Hello is refused before the token is
    // ever compared, so no rights are granted.
    f.sendToServer(meat::HelloMsg{static_cast<std::uint16_t>(meat::kProtocolVersion + 1),
                                  "tester", "test-editor-token"});

    const meat::BlockId before = f.blockAt(kTarget);
    f.sendToServer(meat::VoxelOpMsg{kTarget, 1});
    check(f.blockAt(kTarget) == before, "world is unchanged after a rejected Hello");
}

void testMalformedPacketsDoNotCrash() {
    std::printf("truncated and garbage packets are survivable\n");
    Fixture f;
    if (!f.boot()) { check(false, "server booted"); return; }
    f.hello("test-editor-token");

    // Every message type, truncated to one byte and to an odd length, plus a
    // type byte that does not exist. None of these may crash or mutate anything.
    const meat::BlockId before = f.blockAt(kTarget);
    for (int type = 0; type < 32; ++type) {
        for (std::size_t len = 1; len < 12; ++len) {
            std::vector<std::byte> junk(len, std::byte{0xAB});
            junk[0] = static_cast<std::byte>(type);
            f.wire.clientEnd().send(1, junk, true);
        }
    }
    f.pump();
    check(f.blockAt(kTarget) == before, "no world change from malformed packets");
    check(true, "server survived every malformed packet");
}

// A hostile client is not bound by int overflow etiquette. abs(INT_MIN) is
// still negative, so a naive |v| > max range check lets it through and the
// coordinate reaches chunk indexing as garbage — this is the regression test
// for exactly that bypass.
void testExtremeVoxelCoordinatesAreRefused() {
    std::printf("integer-limit voxel coordinates cannot reach the world\n");
    Fixture f;
    if (!f.boot()) { check(false, "server booted"); return; }
    f.hello("test-editor-token");

    constexpr int kMin = std::numeric_limits<int>::min();
    constexpr int kMax = std::numeric_limits<int>::max();
    const glm::ivec3 hostile[] = {
        {kMin, 40, 40}, {40, kMin, 40}, {40, 40, kMin},
        {kMax, 40, 40}, {40, kMax, 40}, {40, 40, kMax},
        {kMin, kMin, kMin}, {kMax, kMax, kMax},
    };
    for (const glm::ivec3& v : hostile) f.sendToServer(meat::VoxelOpMsg{v, 1});

    // The server must both survive and still function: a legitimate edit after
    // the barrage proves no state was corrupted along the way.
    const meat::BlockId before = f.blockAt(kTarget);
    const meat::BlockId placed = (before == 1) ? meat::BlockId{2} : meat::BlockId{1};
    f.sendToServer(meat::VoxelOpMsg{kTarget, placed});
    check(f.blockAt(kTarget) == placed,
          "the server still applies valid edits after integer-limit coordinates");
}

// LoopbackPair only ever has one peer, but Move/Remove permission checks need a
// prop that exists while an UNAUTHORISED peer attacks it — so this transport
// scripts events for two peers and counts what the server sends back.
struct ScriptedTransport final : meat::Transport {
    std::vector<meat::NetEvent> pending;
    int packetsSent = 0;
    // Latest snapshot the server sent to each peer, reconstructed through the
    // real delta codec exactly the way a client would (per-peer baseline ring).
    std::unordered_map<meat::PeerId, meat::SnapshotMsg> lastSnapshot;
    std::unordered_map<meat::PeerId, std::map<std::uint64_t, meat::SnapshotMsg>> ring;

    void poll(std::vector<meat::NetEvent>& out) override {
        out.insert(out.end(), pending.begin(), pending.end());
        pending.clear();
    }
    void send(meat::PeerId peer, std::span<const std::byte> bytes, bool) override {
        ++packetsSent;
        const std::optional<meat::MsgType> type = meat::peekType(bytes);
        if (!type || *type != meat::MsgType::DeltaSnapshot) return;
        meat::ByteReader r(bytes.subspan(1));
        const std::optional<std::uint64_t> baseTick = meat::peekDeltaBaseline(r);
        if (!baseTick) return;
        static const meat::SnapshotMsg kEmpty{};
        const meat::SnapshotMsg* base = &kEmpty;
        if (*baseTick != 0) {
            const auto it = ring[peer].find(*baseTick);
            if (it == ring[peer].end()) return; // baseline lost: skip like a client would
            base = &it->second;
        }
        meat::SnapshotMsg snap;
        if (!meat::decodeDelta(snap, *base, r)) return;
        auto& peerRing = ring[peer];
        peerRing[snap.tick] = snap;
        while (peerRing.size() > 32) peerRing.erase(peerRing.begin());
        lastSnapshot[peer] = snap;
    }
    void disconnect(meat::PeerId) override {}

    void connect(meat::PeerId p) {
        pending.push_back({meat::NetEvent::Type::Connected, p, {}});
    }
    template <typename Msg>
    void packet(meat::PeerId p, const Msg& msg) {
        pending.push_back({meat::NetEvent::Type::Packet, p, meat::pack(msg)});
    }
};

void testMoveAndRemovePropNeedPermission() {
    std::printf("moving/removing an existing prop needs permission\n");
    ScriptedTransport wire;
    meat::ServerSim server;
    meat::NetPolicy policy;
    policy.allowRemoteEditing = true;
    policy.editorToken = "test-editor-token";
    server.setNetPolicy(policy);
    if (!server.init(1234u)) { check(false, "server booted"); return; }

    // Peer 1 is the session editor and seeds the prop under attack (id 1 —
    // the server's first assigned id).
    wire.connect(1);
    wire.packet(1, meat::HelloMsg{meat::kProtocolVersion, "editor", "test-editor-token"});
    wire.packet(1, meat::PlacePropMsg{"assets/models/prop_crate.obj", glm::mat4(1.0f)});
    server.pump(wire);
    check(server.propCount() == 1, "the editor seeded one prop");

    // Peer 2 is an ordinary player. A successful move/remove is always
    // broadcast (PropAdded / PropRemoved), and a rejection sends nothing —
    // so the packet count is the observable trust boundary for MoveProp.
    wire.connect(2);
    wire.packet(2, meat::HelloMsg{meat::kProtocolVersion, "player", ""});
    server.pump(wire);

    glm::mat4 moved(1.0f);
    moved[3][0] = 5.0f;
    const int sendsBefore = wire.packetsSent;
    wire.packet(2, meat::MovePropMsg{1, moved});
    server.pump(wire);
    check(wire.packetsSent == sendsBefore,
          "an unauthorised MoveProp is not applied or broadcast");

    wire.packet(2, meat::RemovePropMsg{1});
    server.pump(wire);
    check(server.propCount() == 1, "an unauthorised RemoveProp deletes nothing");

    // Control: the same messages from the token holder do go through, proving
    // the denials above failed for the right reason.
    const int sendsBeforeEditor = wire.packetsSent;
    wire.packet(1, meat::MovePropMsg{1, moved});
    server.pump(wire);
    check(wire.packetsSent > sendsBeforeEditor, "an authorised MoveProp is broadcast");

    wire.packet(1, meat::RemovePropMsg{1});
    server.pump(wire);
    check(server.propCount() == 0, "an authorised RemoveProp deletes the prop");
}

// F2 lag compensation. The victim sprints one way, turns, and keeps sprinting;
// the shooter fires at where the victim stood in an OLD snapshot. With the ack
// of that old tick piggybacked on the fire command the shot must land (the
// server rewinds the capsule); the same aim with a fresh view must miss.
void testHitscanIsLagCompensated() {
    std::printf("hitscan rewinds targets to the shooter's acked snapshot\n");
    ScriptedTransport wire;
    meat::GameRules rules;
    rules.terrain = meat::GameRules::Terrain::Superflat; // clean firing lane
    meat::ServerSim server(rules);
    if (!server.init(777u)) { check(false, "server booted"); return; }

    wire.connect(1);
    wire.packet(1, meat::HelloMsg{meat::kProtocolVersion, "shooter", ""});
    wire.connect(2);
    wire.packet(2, meat::HelloMsg{meat::kProtocolVersion, "victim", ""});
    server.pump(wire);

    std::uint64_t cmdTick1 = 0, cmdTick2 = 0;
    auto step = [&](const meat::PlayerCommand& shooterCmd, const meat::PlayerCommand& victimCmd,
                    std::uint64_t shooterAck) {
        meat::PlayerCommand c1 = shooterCmd, c2 = victimCmd;
        c1.tick = ++cmdTick1;
        c2.tick = ++cmdTick2;
        wire.packet(1, meat::CommandMsg{c1, shooterAck});
        wire.packet(2, meat::CommandMsg{c2, 0});
        server.pump(wire);
        server.tick(wire);
    };
    const meat::PlayerCommand idle{};
    meat::PlayerCommand sprintX{}; // victim leg 1: away from spawn along -X
    sprintX.move = {0.0f, 1.0f};
    sprintX.sprint = true;
    sprintX.yaw = 1.5707963f;
    meat::PlayerCommand sprintZ = sprintX; // victim leg 2: perpendicular, along -Z
    sprintZ.yaw = 0.0f;

    auto stateOf = [&](meat::PeerId dest, meat::PeerId playerId) -> const meat::PlayerState* {
        for (const meat::PlayerState& p : wire.lastSnapshot[dest].players)
            if (p.playerId == playerId) return &p;
        return nullptr;
    };

    // Chunk colliders are built on worker threads. Rather than guess a wall-clock
    // budget (unreliable under ASan's much slower instrumented mesher), prime the
    // spawn stream, then wait DETERMINISTICALLY for the mesh queue to drain, then
    // let the victim settle onto the now-present collider.
    auto waitVictimGrounded = [&](int /*unused*/) {
        for (int i = 0; i < 30; ++i) step(idle, idle, 0); // prime spawn-area meshing
        for (int i = 0; i < 8000 && !server.meshingIdle(); ++i) {
            step(idle, idle, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (int i = 0; i < 300; ++i) {
            const meat::PlayerState* v = stateOf(1, 2);
            if (v && v->onGround) return true;
            step(idle, idle, 0);
        }
        return false;
    };

    for (int i = 0; i < 10; ++i) step(idle, idle, 0); // spawn
    if (!waitVictimGrounded(1000)) { check(false, "victim grounded at spawn"); return; }
    for (int i = 0; i < 150; ++i) step(idle, sprintX, 0);  // open ~10 m of distance
    if (!waitVictimGrounded(1000)) { check(false, "victim grounded after the run"); return; }

    // The victim now stands still; the next fresh snapshot is the shooter's
    // "view": T0, with the victim exactly where the pose history has them.
    const std::uint64_t seen = wire.lastSnapshot[1].tick;
    for (int i = 0; i < 8 && wire.lastSnapshot[1].tick == seen; ++i) step(idle, idle, 0);
    if (wire.lastSnapshot[1].tick == seen) { check(false, "a fresh snapshot arrived"); return; }
    const std::uint64_t viewTick = wire.lastSnapshot[1].tick;
    const meat::PlayerState* victimThen = stateOf(1, 2);
    const meat::PlayerState* shooterNow = stateOf(1, 1);
    if (!victimThen || !shooterNow) { check(false, "snapshot carries both players"); return; }
    const glm::vec3 aimTarget = victimThen->pos + glm::vec3(0, 0.9f, 0); // capsule middle
    const glm::vec3 eye = shooterNow->pos + glm::vec3(0, 1.62f, 0);

    // The victim keeps sprinting perpendicular through the lag window (well
    // inside the 250 ms rewind clamp).
    for (int i = 0; i < 10; ++i) step(idle, sprintZ, 0);

    // Aim exactly where the acked snapshot showed the victim.
    const glm::vec3 d = glm::normalize(aimTarget - eye);
    meat::PlayerCommand shot{};
    shot.fire = true;
    shot.yaw = std::atan2(-d.x, -d.z);
    shot.pitch = std::asin(d.y);

    step(shot, sprintZ, viewTick); // stale view: the server must rewind and hit
    for (int i = 0; i < 6; ++i) step(idle, sprintZ, 0); // let a snapshot reach the victim
    const meat::PlayerState* victimAfterHit = stateOf(2, 2);
    if (!victimAfterHit) { check(false, "victim still snapshotted"); return; }
    check(victimAfterHit->health < 100.0f,
          "a shot at the old position HITS when the old snapshot is acked");
    const float healthAfterHit = victimAfterHit->health;

    // Control: same aim with a live view — the victim has long since left that
    // spot, so without the rewind this shot must miss.
    for (int i = 0; i < 12; ++i) step(idle, sprintZ, 0); // cooldown + more distance
    step(shot, sprintZ, 0); // ack 0 = live poses
    for (int i = 0; i < 6; ++i) step(idle, sprintZ, 0);
    const meat::PlayerState* victimAfterMiss = stateOf(2, 2);
    if (!victimAfterMiss) { check(false, "victim still snapshotted"); return; }
    check(victimAfterMiss->health == healthAfterHit,
          "the same aim with a live view MISSES the moved victim");
}

// Effect execution end-to-end: a damaged player using a medkit heals. This
// exercises the use→consumable→runEffects→Heal path (currently the only test of
// the effect system beyond hitscan Damage), observed the way a client sees it —
// through snapshots.
void testMedkitHealsThroughEffectSystem() {
    std::printf("a damaged player heals by using a medkit (effect system)\n");
    ScriptedTransport wire;
    meat::GameRules rules;
    rules.terrain = meat::GameRules::Terrain::Superflat; // clean firing lane
    meat::ServerSim server(rules);
    if (!server.init(778u)) { check(false, "server booted"); return; }

    wire.connect(1);
    wire.packet(1, meat::HelloMsg{meat::kProtocolVersion, "shooter", ""});
    wire.connect(2);
    wire.packet(2, meat::HelloMsg{meat::kProtocolVersion, "victim", ""});
    server.pump(wire);

    std::uint64_t t1 = 0, t2 = 0;
    auto step = [&](const meat::PlayerCommand& a, const meat::PlayerCommand& b) {
        meat::PlayerCommand c1 = a, c2 = b;
        c1.tick = ++t1;
        c2.tick = ++t2;
        wire.packet(1, meat::CommandMsg{c1, 0});
        wire.packet(2, meat::CommandMsg{c2, 0});
        server.pump(wire);
        server.tick(wire);
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // let mesh workers ground players
    };
    auto stateOf = [&](meat::PeerId who) -> const meat::PlayerState* {
        for (const meat::PlayerState& p : wire.lastSnapshot[1].players)
            if (p.playerId == who) return &p;
        return nullptr;
    };
    const meat::PlayerCommand idle{};
    meat::PlayerCommand away{}; // walk the victim clear of the shooter
    away.move = {0.0f, 1.0f};
    away.sprint = true;
    away.yaw = 1.5707963f;

    // Ground both players before doing anything positional — chunk colliders
    // stream in on worker threads (slow under ASan), and a falling victim would
    // make the aimed shot miss. Generous budget; returns the moment grounded.
    auto waitGrounded = [&](meat::PeerId who) {
        for (int i = 0; i < 30; ++i) step(idle, idle); // prime spawn-area meshing
        for (int i = 0; i < 8000 && !server.meshingIdle(); ++i) {
            step(idle, idle);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (int i = 0; i < 300; ++i) {
            const meat::PlayerState* p = stateOf(who);
            if (p && p->onGround) return true;
            step(idle, idle);
        }
        return false;
    };
    if (!waitGrounded(1) || !waitGrounded(2)) { check(false, "both players grounded"); return; }
    for (int i = 0; i < 120; ++i) step(idle, away);  // ~8 m of separation
    if (!waitGrounded(2)) { check(false, "victim grounded after moving"); return; }
    for (int i = 0; i < 30; ++i) step(idle, idle);   // stand still, land a fresh snapshot

    const meat::PlayerState* s0 = stateOf(1);
    const meat::PlayerState* v0 = stateOf(2);
    if (!s0 || !v0) { check(false, "both players snapshotted"); return; }

    // Aim at the victim's torso. Pistol is semi-auto (one shot per press edge),
    // so two spaced trigger pulls deal ~50 damage — hurt but not killed (a kill
    // would respawn them at full health and hide the heal).
    const glm::vec3 eye = s0->pos + glm::vec3(0, 1.62f, 0);
    const glm::vec3 target = v0->pos + glm::vec3(0, 0.9f, 0);
    const glm::vec3 dir = glm::normalize(target - eye);
    meat::PlayerCommand shoot{};
    shoot.fire = true;
    shoot.yaw = std::atan2(-dir.x, -dir.z);
    shoot.pitch = std::asin(dir.y);
    step(shoot, idle);
    for (int i = 0; i < 14; ++i) step(idle, idle); // release + fire-cooldown
    step(shoot, idle);
    for (int i = 0; i < 8; ++i) step(idle, idle);

    const meat::PlayerState* vHurt = stateOf(2);
    if (!vHurt) { check(false, "victim snapshotted"); return; }
    check(vHurt->health < 100.0f && vHurt->health > 0.0f, "the victim is hurt but alive");
    const float hurt = vHurt->health;

    // Victim uses a medkit — slot 16 in the default (non-space) starting loadout
    // (pistol,ap,hp,smg,shotgun,sniper,claymore,turret,companion,shells,rifle,
    //  stone,rpg,grenade,9mm,rockets,MEDKIT). One press edge = one use.
    meat::PlayerCommand heal{};
    heal.use = true;
    heal.selectedSlot = 16;
    step(idle, heal);
    for (int i = 0; i < 10; ++i) step(idle, idle); // let the heal + a snapshot land

    const meat::PlayerState* vHealed = stateOf(2);
    if (!vHealed) { check(false, "victim snapshotted"); return; }
    check(vHealed->health > hurt, "using the medkit restored health via the effect system");
}

// F1 interest management: with a positive interestRadius a client should only
// receive entities near its own player. Same seed, wide vs tiny radius — the
// tiny-radius client must see strictly fewer entities (dungeon NPCs/loot sit
// underground, far from the surface spawn). Players are never scoped out.
void testInterestManagementScopesEntities() {
    std::printf("interest management culls entities outside a client's radius\n");
    auto entityCountFor = [](float radius) -> std::size_t {
        ScriptedTransport wire;
        meat::GameRules rules;
        rules.terrain = meat::GameRules::Terrain::Normal; // spawns dungeon NPCs + loot
        rules.interestRadius = radius;
        meat::ServerSim server(rules);
        if (!server.init(4242u)) return static_cast<std::size_t>(-1);
        wire.connect(1);
        wire.packet(1, meat::HelloMsg{meat::kProtocolVersion, "p", ""});
        server.pump(wire);
        std::uint64_t t = 0;
        for (int i = 0; i < 40; ++i) {
            meat::PlayerCommand c{};
            c.tick = ++t;
            wire.packet(1, meat::CommandMsg{c, 0});
            server.pump(wire);
            server.tick(wire);
        }
        return wire.lastSnapshot[1].entities.size();
    };
    const std::size_t wide = entityCountFor(100000.0f); // effectively unbounded
    const std::size_t tiny = entityCountFor(3.0f);      // just around the player
    check(wide > 0, "there are entities to scope (dungeon NPCs/loot)");
    check(tiny < wide, "a tiny interest radius replicates fewer entities");
}

} // namespace

namespace meattest {

void runNetPermissions() {
    testPlayerCannotEditVoxels();
    testEditorCanEditVoxels();
    testWrongTokenIsStillAPlayer();
    testPolicyOffRefusesTheRightToken();
    testPropEditsNeedPermission();
    testEditorCanPlaceProp();
    testMalformedTransformsAreRefused();
    testAssetPathTraversalIsRefused();
    testVoxelEditsAreRateLimited();
    testProtocolMismatchIsRefused();
    testMalformedPacketsDoNotCrash();
    testExtremeVoxelCoordinatesAreRefused();
    testMoveAndRemovePropNeedPermission();
    testHitscanIsLagCompensated();
    testMedkitHealsThroughEffectSystem();
    testInterestManagementScopesEntities();
}

} // namespace meattest
