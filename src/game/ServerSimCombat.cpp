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
#include "engine/core/ViewMath.h"     // viewForward (projectile/hitscan aim)
#include "engine/net/DeltaSnapshot.h" // hit/blast broadcast
#include "engine/net/Messages.h"      // ScriptFxMsg, pack, ByteWriter
#include "game/WeaponFire.h"          // weapon/penetration data for hitscan

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace meat {

namespace {
// Deterministic per-shot spread: a hash of (peer, tick, pellet) rotates the aim
// direction inside the cone. Same inputs on any peer → same pattern, so a future
// client-side tracer prediction stays in sync without a shared RNG object.
glm::vec3 spreadDir(glm::vec3 dir, float coneDeg, PeerId peer, std::uint64_t tick, int idx) {
    if (coneDeg <= 0.0f) return dir;
    std::uint64_t h = peer * 0x9E3779B97F4A7C15ull + tick * 0xBF58476D1CE4E5B9ull +
                      static_cast<std::uint64_t>(idx) * 0x94D049BB133111EBull;
    h = (h ^ (h >> 31)) * 0xD6E8FEB86659FD93ull;
    const float u = static_cast<float>((h >> 11) & 0xFFFFF) / static_cast<float>(0xFFFFF);
    const float v = static_cast<float>((h >> 33) & 0xFFFFF) / static_cast<float>(0xFFFFF);
    const float cone = glm::radians(coneDeg);
    const float theta = u * 6.2831853f;
    const float r = std::sqrt(v) * cone; // uniform over the cone disc
    // Build a basis around dir and tilt by (r, theta).
    const glm::vec3 up = std::abs(dir.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 right = glm::normalize(glm::cross(dir, up));
    const glm::vec3 realUp = glm::cross(right, dir);
    const glm::vec3 offset = (right * std::cos(theta) + realUp * std::sin(theta)) * std::sin(r);
    return glm::normalize(dir * std::cos(r) + offset);
}
} // namespace

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


// --- hitscan, bullet march, projectiles, and blast (moved from ServerSim.cpp) ---

void ServerSim::fireHitscan(Transport& transport, PeerId peer, Player& player,
                            const ItemDef& weapon) {
    const glm::vec3 aim = viewForward(player.lastCmd.yaw, player.lastCmd.pitch);
    const int pellets = std::max<int>(1, weapon.pellets);
    for (int i = 0; i < pellets; ++i) {
        // Seed spread on the server tick, not lastCmdTick: an auto weapon fires
        // several times per received command, so lastCmdTick would freeze the
        // pattern into a fixed offset. m_tick advances every shot.
        const glm::vec3 dir = spreadDir(aim, weapon.spreadDeg, peer, m_tick, i);
        marchBullet(transport, peer, player, weapon, dir);
    }
}

void ServerSim::marchBullet(Transport& transport, PeerId peer, Player& player,
                            const ItemDef& weapon, glm::vec3 dir) {
    // Shared with processCombat so hardpoints and eye aim stay consistent.
    glm::vec3 origin = combatMuzzle(player);
    float remaining = kHitscanRange;
    // AP/HP ammo: bake the round's multipliers into the shot up front so every
    // flesh/block hit below inherits its character. HP's 0 penetrationMult zeroes
    // the budget -> it deals its (boosted) damage to the first material and stops;
    // AP's >1 mult buys more crossings at a reduced per-hit damage. Deterministic.
    float budget = weapon.penBudget * weapon.penetrationMult;
    // Fold the shooter's active damage modifiers (stim etc.) into every hit.
    const float shotDamage = weapon.damage * weapon.damageMult * damageMultOf(player);
    float damageScale = 1.0f;

    // F2 lag compensation: other players' capsules are judged where they stood
    // in the snapshot the SHOOTER last acked, so a hit lands where the shooter
    // aimed on their own screen. Clamped so a high-ping (or lying) peer cannot
    // pull targets more than 250 ms into the past. NPCs and ships stay live —
    // they are server-driven, so there is no client view of them to honor.
    std::uint64_t rewindTick = player.ackedSnapshotTick;
    if (rewindTick == 0 || rewindTick >= m_tick)
        rewindTick = m_tick; // no snapshot seen yet (loopback boot) => live poses
    else if (m_tick - rewindTick > kMaxRewindTicks)
        rewindTick = m_tick - kMaxRewindTicks;

    for (int hop = 0; hop < 8 && remaining > 0.1f; ++hop) {
        const auto voxelHit = m_voxels.raycast(origin, dir, remaining);
        const float segmentEnd = voxelHit ? voxelHit->t : remaining;

        // Closest player capsule within this air segment beats the wall.
        Player* victim = nullptr;
        PeerId victimPeer = 0;
        float bestT = segmentEnd;
        for (auto& [otherPeer, other] : m_players) {
            if (otherPeer == peer || !other->spawned) continue;
            if (friendlyBlocked(peer, otherPeer)) continue; // teammate: bullet passes through
            glm::vec3 feet = other->controller.position();
            bool crouched = other->controller.crouched();
            if (rewindTick != m_tick) rewoundPlayerPose(*other, rewindTick, feet, crouched);
            const float height = crouched ? 0.95f : 1.8f;
            const glm::vec3 a = feet + glm::vec3(0, kCapsuleRadius, 0);
            const glm::vec3 b = feet + glm::vec3(0, height - kCapsuleRadius, 0);
            float tRay = 0;
            if (raySegmentDistance(origin, dir, segmentEnd, a, b, tRay) <= kCapsuleRadius &&
                tRay < bestT) {
                bestT = tRay;
                victim = other.get();
                victimPeer = otherPeer;
            }
        }
        // NPC capsules compete with player capsules for the closest hit.
        Npc* npcVictim = nullptr;
        for (Npc& npc : m_npcs) {
            if (npc.health <= 0.0f) continue; // corpses don't absorb pellets
            const glm::vec3 a = npc.pos + glm::vec3(0, kCapsuleRadius, 0);
            const glm::vec3 b = npc.pos + glm::vec3(0, 1.7f - kCapsuleRadius, 0);
            float tRay = 0;
            if (raySegmentDistance(origin, dir, segmentEnd, a, b, tRay) <= kCapsuleRadius &&
                tRay < bestT) {
                bestT = tRay;
                npcVictim = &npc;
                victim = nullptr; // the NPC is now the closest flesh
            }
        }

        // H4: ship hulls compete for the closest hit (AABB-ish sphere radius).
        Ship* shipVictim = nullptr;
        for (Ship& sh : m_ships) {
            if (sh.health <= 0.0f) continue;
            // Don't shoot your own seat out from under yourself (pilot or passenger).
            if (sh.pilot == peer || sh.passenger == peer) continue;
            const float rad = glm::length(sh.halfExtents) * 0.65f;
            const glm::vec3 a = sh.pos - glm::vec3(0, sh.halfExtents.y * 0.3f, 0);
            const glm::vec3 b = sh.pos + glm::vec3(0, sh.halfExtents.y * 0.3f, 0);
            float tRay = 0;
            if (raySegmentDistance(origin, dir, segmentEnd, a, b, tRay) <= rad && tRay < bestT) {
                bestT = tRay;
                shipVictim = &sh;
                npcVictim = nullptr;
                victim = nullptr;
            }
        }
        if (shipVictim) {
            damageShip(transport, *shipVictim, shotDamage * damageScale, peer);
            return;
        }
        if (npcVictim) {
            damageNpc(transport, *npcVictim, shotDamage * damageScale);
            return;
        }
        if (victim) {
            victim->health -= shotDamage * damageScale;
            if (victim->health <= 0.0f) {
                log::info("server: player {} fragged player {}", peer, victimPeer);
                killPlayer(*victim, victimPeer, peer);
            }
            return; // flesh stops bullets (AP ammo types may change this later)
        }
        if (!voxelHit || voxelHit->block == 0) return;

        // Chip the block.
        const BlockDef& material = m_voxels.blockRegistry().get(voxelHit->block);
        bool broke = !m_rules.blockDamage; // instant-break rules skip the hp model
        if (m_rules.blockDamage) {
            auto [entry, inserted] = m_voxelDamage.try_emplace(voxelHit->voxel, material.hp);
            entry->second -= shotDamage * damageScale;
            if (entry->second <= 0.0f) {
                m_voxelDamage.erase(entry);
                broke = true;
            }
        }
        if (broke) {
            applyVoxelOp(transport, {voxelHit->voxel, 0});
            if (m_rules.minedBlockDrops) {
                player.inventory.add(m_defaultItems.stoneBlock, 1, m_items);
                player.inventoryDirty = true;
            }
        }

        // Penetrate or stop.
        if (!m_rules.penetration || budget < material.penCost) return;
        budget -= material.penCost;
        damageScale *= 0.65f;

        // Advance the ray past the exit face of this voxel (slab test).
        const glm::vec3 lo = glm::vec3(voxelHit->voxel) * kVoxelSize;
        const glm::vec3 hi = lo + kVoxelSize;
        float exitT = remaining;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(dir[axis]) < 1e-8f) continue;
            const float t = (dir[axis] > 0 ? hi[axis] - origin[axis]
                                           : lo[axis] - origin[axis]) / dir[axis];
            exitT = std::min(exitT, t);
        }
        const float advance = exitT + 0.001f;
        origin += dir * advance;
        remaining -= advance;
    }
}

