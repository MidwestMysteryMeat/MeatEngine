// GameMode framework: the rules of victory layered over a template. Sandbox is
// free-play (no win condition); Deathmatch ends the match when a player reaches
// the frag limit. Scoring is server-authoritative and only counts real
// player-vs-player kills (not suicides or environment deaths).

#include "Harness.h"

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

} // namespace

namespace meattest {

void runGameMode() {
    testDeathmatchWinCondition();
    testSuicideAndEnvironmentDoNotScore();
    testSandboxIgnoresScoring();
}

} // namespace meattest
