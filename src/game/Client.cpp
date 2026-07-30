#include "game/Client.h"

#include "engine/core/Log.h"
#include "engine/core/TickRate.h"
#include "engine/physics/CharacterController.h"
#include "engine/physics/PhysicsWorld.h"
#include "engine/voxel/VoxelWorld.h"

namespace meat {
namespace {
constexpr std::size_t kMaxUnacked = 120; // 2 s of input; beyond this we're desynced anyway
} // namespace

void Client::attach(Transport& transport, const std::string& playerName) {
    m_transport = &transport;
    m_playerName = playerName;
    // Hello is sent on the Connected event in pump() — sending here would race
    // the UDP handshake and vanish (loopback is born connected and masks that).
}

void Client::sendCommand(const PlayerCommand& cmd) {
    if (!m_transport) return;
    m_unacked.push_back(cmd);
    while (m_unacked.size() > kMaxUnacked) m_unacked.pop_front();
    m_transport->send(1, pack(CommandMsg{cmd}), false);
}

void Client::pump(VoxelWorld& voxels, PhysicsWorld& physics, CharacterController& player) {
    if (!m_transport) return;
    std::vector<NetEvent> events;
    m_transport->poll(events);
    for (NetEvent& e : events) {
        if (e.type == NetEvent::Type::Connected) {
            m_transport->send(1, pack(HelloMsg{m_playerName}), true); // server = peer 1
            continue;
        }
        if (e.type != NetEvent::Type::Packet) continue;
        const auto type = peekType(e.data);
        if (!type) continue;
        ByteReader reader(std::span<const std::byte>(e.data).subspan(1));

        switch (*type) {
        case MsgType::Welcome: {
            WelcomeMsg msg;
            if (!decode(msg, reader)) break;
            m_playerId = msg.playerId;
            m_seed = msg.worldSeed;
            m_welcomed = true;
            log::info("client: welcomed as player {} (seed {})", msg.playerId, msg.worldSeed);
            break;
        }
        case MsgType::Snapshot: {
            SnapshotMsg snap;
            if (!decode(snap, reader)) break;
            if (snap.tick <= m_latestSnapshotTick) break; // unreliable channel: drop stale
            m_latestSnapshotTick = snap.tick;
            // Ack: everything the server has already applied leaves the buffer.
            while (!m_unacked.empty() && m_unacked.front().tick <= snap.lastCmdTick)
                m_unacked.pop_front();
            applySnapshot(snap, physics, player);
            break;
        }
        case MsgType::VoxelOp: {
            VoxelOpMsg op;
            if (!decode(op, reader)) break;
            voxels.setBlock(op.voxel, op.block); // server-echoed; mirror applies verbatim
            break;
        }
        default:
            break;
        }
    }
}

void Client::applySnapshot(const SnapshotMsg& snap, PhysicsWorld& physics,
                           CharacterController& player) {
    m_remotes.clear();
    const PlayerState* own = nullptr;
    for (const PlayerState& state : snap.players) {
        if (state.playerId == m_playerId)
            own = &state;
        else
            m_remotes.emplace(state.playerId, state);
    }
    if (!own) return;

    // Rewind-and-replay: adopt the authoritative state, then re-apply every
    // command the server hasn't seen yet. When prediction was right this lands
    // exactly where we already were, so no correction is visible.
    player.setState(own->pos, own->vel);
    for (const PlayerCommand& cmd : m_unacked) player.update(cmd, kFixedDt, physics);
}

} // namespace meat
