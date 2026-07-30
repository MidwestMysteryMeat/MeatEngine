#include "game/ServerSim.h"
#include "engine/core/Log.h"

#include <thread>

namespace meat {
namespace {
constexpr float kFixedDtServer = 1.0f / 60.0f;
constexpr int kSnapshotEvery = 3; // 60 Hz sim → 20 Hz snapshots
constexpr glm::vec3 kSpawnPos{8.0f, 8.0f, 8.0f};
} // namespace

bool ServerSim::init(std::uint32_t worldSeed) {
    m_seed = worldSeed;
    if (!m_physics.init()) return false;
    m_jobs.start(std::thread::hardware_concurrency());

    m_palette = registerDefaultBlocks(m_voxels.blockRegistry());
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
        WelcomeMsg welcome{peer, m_seed, m_tick};
        transport.send(peer, pack(welcome), true);
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
        // Server validates (registry knows the id) then applies + broadcasts;
        // clients only ever apply ops echoed by the server.
        m_voxels.setBlock(op.voxel, op.block);
        for (auto& [otherPeer, unused] : m_players) transport.send(otherPeer, pack(op), true);
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
            if (!player->controller.init(m_physics, kSpawnPos)) continue;
            player->spawned = true;
        }
        player->controller.update(player->lastCmd, kFixedDtServer, m_physics);
        if (player->controller.position().y < -30.0f) // fell out (colliders pending)
            player->controller.setState(kSpawnPos, glm::vec3(0));
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

void ServerSim::broadcastSnapshot(Transport& transport) {
    SnapshotMsg snap;
    snap.tick = m_tick;
    for (auto& [peer, player] : m_players) {
        if (!player->spawned) continue;
        snap.players.push_back({peer, player->controller.position(),
                                player->controller.velocity(), player->lastCmd.yaw,
                                player->lastCmd.pitch, player->controller.onGround(),
                                player->controller.crouched()});
    }
    for (auto& [peer, player] : m_players) {
        snap.lastCmdTick = player->lastCmdTick; // per-recipient ack of their own input
        transport.send(peer, pack(snap), false);
    }
}

} // namespace meat
