// GameMode framework: the rules of victory layered over a template. Sandbox is
// free-play (no win condition); Deathmatch ends the match when a player reaches
// the frag limit. Scoring is server-authoritative and only counts real
// player-vs-player kills (not suicides or environment deaths).

#include "Harness.h"

#include "engine/net/LoopbackTransport.h"
#include "engine/net/Messages.h"
#include "game/ServerSim.h"

#include <cstdio>

namespace {

using meattest::check;

void testDeathmatchWinCondition() {
    std::printf("deathmatch ends when a player reaches the frag limit\n");
    meat::GameRules rules;
    rules.terrain = meat::GameRules::Terrain::Void; // fast, empty world
    rules.gameMode = meat::GameRules::GameMode::Deathmatch;
    rules.fragLimit = 3;
    meat::ServerSim server(rules);
    if (!server.init(1u)) { check(false, "server booted"); return; }

    check(!server.matchOver(), "the match starts open");
    server.registerFrag(1, 2);
    server.registerFrag(1, 2);
    check(server.fragsOf(1) == 2 && !server.matchOver(),
          "frags accumulate but the match stays open below the limit");
    server.registerFrag(1, 2);
    check(server.fragsOf(1) == 3, "the frag is counted");
    check(server.matchOver() && server.matchWinner() == 1,
          "reaching the frag limit ends the match with the right winner");

    // Further frags don't change the latched result.
    server.registerFrag(2, 1);
    check(server.matchWinner() == 1, "the winner is latched, not overwritten");
}

void testSuicideAndEnvironmentDoNotScore() {
    std::printf("suicides and environment kills do not score\n");
    meat::GameRules rules;
    rules.terrain = meat::GameRules::Terrain::Void;
    rules.gameMode = meat::GameRules::GameMode::Deathmatch;
    rules.fragLimit = 1;
    meat::ServerSim server(rules);
    if (!server.init(1u)) { check(false, "server booted"); return; }

    server.registerFrag(1, 1); // self-frag
    server.registerFrag(0, 2); // environment kill (no killer)
    check(server.fragsOf(1) == 0 && !server.matchOver(),
          "a suicide or environment death credits no one");
}

void testSandboxIgnoresScoring() {
    std::printf("sandbox mode never ends and never scores\n");
    meat::GameRules rules; // default gameMode = Sandbox
    rules.terrain = meat::GameRules::Terrain::Void;
    meat::ServerSim server(rules);
    if (!server.init(1u)) { check(false, "server booted"); return; }

    for (int i = 0; i < 50; ++i) server.registerFrag(1, 2);
    check(server.fragsOf(1) == 0 && !server.matchOver(),
          "sandbox ignores frags and has no win condition");
}

// Ignite is a damage-over-time effect: it burns a player for dps × duration and
// then stops. Driven here through the public applyDamageOverTime entry (the same
// state the Ignite effect kind pushes to) on a Void world, so no ambient AI
// muddies the health signal. server.tick() advances one 1/60 s fixed step, so 60
// ticks is exactly one second.
void testIgniteDamageOverTime() {
    std::printf("ignite burns a player over time, then stops when it expires\n");
    meat::GameRules rules;
    rules.terrain = meat::GameRules::Terrain::Void; // empty world: no ambient damage
    meat::ServerSim server(rules);
    meat::LoopbackPair wire;
    if (!server.init(7u)) { check(false, "server booted"); return; }
    const meat::PeerId peer = 1;

    server.pump(wire.serverEnd()); // register the Connected peer
    wire.clientEnd().send(
        peer, meat::pack(meat::HelloMsg{meat::kProtocolVersion, "burnee", ""}), true);
    server.pump(wire.serverEnd());
    for (int i = 0; i < 10; ++i) server.tick(wire.serverEnd()); // spawn + settle

    const float h0 = server.playerHealth(peer);
    check(h0 > 99.0f, "the player starts at full health");

    server.applyDamageOverTime(peer, 20.0f, 1.0f, 0); // 20 dmg over 1 s (environment)
    for (int i = 0; i < 60; ++i) server.tick(wire.serverEnd()); // one second of burning
    const float burned = h0 - server.playerHealth(peer);
    check(burned > 15.0f && burned < 25.0f,
          "ignite removed about dps*duration (~20) health");

    const float h1 = server.playerHealth(peer);
    for (int i = 0; i < 60; ++i) server.tick(wire.serverEnd()); // burn has now expired
    check(h1 - server.playerHealth(peer) < 1.0f, "damage stops once the burn expires");
}

} // namespace

namespace meattest {

void runGameMode() {
    testDeathmatchWinCondition();
    testSuicideAndEnvironmentDoNotScore();
    testSandboxIgnoresScoring();
    testIgniteDamageOverTime();
}

} // namespace meattest