void ServerSim::spawnProjectile(PeerId owner, glm::vec3 pos, glm::vec3 vel,
                                const ItemDef& weapon) {
    Projectile p{m_nextEntityId++, owner, pos, vel, weapon.projectileGravity,
                 weapon.blastRadius, weapon.blastDamage, 6.0f};
    // Carry the weapon's composed on-impact effects; fall back to a derived
    // AreaDamage so a legacy weapon (no authored list) still detonates identically.
    p.onImpact = weapon.effects.empty() ? EffectList{areaDamageEffect(p.damage, p.radius)}
                                        : weapon.effects;
    m_projectiles.push_back(std::move(p));
}

// Radial damage: players by distance falloff, and every solid voxel in range
// takes damage scaled by proximity (explosives carve craters). Server-only.
void ServerSim::applyBlast(Transport& transport, PeerId source, glm::vec3 center,
                           float radius, float damage) {
    for (auto& [peer, player] : m_players) {
        if (!player->spawned) continue;
        if (friendlyBlocked(source, peer)) continue; // teammates shrug off the blast (self still hurts)
        const glm::vec3 body = player->controller.position() + glm::vec3(0, 0.9f, 0);
        const float dist = glm::length(body - center);
        if (dist > radius) continue;
        const float dealt = damage * (1.0f - dist / radius);
        player->health -= dealt;
        if (player->health <= 0.0f) {
            log::info("server: player {} blew up player {}", source, peer);
            killPlayer(*player, peer, source); // blast now credits the source
        }
    }
    for (Npc& npc : m_npcs) {
        const float dist = glm::length(npc.pos + glm::vec3(0, 0.9f, 0) - center);
        if (dist > radius) continue;
        damageNpc(transport, npc, damage * (1.0f - dist / radius));
    }

    // Collect broken voxels, then broadcast ONE batched op message. A 4.5 m
    // crater spans ~6800 voxels; per-voxel applyVoxelOp would fire thousands of
    // reliable packets per rocket. Server state is still updated per voxel.
    const int r = static_cast<int>(std::ceil(radius / kVoxelSize));
    const glm::ivec3 c = glm::ivec3(glm::floor(center / kVoxelSize));
    std::vector<glm::ivec3> broken;
    for (int dy = -r; dy <= r; ++dy)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                const glm::ivec3 v = c + glm::ivec3(dx, dy, dz);
                const BlockId id = m_voxels.blockAt(v);
                if (id == 0) continue;
                const glm::vec3 vc = (glm::vec3(v) + 0.5f) * kVoxelSize;
                const float dist = glm::length(vc - center);
                if (dist > radius) continue;
                bool destroy = !m_rules.blockDamage;
                if (m_rules.blockDamage) {
                    const BlockDef& mat = m_voxels.blockRegistry().get(id);
                    auto [entry, inserted] = m_voxelDamage.try_emplace(v, mat.hp);
                    entry->second -= damage * (1.0f - dist / radius);
                    destroy = entry->second <= 0.0f;
                }
                if (destroy) broken.push_back(v);
            }
    if (broken.empty()) return;
    for (const glm::ivec3& v : broken) {
        m_voxelDamage.erase(v);
        m_voxels.setBlock(v, 0);
    }
    // One BatchVoxelOp to every client.
    ByteWriter w;
    w.write(static_cast<std::uint8_t>(MsgType::BatchVoxelOp));
    w.write(static_cast<std::uint32_t>(broken.size()));
    for (const glm::ivec3& v : broken) w.write(v);
    for (auto& [peer, unused] : m_players) transport.send(peer, w.data(), true);
}

