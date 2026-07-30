#include "game/Client.h"

#include "engine/core/Log.h"
#include "engine/core/TickRate.h"
#include "engine/physics/CharacterController.h"
#include "engine/physics/PhysicsWorld.h"
#include "engine/voxel/VoxelWorld.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>

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

void Client::sendVoxelOp(glm::ivec3 voxel, std::uint16_t block) {
    if (m_transport) m_transport->send(1, pack(VoxelOpMsg{voxel, block}), true);
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
            m_rules.inventoryModel = static_cast<GameRules::InventoryModel>(msg.rulesModel);
            m_rules.setFlagsByte(msg.rulesFlags);
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
        case MsgType::Inventory: {
            Inventory incoming;
            if (incoming.decode(reader)) m_inventory = incoming;
            break;
        }
        case MsgType::VoxelOp: {
            VoxelOpMsg op;
            if (!decode(op, reader)) break;
            voxels.setBlock(op.voxel, op.block); // server-echoed; mirror applies verbatim
            break;
        }
        case MsgType::BatchVoxelOp: { // explosion crater: many voxels → air, one packet
            std::uint32_t count = 0;
            if (!reader.read(count) || count > 200000) break;
            for (std::uint32_t i = 0; i < count; ++i) {
                glm::ivec3 v;
                if (!reader.read(v)) break;
                voxels.setBlock(v, 0);
            }
            break;
        }
        default:
            break;
        }
    }
}

void Client::applySnapshot(const SnapshotMsg& snap, PhysicsWorld& physics,
                           CharacterController& player) {
    const PlayerState* own = nullptr;
    for (const PlayerState& state : snap.players) {
        if (state.playerId == m_playerId) {
            own = &state;
            continue;
        }
        auto& history = m_remotes[state.playerId].states;
        history.emplace_back(snap.tick, state);
        while (history.size() > 32) history.pop_front(); // ~1.6 s at 20 Hz
    }
    // Full-state snapshots: anyone absent has disconnected.
    std::erase_if(m_remotes, [&](const auto& entry) {
        return std::none_of(snap.players.begin(), snap.players.end(),
                            [&](const PlayerState& s) { return s.playerId == entry.first; });
    });
    m_entities = snap.entities;
    if (!own) return;
    m_ownHealth = own->health;

    // Rewind-and-replay: adopt the authoritative state, then re-apply every
    // command the server hasn't seen yet. When prediction was right this lands
    // exactly where we already were, so no correction is visible.
    player.setState(own->pos, own->vel);
    for (const PlayerCommand& cmd : m_unacked) player.update(cmd, kFixedDt, physics);
}

std::vector<PlayerState> Client::remoteViewStates() const {
    std::vector<PlayerState> out;
    if (m_latestSnapshotTick < 6) return out;
    const auto targetTick = static_cast<double>(m_latestSnapshotTick - 6); // 100 ms behind

    for (const auto& [peerId, history] : m_remotes) {
        const auto& states = history.states;
        if (states.empty()) continue;
        // Find the bracketing pair around targetTick.
        const auto after = std::find_if(states.begin(), states.end(), [&](const auto& e) {
            return static_cast<double>(e.first) >= targetTick;
        });
        if (after == states.begin()) {
            out.push_back(states.front().second);
            continue;
        }
        if (after == states.end()) {
            out.push_back(states.back().second); // starved buffer: hold last known
            continue;
        }
        const auto& [tickB, stateB] = *after;
        const auto& [tickA, stateA] = *std::prev(after);
        const float t = tickB > tickA
                            ? static_cast<float>((targetTick - static_cast<double>(tickA)) /
                                                 static_cast<double>(tickB - tickA))
                            : 1.0f;
        PlayerState blended = stateB;
        blended.pos = glm::mix(stateA.pos, stateB.pos, t);
        // Shortest-arc yaw blend so crossing ±π doesn't spin the character.
        float dyaw = stateB.yaw - stateA.yaw;
        while (dyaw > glm::pi<float>()) dyaw -= glm::two_pi<float>();
        while (dyaw < -glm::pi<float>()) dyaw += glm::two_pi<float>();
        blended.yaw = stateA.yaw + dyaw * t;
        out.push_back(blended);
    }
    return out;
}

} // namespace meat
