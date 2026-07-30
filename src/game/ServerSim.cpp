#include "game/ServerSim.h"
#include "engine/core/Log.h"
#include "engine/core/ViewMath.h"

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

constexpr float kFireInterval = 0.15f;
constexpr float kPlaceInterval = 0.20f;
constexpr float kHitscanRange = 60.0f;
constexpr float kHitDamage = 25.0f;
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
    return true;
}

void ServerSim::pump(Transport& transport) {
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
        log::info("server: '{}' joined as player {}", hello.name, peer);
        break;
    }
    case MsgType::Command: {
        CommandMsg msg;
        if (!decode(msg, reader)) return;
        if (msg.cmd.tick <= player.lastCmdTick && player.lastCmdTick != 0) return; // stale
        player.lastCmd = msg.cmd;
        player.lastCmdTick = msg.cmd.tick;
        break;
    }
    case MsgType::VoxelOp: {
        VoxelOpMsg op;
        if (!decode(op, reader)) return;
        applyVoxelOp(transport, op); // client intent; server is the only writer
        break;
    }
    default:
        break;
    }
}

void ServerSim::tick(Transport& transport) {
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
    m_voxels.update(streamCenter, m_jobs);

    ++m_tick;
    if (m_tick % kSnapshotEvery == 0 && !m_players.empty()) broadcastSnapshot(transport);
}

void ServerSim::applyVoxelOp(Transport& transport, const VoxelOpMsg& op) {
    m_voxels.setBlock(op.voxel, op.block);
    for (auto& [peer, unused] : m_players) transport.send(peer, pack(op), true);
}

void ServerSim::giveStartingLoadout(Player& player) {
    player.inventory.add(m_defaultItems.pistol, 1, m_items);
    player.inventory.add(m_defaultItems.ammo9mm, 60, m_items);
    player.inventory.add(m_defaultItems.medkit, 2, m_items);
    player.inventory.add(m_defaultItems.stoneBlock, 32, m_items);
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
            if (hasAmmo) {
                player.fireCooldown = heldDef.fireInterval;
                if (m_rules.finiteAmmo && heldDef.ammoItem != 0) {
                    player.inventory.remove(heldDef.ammoItem, 1);
                    player.inventoryDirty = true;
                }

                const auto voxelHit = m_voxels.raycast(eye, dir, kHitscanRange);
                const float voxelT = voxelHit ? voxelHit->t : kHitscanRange;

                // Closest player capsule in front of the voxel hit wins.
                Player* victim = nullptr;
                PeerId victimPeer = 0;
                float bestT = voxelT;
                for (auto& [otherPeer, other] : m_players) {
                    if (otherPeer == peer || !other->spawned) continue;
                    const glm::vec3 feet = other->controller.position();
                    const float height = other->controller.crouched() ? 0.95f : 1.8f;
                    const glm::vec3 a = feet + glm::vec3(0, kCapsuleRadius, 0);
                    const glm::vec3 b = feet + glm::vec3(0, height - kCapsuleRadius, 0);
                    float tRay = 0;
                    if (raySegmentDistance(eye, dir, kHitscanRange, a, b, tRay) <=
                            kCapsuleRadius &&
                        tRay < bestT) {
                        bestT = tRay;
                        victim = other.get();
                        victimPeer = otherPeer;
                    }
                }

                if (victim) {
                    victim->health -= heldDef.damage;
                    if (victim->health <= 0.0f) {
                        log::info("server: player {} fragged player {}", peer, victimPeer);
                        victim->controller.setState(kSpawnPos, glm::vec3(0));
                        victim->health = 100.0f;
                    }
                } else if (voxelHit && voxelHit->block != 0) {
                    applyVoxelOp(transport, {voxelHit->voxel, 0});
                    if (m_rules.minedBlockDrops) {
                        player.inventory.add(m_defaultItems.stoneBlock, 1, m_items);
                        player.inventoryDirty = true;
                    }
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

    if (player.lastCmd.use && player.useCooldown <= 0.0f &&
        heldDef.type == ItemType::Consumable && player.health < 100.0f) {
        player.useCooldown = 0.5f;
        player.inventory.remove(held.id, 1);
        player.health = glm::min(100.0f, player.health + 50.0f);
        player.inventoryDirty = true;
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
    log::info("loaded '{}' (seed {}, {} edited chunks)", path, m_seed,
              j.value("chunks", nlohmann::json::array()).size());
    return true;
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
    for (auto& [peer, player] : m_players) {
        snap.lastCmdTick = player->lastCmdTick; // per-recipient ack of their own input
        transport.send(peer, pack(snap), false);
    }
}

} // namespace meat
