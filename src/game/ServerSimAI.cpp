// ServerSim — enemy/ally AI: pathfinding and the NPC / turret / companion update
// loops.
//
// Split out of ServerSim.cpp (a god file) to keep concerns separable. These are
// ServerSim member functions in their own translation unit — behaviour identical.
// Contents: planPath (Detour-or-A* path), updateNpcs (aggro / melee / ranged /
// repath), updateTurrets (auto-defense targeting), updateCompanions (follow-owner
// ally). Death routes through killPlayer / damageNpc in ServerSimCombat.cpp.

#include "game/ServerSim.h"
#include "game/ServerSimInternal.h" // kFixedDtServer, defaultSpawnPos

#include "engine/core/Log.h"
#include "engine/core/ViewMath.h" // viewForward
#include "game/Pathfinder.h"      // findPath, snapToStandable

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace meat {

std::vector<glm::ivec3> ServerSim::planPath(glm::vec3 fromPos, glm::vec3 toPos, glm::ivec3 fromCell,
                                            glm::ivec3 toCell) {
    // Optional Detour navmesh first. Its string-pulled world corners are snapped
    // back onto standable voxel cells so the existing cell-follow logic is reused
    // verbatim. Any miss (no navmesh yet, unmapped endpoint, an unsnappable corner)
    // falls through to the voxel A* — the guaranteed, edit-aware fallback.
    std::vector<glm::vec3> corners;
    if (m_navmesh.queryPath(fromPos, toPos, corners) && corners.size() >= 2) {
        std::vector<glm::ivec3> cells{fromCell};
        bool ok = true;
        for (const glm::vec3& corner : corners) {
            glm::ivec3 c;
            if (!snapToStandable(m_voxels, corner, c)) {
                ok = false;
                break;
            }
            if (cells.back() != c) cells.push_back(c);
        }
        if (ok && cells.size() >= 2) return cells;
    }
    return findPath(m_voxels, fromCell, toCell, 1500);
}

void ServerSim::updateNpcs(Transport& transport) {
    constexpr float kAggroRange = 18.0f, kChaserSpeed = 3.2f, kShooterSpeed = 2.2f;
    constexpr float kZombieSpeed = 1.5f; // shamble — slower than a chaser rush
    constexpr float kMeleeRange = 1.4f, kShootRange = 14.0f;

    for (Npc& npc : m_npcs) {
        if (npc.health <= 0.0f) continue; // killed earlier this tick: no attacks from the grave
        // Unloaded chunk (co-op players far apart): every voxel reads air there —
        // LoS would wallhack and pathing would fail. Sleep until terrain exists.
        if (!m_voxels.isChunkLoaded(voxelToChunk(worldToVoxel(npc.pos)))) continue;
        npc.repathTimer -= kFixedDtServer;
        npc.attackCooldown -= kFixedDtServer;
        // Decays toward idle every tick; the path-follow step below refreshes it to full walk
        // while the NPC is actually stepping. This is the authoritative walk weight the client
        // reads from EntityState.anim (client-side speed-from-interp read ~0 and froze the blend).
        npc.animSpeed *= 0.80f;

        // Acquire/keep the nearest visible player.
        PeerId bestPeer = 0;
        Player* bestPlayer = nullptr;
        float bestDist = kAggroRange;
        for (auto& [peer, player] : m_players) {
            if (!player->spawned) continue;
            const glm::vec3 eyeTo = player->controller.position() + glm::vec3(0, 0.9f, 0);
            const glm::vec3 from = npc.pos + glm::vec3(0, 1.2f, 0);
            const float dist = glm::length(eyeTo - from);
            if (dist >= bestDist) continue;
            const glm::vec3 dir = (eyeTo - from) / std::max(dist, 1e-4f);
            if (const auto hit = m_voxels.raycast(from, dir, dist); hit) continue; // wall
            bestDist = dist;
            bestPeer = peer;
            bestPlayer = player.get();
        }
        npc.target = bestPeer;
        if (!bestPlayer) continue; // idle; schedules/wander land later

        const glm::vec3 targetPos = bestPlayer->controller.position();
        const glm::vec3 toTarget = targetPos - npc.pos;
        const float dist = glm::length(toTarget);
        npc.yaw = std::atan2(-toTarget.x, -toTarget.z); // face target (viewForward inverse)

        // Attack when in envelope. Chasers and zombies both melee; zombies swing
        // slower but hit harder.
        const bool melee =
            npc.type == EntityArchetype::NpcChaser || npc.type == EntityArchetype::NpcZombie;
        if (melee && dist < kMeleeRange) {
            if (npc.attackCooldown <= 0.0f) {
                const bool zombie = npc.type == EntityArchetype::NpcZombie;
                npc.attackCooldown = zombie ? 1.6f : 1.0f;
                bestPlayer->health -= zombie ? 15.0f : 12.0f;
                if (bestPlayer->health <= 0.0f) {
                    log::info("server: player {} was mauled", bestPeer);
                    killPlayer(*bestPlayer, bestPeer, 0); // NPC/environment kill
                }
            }
            continue; // in melee range: no need to path
        }
        if (npc.type == EntityArchetype::NpcShooter && dist < kShootRange) {
            if (npc.attackCooldown <= 0.0f) {
                npc.attackCooldown = 1.4f;
                bestPlayer->health -= 8.0f; // LoS already verified above
                if (bestPlayer->health <= 0.0f) {
                    log::info("server: player {} was shot down", bestPeer);
                    killPlayer(*bestPlayer, bestPeer, 0); // NPC/environment kill
                }
            }
            if (dist < kShootRange * 0.6f) continue; // holds distance, doesn't rush
        }

        // (Re)path on the timer, or immediately when a non-empty path ran out.
        // An EMPTY path must wait for the timer — otherwise an unreachable
        // target re-runs a full failed A* every tick and spirals the server.
        // Jitter desynchronizes a room that aggroed on the same tick.
        if (npc.repathTimer <= 0.0f ||
            (npc.pathIndex >= npc.path.size() && !npc.path.empty())) {
            npc.repathTimer = 0.6f + 0.01f * static_cast<float>(npc.id % 16);
            glm::ivec3 from, to;
            if (snapToStandable(m_voxels, npc.pos, from) &&
                snapToStandable(m_voxels, targetPos, to)) {
                npc.path = planPath(npc.pos, targetPos, from, to);
                npc.pathIndex = npc.path.size() > 1 ? 1 : 0; // [0] is where we stand
            } else {
                npc.path.clear();
            }
        }

        // Follow the path kinematically (voxel cells → world centers).
        if (npc.pathIndex < npc.path.size()) {
            const glm::vec3 waypoint =
                (glm::vec3(npc.path[npc.pathIndex]) + glm::vec3(0.5f, 0.0f, 0.5f)) *
                kVoxelSize;
            const glm::vec3 delta = waypoint - npc.pos;
            const float speed = npc.type == EntityArchetype::NpcChaser  ? kChaserSpeed
                                : npc.type == EntityArchetype::NpcZombie ? kZombieSpeed
                                                                         : kShooterSpeed;
            const float stepLen = speed * kFixedDtServer;
            if (glm::length(delta) <= stepLen) {
                npc.pos = waypoint;
                ++npc.pathIndex;
            } else {
                npc.pos += glm::normalize(delta) * stepLen;
            }
            npc.animSpeed = 1.0f; // stepping this tick → full walk (decays back to idle when stopped)
        }
    }

    std::erase_if(m_npcs, [](const Npc& n) { return n.health <= 0.0f; });
    (void)transport;
}

void ServerSim::updateTurrets(Transport& transport) {
    constexpr float kRange = 22.0f, kDamage = 34.0f, kInterval = 0.7f;
    for (Turret& t : m_turrets) {
        if (!m_voxels.isChunkLoaded(voxelToChunk(worldToVoxel(t.pos)))) continue;
        t.fireCooldown -= kFixedDtServer;
        const glm::vec3 muzzle = t.pos + glm::vec3(0, 0.6f, 0);

        // Nearest hostile NPC in range with a clear line of sight.
        Npc* target = nullptr;
        float best = kRange;
        for (Npc& npc : m_npcs) {
            if (npc.health <= 0.0f) continue;
            const glm::vec3 to = npc.pos + glm::vec3(0, 0.9f, 0) - muzzle;
            const float d = glm::length(to);
            if (d >= best) continue;
            if (m_voxels.raycast(muzzle, to / std::max(d, 1e-4f), d)) continue; // wall
            best = d;
            target = &npc;
        }
        if (!target) continue;

        const glm::vec3 to = target->pos - t.pos;
        t.yaw = std::atan2(-to.x, -to.z);
        if (t.fireCooldown <= 0.0f) {
            t.fireCooldown = kInterval;
            damageNpc(transport, *target, kDamage);
        }
    }
}

// Mobile ally: engages the nearest hostile NPC in range (hitscan on a cadence), else
// follows its owner. Reuses the NPC A* pathing to move toward the goal (target or owner).
// NPCs don't aggro companions yet, so a companion is a durable escort, not a decoy.
void ServerSim::updateCompanions(Transport& transport) {
    constexpr float kEngage = 18.0f, kAttackRange = 14.0f, kDamage = 22.0f, kInterval = 0.9f;
    constexpr float kFollowDist = 4.0f, kSpeed = 3.6f;
    for (Companion& c : m_companions) {
        if (!m_voxels.isChunkLoaded(voxelToChunk(worldToVoxel(c.pos)))) continue;
        c.fireCooldown -= kFixedDtServer;
        c.repathTimer -= kFixedDtServer;
        const glm::vec3 muzzle = c.pos + glm::vec3(0, 1.4f, 0);

        // Acquire the nearest hostile NPC in engage range with a clear line of sight.
        Npc* target = nullptr;
        float best = kEngage;
        for (Npc& npc : m_npcs) {
            if (npc.health <= 0.0f) continue;
            const glm::vec3 to = npc.pos + glm::vec3(0, 0.9f, 0) - muzzle;
            const float d = glm::length(to);
            if (d >= best) continue;
            if (m_voxels.raycast(muzzle, to / std::max(d, 1e-4f), d)) continue; // wall
            best = d;
            target = &npc;
        }

        glm::vec3 goal{0};
        bool haveGoal = false;
        if (target) {
            const glm::vec3 to = target->pos - c.pos;
            c.yaw = std::atan2(-to.x, -to.z); // face the enemy
            if (glm::length(to) <= kAttackRange) {
                if (c.fireCooldown <= 0.0f) {
                    c.fireCooldown = kInterval;
                    damageNpc(transport, *target, kDamage);
                }
            } else {
                goal = target->pos; // chase into firing range
                haveGoal = true;
            }
        } else if (const auto it = m_players.find(c.owner);
                   it != m_players.end() && it->second->spawned) {
            const glm::vec3 op = it->second->controller.position();
            const glm::vec3 to = op - c.pos;
            if (glm::length(to) > kFollowDist) { // hang back when already close
                goal = op;
                haveGoal = true;
                c.yaw = std::atan2(-to.x, -to.z);
            }
        }

        if (!haveGoal) continue;
        // (Re)path toward the goal on the timer or when the current path runs out.
        if (c.repathTimer <= 0.0f || (c.pathIndex >= c.path.size() && !c.path.empty())) {
            c.repathTimer = 0.5f + 0.01f * static_cast<float>(c.id % 16);
            glm::ivec3 from, to;
            if (snapToStandable(m_voxels, c.pos, from) && snapToStandable(m_voxels, goal, to)) {
                c.path = planPath(c.pos, goal, from, to);
                c.pathIndex = c.path.size() > 1 ? 1 : 0;
            } else {
                c.path.clear();
            }
        }
        if (c.pathIndex < c.path.size()) {
            const glm::vec3 wp =
                (glm::vec3(c.path[c.pathIndex]) + glm::vec3(0.5f, 0.0f, 0.5f)) * kVoxelSize;
            const glm::vec3 delta = wp - c.pos;
            const float step = kSpeed * kFixedDtServer;
            if (glm::length(delta) <= step) {
                c.pos = wp;
                ++c.pathIndex;
            } else {
                c.pos += glm::normalize(delta) * step;
            }
        }
    }
}

} // namespace meat
