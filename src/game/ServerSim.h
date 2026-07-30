#pragma once
#include "engine/core/JobQueue.h"
#include "engine/net/Messages.h"
#include "engine/net/Transport.h"
#include "engine/physics/CharacterController.h"
#include "engine/physics/PhysicsWorld.h"
#include "engine/voxel/VoxelWorld.h"
#include "game/GameRules.h"
#include "game/Inventory.h"
#include "game/WorldGen.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace meat {

// The authoritative simulation. Owns its own physics/voxel state — completely
// independent of any client's mirror, including the local player's. Never
// touches rendering or input.
class ServerSim {
public:
    explicit ServerSim(GameRules rules = {}) : m_rules(rules) {}

    bool init(std::uint32_t worldSeed);
    bool initFromSave(const std::string& path); // reads seed, then init + replay
    bool saveTo(const std::string& path) const;
    void pump(Transport& transport);  // drain net events, queue commands
    void tick(Transport& transport);  // one 60 Hz step; snapshots every 3rd tick

    std::uint32_t seed() const { return m_seed; }
    std::uint64_t currentTick() const { return m_tick; }
    const GameRules& rules() const { return m_rules; }

private:
    struct Player {
        CharacterController controller;
        PlayerCommand lastCmd{};
        std::uint64_t lastCmdTick = 0;
        bool spawned = false;
        float health = 100.0f;
        float fireCooldown = 0.0f;
        float placeCooldown = 0.0f;
        float useCooldown = 0.0f;
        Inventory inventory;
        bool inventoryDirty = false;
        std::optional<glm::vec3> spawnOverride; // from a save file
    };

    struct SavedPlayer {
        glm::vec3 pos{0};
        float health = 100.0f;
        Inventory inventory;
    };

    void handlePacket(Transport& transport, PeerId peer, std::span<const std::byte> data);
    void broadcastSnapshot(Transport& transport);
    void applyVoxelOp(Transport& transport, const VoxelOpMsg& op);
    void processCombat(Transport& transport, PeerId peer, Player& player);
    void sendInventory(Transport& transport, PeerId peer, const Player& player) const;
    void giveStartingLoadout(Player& player);

    JobQueue m_jobs;         // server-side meshing feeds colliders only
    PhysicsWorld m_physics;
    VoxelWorld m_voxels;
    std::unordered_map<PeerId, std::unique_ptr<Player>> m_players;
    BlockPalette m_palette;
    GameRules m_rules;
    ItemRegistry m_items;
    DefaultItems m_defaultItems;
    std::optional<SavedPlayer> m_pendingRestore; // applied to the next Hello
    std::uint32_t m_seed = 0;
    std::uint64_t m_tick = 0;
};

} // namespace meat
