#include "game/Client.h"

#include "engine/core/Log.h"
#include "engine/core/TickRate.h"
#include "engine/net/DeltaSnapshot.h"
#include "engine/physics/CharacterController.h"
#include "engine/physics/PhysicsWorld.h"
#include "engine/voxel/VoxelWorld.h"
#include "game/EntityTypes.h"
#include "game/ShipControl.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <utility>

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
    m_transport->send(1, pack(CommandMsg{cmd, m_ackTick}), false); // piggyback snapshot ack
}

void Client::sendVoxelOp(glm::ivec3 voxel, std::uint16_t block) {
    if (m_transport) m_transport->send(1, pack(VoxelOpMsg{voxel, block}), true);
}

void Client::sendPlaceProp(const std::string& asset, const glm::mat4& transform) {
    if (m_transport) m_transport->send(1, pack(PlacePropMsg{asset, transform}), true);
}

void Client::sendMoveProp(std::uint32_t id, const glm::mat4& transform) {
    if (m_transport) m_transport->send(1, pack(MovePropMsg{id, transform}), true);
}

void Client::sendRemoveProp(std::uint32_t id) {
    if (m_transport) m_transport->send(1, pack(RemovePropMsg{id}), true);
}

std::vector<PropAddedMsg> Client::takeNewProps() { return std::exchange(m_newProps, {}); }

std::vector<std::uint32_t> Client::takeRemovedProps() { return std::exchange(m_removedProps, {}); }

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
            // Host-authoritative world scale + environment (terrain already in flags).
            // Clamped to the same band applyVoxelSize uses so a hostile Welcome can't
            // blow meshing cost or invert coordinates.
            const float vs = msg.voxelSize < 0.1f ? 0.1f : msg.voxelSize > 8.0f ? 8.0f : msg.voxelSize;
            m_rules.voxelSize = vs;
            m_rules.environment = static_cast<GameRules::Environment>(msg.environment > 2 ? 0 : msg.environment);
            m_welcomed = true;
            log::info("client: welcomed as player {} (seed {}, voxelSize {:.3f} m, env {})",
                      msg.playerId, msg.worldSeed, m_rules.voxelSize,
                      static_cast<int>(m_rules.environment));
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
        case MsgType::DeltaSnapshot: {
            // Peek the baseline tick (non-consuming copy), pick the matching
            // snapshot out of our ring, reconstruct the FULL snapshot, then hand
            // it to the unchanged applySnapshot path.
            const auto baseTick = peekDeltaBaseline(reader);
            if (!baseTick) break;
            static const SnapshotMsg kEmpty{};
            const SnapshotMsg* base = &kEmpty;
            if (*baseTick != 0) {
                auto it = m_snapRing.find(*baseTick);
                if (it == m_snapRing.end()) break; // baseline lost: wait for a keyframe
                base = &it->second;
            }
            SnapshotMsg snap;
            if (!decodeDelta(snap, *base, reader)) break;
            if (snap.tick <= m_latestSnapshotTick) break; // unreliable channel: drop stale
            m_latestSnapshotTick = snap.tick;
            m_ackTick = snap.tick;                        // piggybacked on the next CommandMsg
            m_snapRing[snap.tick] = snap;                 // keep last 32 as baselines
            while (m_snapRing.size() > 32) m_snapRing.erase(m_snapRing.begin());
            while (!m_unacked.empty() && m_unacked.front().tick <= snap.lastCmdTick)
                m_unacked.pop_front();
            applySnapshot(snap, physics, player); // UNCHANGED reconciliation path
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
        case MsgType::PropAdded: {
            PropAddedMsg msg;
            if (decode(msg, reader)) m_newProps.push_back(std::move(msg));
            break;
        }
        case MsgType::RemoveProp: {
            RemovePropMsg msg;
            if (decode(msg, reader)) m_removedProps.push_back(msg.id);
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
    m_vehicleId = own->vehicleId;
    m_vehicleRole = own->vehicleRole;
    m_ownPos = own->pos;
    m_ownYaw = own->yaw;
    m_ownPitch = own->pitch;

    // Rewind-and-replay: adopt the authoritative state, then re-apply every
    // command the server hasn't seen yet. When prediction was right this lands
    // exactly where we already were, so no correction is visible.
    player.setState(own->pos, own->vel);
    if (m_vehicleId != 0 && m_vehicleRole == 1) {
        // Pilot only: predict thrusters (ship) or ground car (racer). Passengers skip.
        ShipPose pose{own->pos, own->vel, own->yaw, own->pitch};
        for (const EntityState& e : m_entities) {
            if (e.id == m_vehicleId &&
                e.archetype == static_cast<std::uint8_t>(EntityArchetype::Ship)) {
                pose.pitch = unpackShipPitch(e.data);
                pose.yaw = e.yaw;
                pose.pos = e.pos;
                break;
            }
        }
        const bool groundVehicle = m_rules.gameTemplate == GameRules::Template::Racer;
        for (const PlayerCommand& cmd : m_unacked) {
            if (groundVehicle)
                integrateRacer(pose, cmd, kFixedDt, glm::vec3(0.0f, -18.0f, 0.0f));
            else
                integrateShip(pose, cmd, kFixedDt, glm::vec3(0.0f, -1.5f, 0.0f));
        }
        player.setState(pose.pos, pose.vel);
        m_ownPos = pose.pos;
        m_ownYaw = pose.yaw;
        m_ownPitch = pose.pitch;
    } else if (m_vehicleId != 0) {
        // Passenger: hold snapshot seat pose (server glues each tick).
        m_ownPos = own->pos;
        m_ownYaw = own->yaw;
        m_ownPitch = own->pitch;
    } else {
        for (const PlayerCommand& cmd : m_unacked) player.update(cmd, kFixedDt, physics);
    }
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
