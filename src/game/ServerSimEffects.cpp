// ServerSim — effect system, combat modifiers, and entity/crowd spawn helpers.
//
// Split out of ServerSim.cpp (which had grown into a god file) to keep concerns
// separable. These are ServerSim member functions compiled in their own
// translation unit — behaviour is identical, they just no longer bloat the core
// tick/netcode file. Contents: damage/speed modifier folding, the GAS-lite effect
// executors (Damage/AreaDamage/Heal/ApplyModifier/Knockback/Ignite/Chain/
// SpawnEntity) and their public ability entries (applyDamageOverTime/
// applyChainDamage), the DoT burn tick, and the turret/companion/crowd spawners.
// Declarations live in ServerSim.h. Player death routes through killPlayer, which
// stays in ServerSim.cpp.

#include "game/ServerSim.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace meat {

float ServerSim::damageMultOf(const Player& player) {
    float m = 1.0f;
    for (const Player::ActiveModifier& mod : player.modifiers) m *= mod.damageMult;
    return m;
}

float ServerSim::speedMultOf(const Player& player) {
    float m = 1.0f;
    for (const Player::ActiveModifier& mod : player.modifiers) m *= mod.speedMult;
    return m;
}

void ServerSim::tickModifiers(Player& player, float dt) {
    if (player.modifiers.empty()) return; // hot path: most players carry none
    for (Player::ActiveModifier& mod : player.modifiers) mod.remaining -= dt;
    std::erase_if(player.modifiers,
                  [](const Player::ActiveModifier& m) { return m.remaining <= 0.0f; });
}

void ServerSim::applyDamageOverTime(PeerId target, float dps, float seconds, PeerId source) {
    if (dps <= 0.0f || seconds <= 0.0f) return;
    const auto it = m_players.find(target);
    if (it == m_players.end() || !it->second) return;
    it->second->burns.push_back({dps, seconds, source});
}

std::uint32_t ServerSim::spawnTurret(PeerId owner, glm::vec3 pos) {
    Turret t;
    t.id = m_nextEntityId++;
    t.owner = owner;
    t.pos = pos;
    m_turrets.push_back(t);
    return t.id;
}

std::uint32_t ServerSim::spawnCompanion(PeerId owner, glm::vec3 pos) {
    Companion c;
    c.id = m_nextEntityId++;
    c.owner = owner;
    c.pos = pos;
    m_companions.push_back(c);
    return c.id;
}

void ServerSim::spawnCrowd(std::uint32_t seed, int count, glm::vec3 center, float radius) {
    m_crowd.spawn(seed, count, center, radius);
    // Reserve a contiguous id block so each agent keeps a stable entity id across
    // snapshots (the delta codec keys on id). agent i => m_crowdBaseId + i.
    m_crowdBaseId = m_nextEntityId;
    m_nextEntityId += static_cast<std::uint32_t>(m_crowd.size());
}

void ServerSim::applyChainDamage(PeerId source, glm::vec3 origin, float damage,
                                 int maxTargets, float range) {
    if (damage <= 0.0f || maxTargets <= 0) return;
    const float range2 = range * range;
    std::vector<PeerId> hit;
    hit.reserve(static_cast<std::size_t>(maxTargets));
    glm::vec3 lastPos = origin;
    for (int n = 0; n < maxTargets; ++n) {
        // Nearest live, spawned, not-yet-hit player within range of the last arc.
        Player* best = nullptr;
        PeerId bestPeer = 0;
        float bestD2 = range2;
        for (auto& [peer, pl] : m_players) {
            if (!pl || !pl->spawned || pl->health <= 0.0f) continue;
            if (peer == source) continue; // a chain doesn't arc back to its caster
            if (std::find(hit.begin(), hit.end(), peer) != hit.end()) continue;
            if (friendlyBlocked(source, peer)) continue; // arc skips teammates
            const glm::vec3 d = pl->controller.position() - lastPos;
            const float d2 = glm::dot(d, d);
            if (d2 <= bestD2) { bestD2 = d2; best = pl.get(); bestPeer = peer; }
        }
        if (!best) break; // arc dies when no target is in range
        best->health -= damage;
        hit.push_back(bestPeer);
        lastPos = best->controller.position();
        if (best->health <= 0.0f) killPlayer(*best, bestPeer, source);
    }
}

