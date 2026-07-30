#include "game/ServerSim.h"
#include "engine/core/Log.h"
#include "engine/core/ViewMath.h"
#include "game/DungeonGen.h"
#include "game/Pathfinder.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

namespace meat {
namespace {
constexpr float kFixedDtServer = 1.0f / 60.0f;
constexpr int kSnapshotEvery = 3; // 60 Hz sim → 20 Hz snapshots
constexpr glm::vec3 kSpawnPos{8.0f, 8.0f, 8.0f};

constexpr float kPlaceInterval = 0.20f;
constexpr float kHitscanRange = 60.0f;
constexpr float kCapsuleRadius = 0.35f; // keep in sync with CharacterTuning

// Distance between a ray segment [ro, ro + rd*range] and segment [a, b];
// tRayOut = distance along the ray at the closest approach. Standard clamped
// closest-point-of-two-segments; rd must be unit length.
float raySegmentDistance(glm::vec3 ro, glm::vec3 rd, float range, glm::vec3 a, glm::vec3 b,
                         float& tRayOut) {
    const glm::vec3 u = rd * range, v = b - a, w0 = ro - a;
    const float A = glm::dot(u, u), B = glm::dot(u, v), C = glm::dot(v, v);
    const float D = glm::dot(u, w0), E = glm::dot(v, w0);
    const float denom = A * C - B * B;
    float s = denom > 1e-6f ? glm::clamp((B * E - C * D) / denom, 0.0f, 1.0f) : 0.0f;
    float t = C > 1e-6f ? glm::clamp((B * s + E) / C, 0.0f, 1.0f) : 0.0f;
    // Re-clamp s against the chosen t (one refinement is exact for segments).
    s = A > 1e-6f ? glm::clamp((B * t - D) / A, 0.0f, 1.0f) : 0.0f;
    tRayOut = s * range;
    return glm::length((ro + u * s) - (a + v * t));
}
} // namespace

bool ServerSim::init(std::uint32_t worldSeed) {
    m_seed = worldSeed;
    if (!m_physics.init()) return false;
    m_jobs.start(std::thread::hardware_concurrency());

    m_palette = registerDefaultBlocks(m_voxels.blockRegistry());
    m_defaultItems = registerDefaultItems(m_items, m_palette.stone);
    m_voxels.setGenerator(makeTerrainGenerator(m_seed, m_palette));
    m_voxels.setMeshReadyCallback([this](ChunkPos pos, ChunkMeshData data) {
        if (!data.indices.empty())
            m_physics.syncChunkCollider(pos, data);
        else
            m_physics.removeChunkCollider(pos);
    });
    m_voxels.setChunkUnloadedCallback(
        [this](ChunkPos pos) { m_physics.removeChunkCollider(pos); });
    spawnDungeonLoot();
    spawnDungeonNpcs();
    setupScripting();
    m_scripts.onInit(m_seed);
    return true;
}

void ServerSim::setupScripting() {
    m_scriptRng ^= m_seed * 0x9E3779B97F4A7C15ull; // per-world deterministic stream
    // Capability table: scripts get exactly these server-authoritative actions.
    ScriptApi api;
    api.log = [](const std::string& s) { log::info("[lua] {}", s); };
    api.setBlock = [this](int x, int y, int z, int b) {
        const auto id = static_cast<BlockId>(b < 0 ? 0 : b);
        if (!m_voxels.blockRegistry().isValid(id)) return;
        if (std::abs(x) > 100000 || std::abs(y) > 100000 || std::abs(z) > 100000)
            return; // same guard as the network path; unbounded coords → OOM
        // Edits go through applyVoxelOp when a client is connected (broadcasts);
        // otherwise into the overlay, which overlay-on-join replays to joiners.
        if (m_activeTransport)
            applyVoxelOp(*m_activeTransport, {glm::ivec3(x, y, z), id});
        else
            m_voxels.setBlock(glm::ivec3(x, y, z), id);
    };
    api.getBlock = [this](int x, int y, int z) {
        return static_cast<int>(m_voxels.blockAt(glm::ivec3(x, y, z)));
    };
    api.spawnPickup = [this](float x, float y, float z, int item, int count) {
        if (item <= 0 || count <= 0) return;
        if (m_entities.size() >= 4096) return; // entity cap: a runaway loop can't OOM us
        WorldEntity e;
        e.id = m_nextEntityId++;
        e.type = EntityArchetype::ItemPickup;
        e.pos = {x, y, z};
        e.item = static_cast<ItemId>(item);
        e.count = static_cast<std::uint16_t>(std::min(count, 0xFFFF)); // clamp, no wrap
        m_entities.push_back(e);
    };
    api.playerCount = [this] { return playerCount(); };
    api.tick = [this] { return m_tick; };
    // Deterministic seeded RNG so scripted content matches across peers/replays.
    api.randi = [this](int lo, int hi) {
        if (hi < lo) std::swap(lo, hi);
        m_scriptRng = m_scriptRng * 6364136223846793005ull + 1442695040888963407ull;
        // Widen before subtracting: (hi - lo) in int would overflow on a wide range.
        const std::uint64_t span =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(hi) - lo) + 1;
        return static_cast<int>(lo + static_cast<std::int64_t>((m_scriptRng >> 33) % span));
    };
    api.itemId = [this](const std::string& n) -> int {
        if (n == "pistol") return m_defaultItems.pistol;
        if (n == "ammo9mm") return m_defaultItems.ammo9mm;
        if (n == "shells") return m_defaultItems.shells;
        if (n == "rockets") return m_defaultItems.rockets;
        if (n == "medkit") return m_defaultItems.medkit;
        if (n == "stone") return m_defaultItems.stoneBlock;
        if (n == "rpg") return m_defaultItems.rpg;
        if (n == "shotgun") return m_defaultItems.shotgun;
        return 0;
    };
    m_scripts.bind(std::move(api));
    m_scripts.loadDir(m_scriptDir);
}

