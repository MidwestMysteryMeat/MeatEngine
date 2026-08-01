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

#include "engine/net/LoopbackTransport.h"
#include "engine/net/Messages.h"
#include "game/ServerSim.h"

#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (condition) {
        std::printf("  [pass] %s\n", what);
    } else {
        std::printf("  [FAIL] %s\n", what);
        ++g_failures;
    }
}

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

} // namespace

int main() {
    std::printf("MeatEngine headless tests\n\n");

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

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