void ServerSim::tickBurns(Transport& transport, PeerId peer, Player& player, float dt) {
    if (player.burns.empty()) return; // hot path: most players carry none
    // Clamp each tick's contribution to the burn's own remaining lifetime so a
    // near-expired burn doesn't over-deal on its final tick, then age all burns.
    float total = 0.0f;
    PeerId killer = 0;
    for (Player::Burn& b : player.burns) {
        const float step = std::min(b.remaining, dt);
        if (step > 0.0f) {
            total += b.dps * step;
            killer = b.source; // last source to still deal this tick takes the kill
        }
        b.remaining -= dt;
    }
    std::erase_if(player.burns, [](const Player::Burn& b) { return b.remaining <= 0.0f; });
    if (total <= 0.0f || player.health <= 0.0f) return;
    player.health -= total;
    if (player.health <= 0.0f) killPlayer(player, peer, killer); // DoT kill credit
    (void)transport;
}

void ServerSim::applyEffect(Transport& transport, const Effect& effect, PeerId source,
                            glm::vec3 targetPos, Player* targetPlayer, Npc* targetNpc) {
    // Outgoing damage scales by the acting player's active buffs (stim etc.).
    float srcMult = 1.0f;
    if (const auto it = m_players.find(source); it != m_players.end())
        srcMult = damageMultOf(*it->second);

    switch (effect.kind) {
    case EffectKind::Damage: {
        const float dmg = effect.params[0] * srcMult;
        if (targetNpc) {
            damageNpc(transport, *targetNpc, dmg);
        } else if (targetPlayer) {
            const PeerId victimPeer = peerOfPlayer(targetPlayer);
            if (friendlyBlocked(source, victimPeer)) break; // no teammate damage
            targetPlayer->health -= dmg;
            if (targetPlayer->health <= 0.0f)
                killPlayer(*targetPlayer, victimPeer, source); // effect Damage now credits
        }
        break;
    }
    case EffectKind::AreaDamage:
        // Reuse the existing blast falloff + voxel-crater carving verbatim.
        applyBlast(transport, source, targetPos, effect.radius, effect.params[0] * srcMult);
        break;
    case EffectKind::Heal:
        if (targetPlayer)
            targetPlayer->health = glm::min(100.0f, targetPlayer->health + effect.params[0]);
        break;
    case EffectKind::ApplyModifier:
        if (targetPlayer)
            targetPlayer->modifiers.push_back({effect.params[0], effect.params[1], effect.duration});
        break;
    case EffectKind::Ignite:
        // Damage-over-time: params[0] = damage/sec, duration = seconds. Stacks;
        // ticked in tickBurns each fixed tick, credited to the igniter on kill.
        if (targetPlayer && !friendlyBlocked(source, peerOfPlayer(targetPlayer)))
            targetPlayer->burns.push_back({effect.params[0], effect.duration, source});
        break;
    case EffectKind::Chain:
        // Arc damage: params[0] = damage per target, params[1] = max targets,
        // `radius` = jump range. Starts nearest the primary target's position (or
        // the effect's targetPos for a positional trigger).
        applyChainDamage(source,
                         targetPlayer ? targetPlayer->controller.position() : targetPos,
                         effect.params[0], std::max(1, static_cast<int>(effect.params[1])),
                         effect.radius);
        break;
    case EffectKind::SpawnEntity:
        // Summon an owned AI helper at the effect's target point (params[0]:
        // 0 = turret, >=1 = companion). Owner is the acting player; it replicates
        // through the entity-snapshot path like any deployed helper.
        if (effect.params[0] >= 0.5f) spawnCompanion(source, targetPos);
        else spawnTurret(source, targetPos);
        break;
    case EffectKind::Knockback:
        if (targetPlayer) {
            // Push away from the effect origin — the source player's position if
            // we have it, else the effect's targetPos (a blast center).
            glm::vec3 from = targetPos;
            if (const auto it = m_players.find(source); it != m_players.end() && it->second)
                from = it->second->controller.position();
            glm::vec3 dir = targetPlayer->controller.position() - from;
            dir.y = 0.0f; // shove is horizontal; the lift is added separately
            const float len = glm::length(dir);
            dir = len > 1e-3f ? dir / len : glm::vec3(0.0f, 0.0f, 1.0f);
            const float mag = effect.params[0];
            targetPlayer->controller.addImpulse(dir * mag + glm::vec3(0.0f, mag * 0.4f, 0.0f));
        }
        break;
    }
}

void ServerSim::runEffects(Transport& transport, const EffectList& effects, PeerId source,
                           glm::vec3 targetPos, Player* targetPlayer, Npc* targetNpc) {
    for (const Effect& e : effects)
        applyEffect(transport, e, source, targetPos, targetPlayer, targetNpc);
}

} // namespace meat