void ServerSim::spawnDungeonNpcs() {
    const DungeonLayout layout = DungeonLayout::generate(m_seed, {});
    std::size_t i = 0;
    for (const auto& room : layout.rooms()) {
        // Chasers guard every 3rd room, shooters every 5th; entrance room stays clear.
        const bool chaser = i % 3 == 1, shooter = i % 5 == 4;
        if (chaser || shooter) {
            Npc npc;
            npc.id = m_nextEntityId++;
            npc.type = chaser ? EntityArchetype::NpcChaser : EntityArchetype::NpcShooter;
            npc.health = chaser ? 60.0f : 40.0f;
            const glm::ivec3 c{std::min((room.min.x + room.max.x) / 2 + 1, room.max.x),
                               room.min.y,
                               std::min((room.min.z + room.max.z) / 2 + 1, room.max.z)};
            npc.pos = (glm::vec3(c) + glm::vec3(0.5f, 0.0f, 0.5f)) * kVoxelSize;
            m_npcs.push_back(std::move(npc));
        }
        ++i;
    }
    log::info("server: spawned {} dungeon NPCs", m_npcs.size());
}

void ServerSim::damageNpc(Transport& transport, Npc& npc, float damage) {
    if (npc.health <= 0.0f) return; // already dead: no double loot from multi-pellet kills
    npc.health -= damage;
    if (npc.health > 0.0f) return;
    // Death: drop a small ammo cache where it fell (survivors loot the room).
    WorldEntity drop;
    drop.id = m_nextEntityId++;
    drop.type = EntityArchetype::ItemPickup;
    drop.pos = npc.pos + glm::vec3(0, 0.3f, 0);
    drop.item = m_defaultItems.ammo9mm;
    drop.count = 12;
    m_entities.push_back(drop);
    (void)transport; // death effects (sound/particles) ride future events
}

