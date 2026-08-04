// ServerSim — combat resolution: NPC damage, kill scoring / teams, and the single
// player death+respawn path.
//
// Split out of ServerSim.cpp (a god file) to keep concerns separable. These are
// ServerSim member functions in their own translation unit — behaviour is
// identical. Contents: damageNpc, team assignment + friendly-fire gate + frag
// scoring (registerFrag), the drop-on-death loot scatter, and killPlayer (the one
// death path every damage source routes through). Declarations live in ServerSim.h.

#include "game/ServerSim.h"
#include "game/ServerSimInternal.h" // defaultSpawnPos

#include "engine/core/Log.h"
#include "engine/net/Messages.h" // ScriptFxMsg, pack

#include <cmath>
#include <cstdint>

namespace meat {

void ServerSim::damageNpc(Transport& transport, Npc& npc, float damage) {
    if (npc.health <= 0.0f) return; // already dead: no double loot from multi-pellet kills
    npc.health -= damage;
    if (npc.health > 0.0f) return;
    // Death: drop a small ammo cache where it fell (survivors loot the room).
    spawnPickup(m_defaultItems.ammo9mm, 12, npc.pos + glm::vec3(0, 0.3f, 0));
    (void)transport; // death effects (sound/particles) ride future events
}

// Drop-on-death: scatter part of the victim's bag as world pickups at the spot
// Credit a player-vs-player kill for Deathmatch and latch the match result once
// someone reaches fragLimit. Suicides / environment kills (killer 0 or self) do
// not score. Sandbox mode ignores this entirely.
void ServerSim::assignTeam(Player& player) {
    if (m_rules.gameMode != GameRules::GameMode::TeamDeathmatch) return;
    if (player.team != 0) return; // already assigned
    // Balance across two teams: join whichever currently has fewer members (ties
    // go to team 1). Count spawned players' current teams.
    int counts[3] = {0, 0, 0}; // index 1,2 used
    for (const auto& [peer, pl] : m_players)
        if (pl && pl->team >= 1 && pl->team <= 2) ++counts[pl->team];
    player.team = (counts[1] <= counts[2]) ? 1 : 2;
}

bool ServerSim::friendlyBlocked(PeerId src, PeerId dst) const {
    if (m_rules.friendlyFire || src == 0 || dst == 0 || src == dst) return false;
    const int st = teamOf(src);
    return st != 0 && st == teamOf(dst); // same team, friendly fire off
}

PeerId ServerSim::peerOfPlayer(const Player* p) const {
    if (!p) return 0;
    for (const auto& [peer, pl] : m_players)
        if (pl.get() == p) return peer;
    return 0;
}

void ServerSim::registerFrag(PeerId killer, PeerId victim) {
    const bool team = m_rules.gameMode == GameRules::GameMode::TeamDeathmatch;
    if (m_rules.gameMode != GameRules::GameMode::Deathmatch && !team) return;
    if (killer == 0 || killer == victim) return;
    const int killerTeam = teamOf(killer);
    // A teammate kill (only possible with friendly fire on) scores for no one.
    if (team && killerTeam != 0 && killerTeam == teamOf(victim)) return;
    ++m_frags[killer]; // personal tally is always kept (leaderboard)
    const int score = team ? (m_teamFrags[killerTeam] += 1) : m_frags[killer];
    if (!m_matchOver && score >= m_rules.fragLimit) {
        m_matchOver = true;
        if (team) {
            m_winningTeam = killerTeam;
            log::info("server: TEAM DEATHMATCH over — team {} wins ({} frags)", killerTeam,
                      score);
        } else {
            m_matchWinner = killer;
            log::info("server: DEATHMATCH over — player {} wins ({} frags)", killer, score);
        }
        // HUD banner to everyone (reuses the ScriptFx announce path). m_activeTransport
        // is set during a tick's combat step, and null in a direct unit-test call.
        if (m_activeTransport) {
            ScriptFxMsg fx;
            fx.kind = 1; // HUD announce
            fx.duration = 8.0f;
            fx.r = 1.0f;
            fx.g = 0.85f;
            fx.b = 0.2f;
            fx.text = team ? ("Team Deathmatch — team " + std::to_string(killerTeam) + " wins!")
                           : ("Deathmatch — player " + std::to_string(killer) + " wins!");
            for (const auto& [peer, pl] : m_players)
                if (pl) m_activeTransport->send(peer, pack(fx), true);
        }
    }
}

// they fell, so a killer (or the room) can loot the kill. Deterministic — the
// same bag + position produce the same scatter on every peer/replay, keeping the
// server-authoritative snapshots in sync. GameRules-gated (no-op when disabled).
void ServerSim::dropPlayerLoot(Player& player, glm::vec3 pos) {
    if (!m_rules.dropOnDeath) return;
    constexpr int kMaxDeathDrops = 6;            // a subset — a corpse to loot, not a landfill
    constexpr float kGoldenAngle = 2.39996323f;  // rad: fans the stacks into an even ring
    constexpr float kScatterRadius = 0.6f;       // metres from the death spot
    int dropped = 0;
    for (int i = 0; i < Inventory::kSlots && dropped < kMaxDeathDrops; ++i) {
        ItemStack& s = player.inventory.slot(i);
        if (s.id == 0 || s.count == 0) continue;
        const float a = static_cast<float>(dropped) * kGoldenAngle;
        spawnPickup(s.id, s.count,
                    pos + glm::vec3(std::cos(a) * kScatterRadius, 0.3f,
                                    std::sin(a) * kScatterRadius));
        s = {}; // the stack left the bag for the ground
        ++dropped;
    }
    if (dropped > 0) player.inventoryDirty = true; // flushed on the victim's next combat tick
}

void ServerSim::killPlayer(Player& victim, PeerId victimPeer, PeerId killer) {
    dropPlayerLoot(victim, victim.controller.position()); // scatter before respawn
    victim.controller.setState(defaultSpawnPos(), glm::vec3(0));
    victim.health = 100.0f;
    victim.burns.clear();     // respawn is a clean slate: no lingering DoT...
    victim.modifiers.clear(); // ...or buffs/debuffs (else a burn re-kills post-respawn)
    // Eject from any ship seat so the hull frees up and the capsule respawns clean.
    if (victim.pilotingShip != 0) {
        if (Ship* ps = findShip(victim.pilotingShip)) {
            if (victim.shipRole == 1) ps->pilot = 0;
            if (victim.shipRole == 2) ps->passenger = 0;
            if (ps->pilot == 0 && ps->passenger == 0) ensureShipBody(*ps);
        }
        victim.pilotingShip = 0;
        victim.shipRole = 0;
    }
    registerFrag(killer, victimPeer); // no-op for Sandbox / environment / suicide
    if (victimPeer != 0) m_scripts.onPlayerDeath(static_cast<std::uint32_t>(victimPeer));
}

} // namespace meat