void ServerSim::updateProjectiles(Transport& transport) {
    for (auto it = m_projectiles.begin(); it != m_projectiles.end();) {
        Projectile& p = *it;
        p.ownerGrace -= kFixedDtServer;
        p.vel.y -= p.gravity * kFixedDtServer;
        const glm::vec3 next = p.pos + p.vel * kFixedDtServer;

        bool detonate = false;
        glm::vec3 at = next;
        // Voxel impact along this step.
        const glm::vec3 step = next - p.pos;
        const float dist = glm::length(step);
        if (dist > 1e-4f) {
            if (const auto hit = m_voxels.raycast(p.pos, step / dist, dist)) {
                detonate = true;
                at = p.pos + (step / dist) * hit->t;
            }
        }
        // Player impact (skip the owner for the first moments handled by muzzle offset).
        if (!detonate) {
            for (auto& [peer, player] : m_players) {
                if (!player->spawned) continue;
                if (peer == p.owner && p.ownerGrace > 0.0f) continue; // clearing our own muzzle
                const glm::vec3 body = player->controller.position() + glm::vec3(0, 0.9f, 0);
                if (glm::length(body - next) < 0.6f) {
                    detonate = true;
                    at = next;
                    break;
                }
            }
        }
        if (!detonate) { // direct rocket hits on NPCs detonate too
            for (const Npc& npc : m_npcs) {
                if (npc.health <= 0.0f) continue;
                if (glm::length(npc.pos + glm::vec3(0, 0.9f, 0) - next) < 0.6f) {
                    detonate = true;
                    at = next;
                    break;
                }
            }
        }
        p.pos = next;
        p.life -= kFixedDtServer;
        if (p.life <= 0.0f) detonate = true;

        if (detonate) {
            // Route the blast through the effect core (behaviour-equal to the old
            // inline applyBlast, now data-driven — the list could add more effects).
            runEffects(transport, p.onImpact, p.owner, at, nullptr, nullptr);
            it = m_projectiles.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_deployables.begin(); it != m_deployables.end();) {
        Deployable& d = *it;
        d.armTime -= kFixedDtServer;
        bool triggered = false;
        for (auto& [peer, player] : m_players) {
            if (!player->spawned) continue;
            if (peer == d.owner && d.armTime > 0.0f) continue; // don't kill the layer while arming
            const glm::vec3 body = player->controller.position() + glm::vec3(0, 0.9f, 0);
            if (glm::length(body - d.pos) < d.triggerRange) {
                triggered = true;
                break;
            }
        }
        if (!triggered && d.armTime <= 0.0f) { // NPCs walking over a claymore set it off
            for (const Npc& npc : m_npcs) {
                if (npc.health <= 0.0f) continue;
                if (glm::length(npc.pos + glm::vec3(0, 0.9f, 0) - d.pos) < d.triggerRange) {
                    triggered = true;
                    break;
                }
            }
        }
        if (triggered) {
            runEffects(transport, d.onTrigger, d.owner, d.pos, nullptr, nullptr);
            it = m_deployables.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace meat