void ServerSim::updateNpcs(Transport& transport) {
    constexpr float kAggroRange = 18.0f, kChaserSpeed = 3.2f, kShooterSpeed = 2.2f;
    constexpr float kMeleeRange = 1.4f, kShootRange = 14.0f;

    for (Npc& npc : m_npcs) {
        if (npc.health <= 0.0f) continue; // killed earlier this tick: no attacks from the grave
        // Unloaded chunk (co-op players far apart): every voxel reads air there —
        // LoS would wallhack and pathing would fail. Sleep until terrain exists.
        if (!m_voxels.isChunkLoaded(voxelToChunk(worldToVoxel(npc.pos)))) continue;
        npc.repathTimer -= kFixedDtServer;
        npc.attackCooldown -= kFixedDtServer;

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

        // Attack when in envelope.
        if (npc.type == EntityArchetype::NpcChaser && dist < kMeleeRange) {
            if (npc.attackCooldown <= 0.0f) {
                npc.attackCooldown = 1.0f;
                bestPlayer->health -= 12.0f;
                if (bestPlayer->health <= 0.0f) {
                    log::info("server: player {} was mauled", bestPeer);
                    bestPlayer->controller.setState(kSpawnPos, glm::vec3(0));
                    bestPlayer->health = 100.0f;
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
                    bestPlayer->controller.setState(kSpawnPos, glm::vec3(0));
                    bestPlayer->health = 100.0f;
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
                npc.path = findPath(m_voxels, from, to, 1500);
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
            const float speed =
                npc.type == EntityArchetype::NpcChaser ? kChaserSpeed : kShooterSpeed;
            const float stepLen = speed * kFixedDtServer;
            if (glm::length(delta) <= stepLen) {
                npc.pos = waypoint;
                ++npc.pathIndex;
            } else {
                npc.pos += glm::normalize(delta) * stepLen;
            }
        }
    }

    std::erase_if(m_npcs, [](const Npc& n) { return n.health <= 0.0f; });
    (void)transport;
}

void ServerSim::spawnDungeonLoot() {
    // Same pure function the terrain generator uses — identical layout for free.
    const DungeonLayout layout = DungeonLayout::generate(m_seed, {});
    std::size_t roomIndex = 0;
    for (const auto& room : layout.rooms()) {
        if (roomIndex >= 16) break;
        const glm::ivec3 c{(room.min.x + room.max.x) / 2, room.min.y,
                           (room.min.z + room.max.z) / 2};
        WorldEntity e;
        e.id = m_nextEntityId++;
        e.type = EntityArchetype::ItemPickup;
        e.pos = (glm::vec3(c) + glm::vec3(0.5f, 0.2f, 0.5f)) * kVoxelSize;
        e.pos.y = static_cast<float>(c.y) * kVoxelSize + 0.3f; // resting on the floor
        if (roomIndex % 3 == 2) {
            e.item = m_defaultItems.medkit;
            e.count = 1;
        } else {
            e.item = m_defaultItems.ammo9mm;
            e.count = 24;
        }
        m_entities.push_back(e);
        ++roomIndex;
    }
    log::info("server: spawned {} loot pickups in dungeon rooms", m_entities.size());
}

// Penetrating hitscan: the ray marches through materials spending a penetration
// budget; each block crossed attenuates damage; flesh stops the bullet. Chip
// damage accumulates in the sparse map until a block breaks into a VoxelOp.
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
    const glm::vec3 eye =
        player.controller.position() + glm::vec3(0, player.controller.eyeHeight(), 0);

    glm::vec3 origin = eye;
    float remaining = kHitscanRange;
    float budget = weapon.penBudget;
    float damageScale = 1.0f;

    for (int hop = 0; hop < 8 && remaining > 0.1f; ++hop) {
        const auto voxelHit = m_voxels.raycast(origin, dir, remaining);
        const float segmentEnd = voxelHit ? voxelHit->t : remaining;

        // Closest player capsule within this air segment beats the wall.
        Player* victim = nullptr;
        PeerId victimPeer = 0;
        float bestT = segmentEnd;
        for (auto& [otherPeer, other] : m_players) {
            if (otherPeer == peer || !other->spawned) continue;
            const glm::vec3 feet = other->controller.position();
            const float height = other->controller.crouched() ? 0.95f : 1.8f;
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

        if (npcVictim) {
            damageNpc(transport, *npcVictim, weapon.damage * damageScale);
            return;
        }
        if (victim) {
            victim->health -= weapon.damage * damageScale;
            if (victim->health <= 0.0f) {
                log::info("server: player {} fragged player {}", peer, victimPeer);
                victim->controller.setState(kSpawnPos, glm::vec3(0));
                victim->health = 100.0f;
            }
            return; // flesh stops bullets (AP ammo types may change this later)
        }
        if (!voxelHit || voxelHit->block == 0) return;

        // Chip the block.
        const BlockDef& material = m_voxels.blockRegistry().get(voxelHit->block);
        bool broke = !m_rules.blockDamage; // instant-break rules skip the hp model
        if (m_rules.blockDamage) {
            auto [entry, inserted] = m_voxelDamage.try_emplace(voxelHit->voxel, material.hp);
            entry->second -= weapon.damage * damageScale;
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
    m_projectiles.push_back({m_nextEntityId++, owner, pos, vel, weapon.projectileGravity,
                             weapon.blastRadius, weapon.blastDamage, 6.0f});
}

// Radial damage: players by distance falloff, and every solid voxel in range
// takes damage scaled by proximity (explosives carve craters). Server-only.
void ServerSim::applyBlast(Transport& transport, PeerId source, glm::vec3 center,
                           float radius, float damage) {
    for (auto& [peer, player] : m_players) {
        if (!player->spawned) continue;
        const glm::vec3 body = player->controller.position() + glm::vec3(0, 0.9f, 0);
        const float dist = glm::length(body - center);
        if (dist > radius) continue;
        const float dealt = damage * (1.0f - dist / radius);
        player->health -= dealt;
        if (player->health <= 0.0f) {
            log::info("server: player {} blew up player {}", source, peer);
            player->controller.setState(kSpawnPos, glm::vec3(0));
            player->health = 100.0f;
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
            applyBlast(transport, p.owner, at, p.radius, p.damage);
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
            applyBlast(transport, d.owner, d.pos, d.radius, d.damage);
            it = m_deployables.erase(it);
        } else {
            ++it;
        }
    }
}

bool ServerSim::tryPickup(Transport& transport, PeerId peer, Player& player) {
    const glm::vec3 feet = player.controller.position();
    for (auto it = m_entities.begin(); it != m_entities.end(); ++it) {
        if (it->type != EntityArchetype::ItemPickup) continue;
        const glm::vec3 d = it->pos - (feet + glm::vec3(0, 0.9f, 0));
        if (glm::dot(d, d) > 1.5f * 1.5f) continue;
        const std::uint16_t leftover = player.inventory.add(it->item, it->count, m_items);
        if (leftover == it->count) continue; // bag full for THIS item; try other loot
        if (leftover == 0) {
            m_entities.erase(it); // absent from the next snapshot = despawned
        } else {
            it->count = leftover;
        }
        sendInventory(transport, peer, player);
        return true; // one pickup per press
    }
    return false;
}

void ServerSim::pump(Transport& transport) {
    m_activeTransport = &transport; // script callbacks may broadcast
    std::vector<NetEvent> events;
    transport.poll(events);
    for (NetEvent& e : events) {
        switch (e.type) {
        case NetEvent::Type::Connected:
            log::info("server: peer {} connected", e.peer);
            m_players.emplace(e.peer, std::make_unique<Player>());
            break;
        case NetEvent::Type::Disconnected:
            log::info("server: peer {} disconnected", e.peer);
            m_players.erase(e.peer);
            break;
        case NetEvent::Type::Packet:
            handlePacket(transport, e.peer, e.data);
            break;
        }
    }
}

void ServerSim::handlePacket(Transport& transport, PeerId peer,
                             std::span<const std::byte> data) {
    const auto type = peekType(data);
    if (!type) return;
    auto it = m_players.find(peer);
    if (it == m_players.end()) return;
    Player& player = *it->second;
    ByteReader reader(data.subspan(1));

    switch (*type) {
    case MsgType::Hello: {
        HelloMsg hello;
        if (!decode(hello, reader)) return;
        if (player.helloDone) return; // replayed Hello must not re-grant loadout
        player.helloDone = true;
        WelcomeMsg welcome{peer, m_seed, m_tick,
                           static_cast<std::uint8_t>(m_rules.inventoryModel),
                           m_rules.flagsByte()};
        transport.send(peer, pack(welcome), true);
        if (m_pendingRestore) { // save-file state goes to the first arrival
            player.health = m_pendingRestore->health;
            player.inventory = m_pendingRestore->inventory;
            player.spawnOverride = m_pendingRestore->pos; // applied at spawn in tick()
            m_pendingRestore.reset();
        } else {
            giveStartingLoadout(player);
        }
        sendInventory(transport, peer, player);
        sendOverlayTo(transport, peer); // world edits (editor/script/save) → this client
        log::info("server: '{}' joined as player {}", hello.name, peer);
        m_scripts.onPlayerJoin(peer);
        break;
    }
    case MsgType::Command: {
        CommandMsg msg;
        if (!decode(msg, reader)) return;
        // Strictly-increasing after the first accepted command; hasCmd closes
        // the tick-0 replay window (tick 0 would else re-pass the != 0 guard).
        if (player.hasCmd && msg.cmd.tick <= player.lastCmdTick) return; // stale/replayed
        player.lastCmd = msg.cmd;
        player.lastCmdTick = msg.cmd.tick;
        player.hasCmd = true;
        break;
    }
    case MsgType::VoxelOp: {
        VoxelOpMsg op;
        if (!decode(op, reader)) return;
        // Client intent (editor brushes). Validate: a hostile block id would hit
        // the registry assert server-side. No range gate — the editor legitimately
        // builds far from the player body; per-peer edit permissions are the TODO.
        if (!m_voxels.blockRegistry().isValid(op.block)) return;
        if (glm::abs(op.voxel.x) > 100000 || glm::abs(op.voxel.y) > 100000 ||
            glm::abs(op.voxel.z) > 100000)
            return;
        applyVoxelOp(transport, op);
        break;
    }
    default:
        break;
    }
}

void ServerSim::tick(Transport& transport) {
    m_activeTransport = &transport;
    m_jobs.drainMainThread(); // collider syncs from finished mesh jobs

    glm::vec3 streamCenter = kSpawnPos;
    bool first = true;
    for (auto& [peer, player] : m_players) {
        if (!player->spawned) {
            if (!player->controller.init(m_physics, player->spawnOverride.value_or(kSpawnPos)))
                continue;
            player->spawned = true;
        }
        player->controller.update(player->lastCmd, kFixedDtServer, m_physics);
        if (player->controller.position().y < -30.0f) // fell out (colliders pending)
            player->controller.setState(kSpawnPos, glm::vec3(0));
        processCombat(transport, peer, *player);
        if (first) { // TODO: multi-center streaming once co-op players roam apart
            streamCenter = player->controller.position();
            first = false;
        }
    }
    m_physics.step(kFixedDtServer);
    updateProjectiles(transport);
    updateNpcs(transport);
    m_voxels.update(streamCenter, m_jobs);

    ++m_tick;
    if (m_scripts.loaded() && m_tick % 20 == 0) m_scripts.onTick(m_tick); // ~3 Hz gameplay hook
    if (m_tick % kSnapshotEvery == 0 && !m_players.empty()) broadcastSnapshot(transport);
}

void ServerSim::applyVoxelOp(Transport& transport, const VoxelOpMsg& op) {
    m_voxelDamage.erase(op.voxel); // chip damage dies with the block; fresh block = full hp
    m_voxels.setBlock(op.voxel, op.block);
    for (auto& [peer, unused] : m_players) transport.send(peer, pack(op), true);
}

void ServerSim::giveStartingLoadout(Player& player) {
    // Full reference arsenal so every weapon archetype is reachable in the slice.
    player.inventory.add(m_defaultItems.pistol, 1, m_items);
    player.inventory.add(m_defaultItems.smg, 1, m_items);
    player.inventory.add(m_defaultItems.shotgun, 1, m_items);
    player.inventory.add(m_defaultItems.sniper, 1, m_items);
    player.inventory.add(m_defaultItems.rpg, 1, m_items);
    player.inventory.add(m_defaultItems.grenade, 3, m_items);
    player.inventory.add(m_defaultItems.claymore, 2, m_items);
    player.inventory.add(m_defaultItems.ammo9mm, 90, m_items);
    player.inventory.add(m_defaultItems.shells, 24, m_items);
    player.inventory.add(m_defaultItems.rifleAmmo, 30, m_items);
    player.inventory.add(m_defaultItems.rockets, 4, m_items);
    player.inventory.add(m_defaultItems.medkit, 2, m_items);
    player.inventory.add(m_defaultItems.stoneBlock, 32, m_items);
}

void ServerSim::sendOverlayTo(Transport& transport, PeerId peer) const {
    // Clients regenerate terrain from the seed, so any server-side edit (editor
    // brush, script, or a loaded save) must be replayed or the joiner desyncs.
    for (const auto& [cp, edits] : m_voxels.editOverlay()) {
        for (const auto& [index, block] : edits) {
            const int x = index % kChunkSize, z = (index / kChunkSize) % kChunkSize,
                      y = index / (kChunkSize * kChunkSize);
            const glm::ivec3 v(cp.x * kChunkSize + x, cp.y * kChunkSize + y,
                               cp.z * kChunkSize + z);
            transport.send(peer, pack(VoxelOpMsg{v, block}), true);
        }
    }
}

void ServerSim::sendInventory(Transport& transport, PeerId peer, const Player& player) const {
    ByteWriter w;
    w.write(static_cast<std::uint8_t>(MsgType::Inventory));
    player.inventory.encode(w);
    transport.send(peer, w.data(), true);
}

void ServerSim::processCombat(Transport& transport, PeerId peer, Player& player) {
    player.fireCooldown -= kFixedDtServer;
    player.placeCooldown -= kFixedDtServer;
    player.useCooldown -= kFixedDtServer;

    const glm::vec3 eye =
        player.controller.position() + glm::vec3(0, player.controller.eyeHeight(), 0);
    const glm::vec3 dir = viewForward(player.lastCmd.yaw, player.lastCmd.pitch);
    const int slotIndex = player.lastCmd.selectedSlot % Inventory::kSlots;
    const ItemStack& held = player.inventory.slot(slotIndex);
    const ItemDef& heldDef = m_items.get(held.id);

    if (player.lastCmd.fire && player.fireCooldown <= 0.0f) {
        if (heldDef.type == ItemType::Weapon) {
            const bool hasAmmo = !m_rules.finiteAmmo || heldDef.ammoItem == 0 ||
                                 player.inventory.countOf(heldDef.ammoItem) > 0;
            // Thrown/placed weapons (grenade/claymore) consume the weapon item
            // itself; hitscan/rpg consume their ammo item.
            const bool selfConsumed = heldDef.delivery != DeliveryKind::Hitscan &&
                                      heldDef.ammoItem == 0;
            const bool hasCharge = !m_rules.finiteAmmo || !selfConsumed || held.count > 0;
            if (hasAmmo && hasCharge) {
                player.fireCooldown = heldDef.fireInterval;
                if (m_rules.finiteAmmo && heldDef.ammoItem != 0) {
                    player.inventory.remove(heldDef.ammoItem, 1);
                    player.inventoryDirty = true;
                } else if (m_rules.finiteAmmo && selfConsumed) {
                    player.inventory.remove(held.id, 1);
                    player.inventoryDirty = true;
                }

                const glm::vec3 muzzle = eye + dir * 0.6f;
                if (heldDef.delivery == DeliveryKind::Hitscan) {
                    fireHitscan(transport, peer, player, heldDef);
                } else if (heldDef.delivery == DeliveryKind::Projectile) {
                    const glm::vec3 vel = dir * heldDef.projectileSpeed;
                    spawnProjectile(peer, muzzle, vel, heldDef);
                } else if (heldDef.delivery == DeliveryKind::Deployable) {
                    // Stick it on the surface the player is looking at (short reach).
                    glm::vec3 at = eye + dir * 2.5f;
                    if (const auto hit = m_voxels.raycast(eye, dir, 3.0f))
                        at = eye + dir * std::max(0.5f, hit->t - 0.2f);
                    Deployable dep;
                    dep.id = m_nextEntityId++;
                    dep.owner = peer;
                    dep.pos = at;
                    dep.radius = heldDef.blastRadius;
                    dep.damage = heldDef.blastDamage;
                    m_deployables.push_back(dep); // armTime/triggerRange keep their defaults
                }
            }
        } else if (heldDef.type == ItemType::Block) {
            // Holding a block: LMB is the mining tool (short range, no player damage).
            player.fireCooldown = kPlaceInterval;
            if (const auto hit = m_voxels.raycast(eye, dir, 6.0f); hit && hit->block != 0) {
                applyVoxelOp(transport, {hit->voxel, 0});
                if (m_rules.minedBlockDrops) {
                    player.inventory.add(m_defaultItems.stoneBlock, 1, m_items);
                    player.inventoryDirty = true;
                }
            }
        }
    }

    if (player.lastCmd.place && player.placeCooldown <= 0.0f &&
        heldDef.type == ItemType::Block) {
        const bool hasBlocks = !m_rules.minedBlockDrops || held.count > 0;
        if (hasBlocks) {
            player.placeCooldown = kPlaceInterval;
            if (const auto hit = m_voxels.raycast(eye, dir, 8.0f);
                hit && hit->normal != glm::ivec3(0)) {
                const glm::ivec3 target = hit->voxel + hit->normal;
                // Don't entomb anyone: reject placement overlapping a player capsule.
                const glm::vec3 center = (glm::vec3(target) + 0.5f) * kVoxelSize;
                bool blocked = false;
                for (auto& [otherPeer, other] : m_players) {
                    if (!other->spawned) continue;
                    const glm::vec3 feet = other->controller.position();
                    if (glm::abs(center.x - feet.x) < 0.6f &&
                        glm::abs(center.z - feet.z) < 0.6f && center.y > feet.y - 0.5f &&
                        center.y < feet.y + 2.0f) {
                        blocked = true;
                        break;
                    }
                }
                if (!blocked && m_voxels.blockAt(target) == 0) {
                    applyVoxelOp(transport, {target, heldDef.blockId});
                    if (m_rules.minedBlockDrops) {
                        player.inventory.remove(held.id, 1);
                        player.inventoryDirty = true;
                    }
                }
            }
        }
    }

    if (player.lastCmd.use && player.useCooldown <= 0.0f) {
        player.useCooldown = 0.5f;
        const bool grabbed = tryPickup(transport, peer, player); // loot wins over consuming
        if (!grabbed && heldDef.type == ItemType::Consumable && player.health < 100.0f) {
            player.inventory.remove(held.id, 1);
            player.health = glm::min(100.0f, player.health + 50.0f);
            player.inventoryDirty = true;
        }
    }

    if (player.inventoryDirty) {
        sendInventory(transport, peer, player);
        player.inventoryDirty = false;
    }
}

bool ServerSim::saveTo(const std::string& path) const {
    nlohmann::json j;
    j["seed"] = m_seed;
    j["tick"] = m_tick;

    nlohmann::json chunks = nlohmann::json::array();
    for (const auto& [pos, edits] : m_voxels.editOverlay()) {
        nlohmann::json entry;
        entry["pos"] = {pos.x, pos.y, pos.z};
        nlohmann::json cells = nlohmann::json::array();
        for (const auto& [index, block] : edits) cells.push_back({index, block});
        entry["cells"] = std::move(cells);
        chunks.push_back(std::move(entry));
    }
    j["chunks"] = std::move(chunks);

    nlohmann::json players = nlohmann::json::array();
    for (const auto& [peer, player] : m_players) {
        if (!player->spawned) continue;
        const glm::vec3 p = player->controller.position();
        nlohmann::json entry;
        entry["pos"] = {p.x, p.y, p.z};
        entry["health"] = player->health;
        nlohmann::json inv = nlohmann::json::array();
        for (int i = 0; i < Inventory::kSlots; ++i)
            inv.push_back({player->inventory.slot(i).id, player->inventory.slot(i).count});
        entry["inventory"] = std::move(inv);
        players.push_back(std::move(entry));
    }
    j["players"] = std::move(players);

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        log::error("save: cannot open '{}'", path);
        return false;
    }
    out << j.dump();
    out.flush();
    if (!out.good()) { // disk full / write error must not report success
        log::error("save: write to '{}' failed", path);
        return false;
    }
    log::info("saved to '{}' ({} edited chunks, {} players)", path, j["chunks"].size(),
              j["players"].size());
    return true;
}

bool ServerSim::initFromSave(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        log::error("load: cannot open '{}'", path);
        return false;
    }
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.contains("seed")) {
        log::error("load: '{}' is not a valid save", path);
        return false;
    }
    if (!init(j["seed"].get<std::uint32_t>())) return false;
    m_tick = j.value("tick", std::uint64_t{0});

    // nlohmann throws on structural mismatch; a truncated or hand-edited save
    // must not crash the server, so the replay is exception-bounded.
    try {
        loadSaveBody(j);
    } catch (const nlohmann::json::exception& e) {
        log::error("load: '{}' is structurally invalid ({}) — starting fresh", path, e.what());
        m_pendingRestore.reset();
    }
    log::info("loaded '{}' (seed {}, {} edited chunks)", path, m_seed,
              j.value("chunks", nlohmann::json::array()).size());
    return true;
}

void ServerSim::loadSaveBody(const nlohmann::json& j) {
    for (const auto& entry : j.value("chunks", nlohmann::json::array())) {
        const auto& p = entry["pos"];
        const ChunkPos cp{p[0].get<int>(), p[1].get<int>(), p[2].get<int>()};
        for (const auto& cell : entry["cells"]) {
            const auto index = cell[0].get<std::uint16_t>();
            const int x = index % kChunkSize, z = (index / kChunkSize) % kChunkSize,
                      y = index / (kChunkSize * kChunkSize);
            m_voxels.setBlock(glm::ivec3(cp.x * kChunkSize + x, cp.y * kChunkSize + y,
                                         cp.z * kChunkSize + z),
                              cell[1].get<BlockId>());
        }
    }

    const auto players = j.value("players", nlohmann::json::array());
    if (!players.empty()) { // restored state goes to the first player who joins
        const auto& entry = players[0];
        SavedPlayer restored;
        restored.pos = {entry["pos"][0].get<float>(), entry["pos"][1].get<float>(),
                        entry["pos"][2].get<float>()};
        restored.health = entry.value("health", 100.0f);
        const auto inv = entry.value("inventory", nlohmann::json::array());
        for (int i = 0; i < Inventory::kSlots && i < static_cast<int>(inv.size()); ++i)
            restored.inventory.slot(i) = {inv[i][0].get<ItemId>(), inv[i][1].get<std::uint16_t>()};
        m_pendingRestore = restored;
    }
}

void ServerSim::broadcastSnapshot(Transport& transport) {
    SnapshotMsg snap;
    snap.tick = m_tick;
    for (auto& [peer, player] : m_players) {
        if (!player->spawned) continue;
        snap.players.push_back({peer, player->controller.position(),
                                player->controller.velocity(), player->lastCmd.yaw,
                                player->lastCmd.pitch, player->controller.onGround(),
                                player->controller.crouched(), player->health});
    }
    // Threats first: if the entity cap ever bites, invisible-but-lethal NPCs and
    // rockets are far worse than an unrendered ammo box.
    for (const Npc& n : m_npcs) {
        if (snap.entities.size() >= kMaxSnapshotEntities) break;
        snap.entities.push_back({n.id, static_cast<std::uint8_t>(n.type), n.pos, n.yaw, 0,
                                 n.health, 0});
    }
    for (const Projectile& p : m_projectiles) {
        if (snap.entities.size() >= kMaxSnapshotEntities) break;
        snap.entities.push_back({p.id, static_cast<std::uint8_t>(EntityArchetype::Projectile),
                                 p.pos, 0, 0, 0.0f, 0});
    }
    for (const Deployable& d : m_deployables) {
        if (snap.entities.size() >= kMaxSnapshotEntities) break;
        snap.entities.push_back({d.id, static_cast<std::uint8_t>(EntityArchetype::Deployable),
                                 d.pos, 0, 0, 0.0f, 0});
    }
    for (const WorldEntity& e : m_entities) {
        if (snap.entities.size() >= kMaxSnapshotEntities) break;
        snap.entities.push_back({e.id, static_cast<std::uint8_t>(e.type), e.pos, e.yaw, 0,
                                 0.0f, e.item});
    }
    for (auto& [peer, player] : m_players) {
        snap.lastCmdTick = player->lastCmdTick; // per-recipient ack of their own input
        transport.send(peer, pack(snap), false);
    }
}

} // namespace meat
