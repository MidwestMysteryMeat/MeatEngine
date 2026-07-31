#pragma once
#include "engine/net/Messages.h"
#include "engine/net/Transport.h"
#include "game/GameRules.h"
#include "game/Inventory.h"

#include <deque>
#include <map>
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

    // Voxel edit intent (editor brushes, scripted edits). Server validates,
    // applies, and echoes to everyone — including us.
    void sendVoxelOp(glm::ivec3 voxel, std::uint16_t block);

    // Prop placement intent (editor). Server assigns an id, adds a collider, and
    // echoes PropAddedMsg to everyone (surfaced via takeNewProps below).
    void sendPlaceProp(const std::string& asset, const glm::mat4& transform);
    // Prop transform update (editor gizmo). Server validates the id, rebuilds the
    // collider, and rebroadcasts PropAdded with the new transform.
    void sendMoveProp(std::uint32_t id, const glm::mat4& transform);
    // Prop deletion intent (editor outliner Delete). Server removes + broadcasts.
    void sendRemoveProp(std::uint32_t id);

    // Drain props the server reported since the last call. The Engine applies
    // them (render mesh + client-mirror collider); the Client stays protocol-only.
    // PropAdded with an already-known id means "transform updated" (move).
    std::vector<PropAddedMsg> takeNewProps();
    std::vector<std::uint32_t> takeRemovedProps();

    // Drain net events: Welcome, VoxelOps into the mirror, Snapshots into
    // rewind-and-replay reconciliation of the local character.
    void pump(VoxelWorld& voxels, PhysicsWorld& physics, CharacterController& player);

    float health() const { return m_ownHealth; }
    const GameRules& rules() const { return m_rules; }
    const Inventory& inventory() const { return m_inventory; } // UI mirror; server owns truth

    // Remote players sampled 100 ms behind the newest snapshot, interpolated
    // between the bracketing snapshot states — smooth despite 20 Hz updates.
    std::vector<PlayerState> remoteViewStates() const;

    // World entities, newest snapshot state (pickups are static; movers get
    // interpolation when projectiles/NPCs land).
    const std::vector<EntityState>& entities() const { return m_entities; }

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
    std::vector<EntityState> m_entities;
    // Props reported by the server this frame, drained by the Engine each loop.
    std::vector<PropAddedMsg> m_newProps;
    std::vector<std::uint32_t> m_removedProps;
    // Newest snapshot tick we hold; piggybacked to the server as the delta-baseline
    // ack on every CommandMsg. 0 until the first snapshot lands (server keyframes).
    std::uint64_t m_ackTick = 0;
    // Ring of reconstructed snapshots (tick -> full state) usable as delta
    // baselines. Symmetric with the server's ring; 32 deep (~1.6 s at 20 Hz).
    std::map<std::uint64_t, SnapshotMsg> m_snapRing;
};

} // namespace meat
