#pragma once
#include "engine/net/Messages.h"
#include "engine/net/Transport.h"

#include <deque>
#include <string>
#include <unordered_map>

namespace meat {

class VoxelWorld;
class PhysicsWorld;
class CharacterController;

// Connection + prediction logic for one player. The Engine owns the client-side
// mirror (voxels/physics/character) and hands references in; this class owns
// only protocol state, so it works identically over loopback and UDP.
class Client {
public:
    void attach(Transport& transport, const std::string& playerName); // sends Hello
    bool welcomed() const { return m_welcomed; }
    PeerId playerId() const { return m_playerId; }
    std::uint32_t worldSeed() const { return m_seed; }

    // Record locally + send to server. Call exactly once per fixed tick.
    void sendCommand(const PlayerCommand& cmd);

    // Drain net events: Welcome, VoxelOps into the mirror, Snapshots into
    // rewind-and-replay reconciliation of the local character.
    void pump(VoxelWorld& voxels, PhysicsWorld& physics, CharacterController& player);

    // Newest known state per remote player (interpolation buffers come with
    // visible player meshes; until then newest-state is enough).
    const std::unordered_map<PeerId, PlayerState>& remotePlayers() const { return m_remotes; }

private:
    void applySnapshot(const SnapshotMsg& snap, PhysicsWorld& physics,
                       CharacterController& player);

    Transport* m_transport = nullptr;
    std::string m_playerName;
    PeerId m_playerId = 0;
    std::uint32_t m_seed = 0;
    bool m_welcomed = false;
    std::uint64_t m_latestSnapshotTick = 0;
    std::deque<PlayerCommand> m_unacked; // commands newer than the server's ack
    std::unordered_map<PeerId, PlayerState> m_remotes;
};

} // namespace meat
