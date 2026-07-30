#pragma once
#include "engine/net/Messages.h"
#include "engine/net/Transport.h"
#include "game/GameRules.h"
#include "game/Inventory.h"

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

    float health() const { return m_ownHealth; }
    const GameRules& rules() const { return m_rules; }
    const Inventory& inventory() const { return m_inventory; } // UI mirror; server owns truth

    // Remote players sampled 100 ms behind the newest snapshot, interpolated
    // between the bracketing snapshot states — smooth despite 20 Hz updates.
    std::vector<PlayerState> remoteViewStates() const;

private:
    void applySnapshot(const SnapshotMsg& snap, PhysicsWorld& physics,
                       CharacterController& player);

    struct RemoteHistory {
        std::deque<std::pair<std::uint64_t, PlayerState>> states; // tick-ascending
    };

    Transport* m_transport = nullptr;
    std::string m_playerName;
    PeerId m_playerId = 0;
    std::uint32_t m_seed = 0;
    bool m_welcomed = false;
    float m_ownHealth = 100.0f;
    GameRules m_rules;
    Inventory m_inventory;
    std::uint64_t m_latestSnapshotTick = 0;
    std::deque<PlayerCommand> m_unacked; // commands newer than the server's ack
    std::unordered_map<PeerId, RemoteHistory> m_remotes;
};

} // namespace meat
