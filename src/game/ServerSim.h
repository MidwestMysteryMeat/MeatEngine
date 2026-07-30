#pragma once
#include "engine/core/JobQueue.h"
#include "engine/net/Messages.h"
#include "engine/net/Transport.h"
#include "engine/script/ScriptHost.h"
#include "engine/physics/CharacterController.h"
#include "engine/physics/PhysicsWorld.h"
#include "engine/voxel/VoxelWorld.h"
#include "game/EntityTypes.h"
#include "game/GameRules.h"
#include "game/Inventory.h"
#include "game/WorldGen.h"

#include <nlohmann/json_fwd.hpp>

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

    // Where gameplay scripts load from. Default is the engine's built-in assets;
    // a game project points this at its own scripts dir (see --project).
    void setScriptDir(std::string dir) { m_scriptDir = std::move(dir); }

    bool init(std::uint32_t worldSeed);
    bool initFromSave(const std::string& path); // reads seed, then init + replay
    bool saveTo(const std::string& path) const;
    void pump(Transport& transport);  // drain net events, queue commands
    void tick(Transport& transport);  // one 60 Hz step; snapshots every 3rd tick

    bool reloadScripts() { return m_scripts.reload(); } // live editing (host/SP)
    std::uint32_t seed() const { return m_seed; }
    std::uint64_t currentTick() const { return m_tick; }
    const GameRules& rules() const { return m_rules; }
    int playerCount() const { return static_cast<int>(m_players.size()); }

private:
    struct Player {
        CharacterController controller;
        PlayerCommand lastCmd{};
        std::uint64_t lastCmdTick = 0;
        bool hasCmd = false; // false until the first command; closes tick-0 replay
        bool spawned = false;
        bool helloDone = false; // guards Welcome/loadout against Hello replay
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

    struct Projectile {
        std::uint32_t id = 0;
        PeerId owner = 0;
        glm::vec3 pos{0}, vel{0};
        float gravity = 0.0f;
        float radius = 0.0f, damage = 0.0f; // blast on impact
        float life = 6.0f;                  // seconds before self-detonate
        float ownerGrace = 0.12f;           // owner-immune while clearing the muzzle
    };
    struct Deployable {
        std::uint32_t id = 0;
        PeerId owner = 0;
        glm::vec3 pos{0};
        float radius = 0.0f, damage = 0.0f;
        float armTime = 1.0f; // won't trigger on its own owner while arming
        float triggerRange = 2.2f;
    };
    // A placed auto-turret: targets the nearest hostile NPC in range + line of
    // sight and fires hitscan on a cadence. Owned by a player, has health.
    struct Turret {
        std::uint32_t id = 0;
        PeerId owner = 0;
        glm::vec3 pos{0};
        float yaw = 0.0f;
        float health = 120.0f;
        float fireCooldown = 0.0f;
    };
    struct Npc {
        std::uint32_t id = 0;
        EntityArchetype type = EntityArchetype::NpcChaser;
        glm::vec3 pos{0};
        float yaw = 0.0f;
        float health = 60.0f;
        PeerId target = 0;          // aggroed player
        std::vector<glm::ivec3> path;
        std::size_t pathIndex = 0;
        float repathTimer = 0.0f;
        float attackCooldown = 0.0f;
    };
    struct Companion {                // mobile ally: follows owner, shoots hostile NPCs
        std::uint32_t id = 0;
        PeerId owner = 0;
        glm::vec3 pos{0};
        float yaw = 0.0f;
        float health = 150.0f;
        float fireCooldown = 0.0f;
        std::vector<glm::ivec3> path; // reuses the NPC A* pathing toward owner/target
        std::size_t pathIndex = 0;
        float repathTimer = 0.0f;
    };

    void handlePacket(Transport& transport, PeerId peer, std::span<const std::byte> data);
    void broadcastSnapshot(Transport& transport);
    void applyVoxelOp(Transport& transport, const VoxelOpMsg& op);
    void processCombat(Transport& transport, PeerId peer, Player& player);
    void sendInventory(Transport& transport, PeerId peer, const Player& player) const;
    void sendOverlayTo(Transport& transport, PeerId peer) const; // replay world edits
    void giveStartingLoadout(Player& player);

    void spawnDungeonLoot();
    void spawnDungeonNpcs();
    void updateNpcs(Transport& transport);
    void updateTurrets(Transport& transport);
    void updateCompanions(Transport& transport);
    void damageNpc(Transport& transport, Npc& npc, float damage); // death → loot drop
    void loadSaveBody(const nlohmann::json& j); // may throw; initFromSave bounds it
    bool tryPickup(Transport& transport, PeerId peer, Player& player); // true if grabbed
    void fireHitscan(Transport& transport, PeerId peer, Player& player,
                     const ItemDef& weapon);
    void marchBullet(Transport& transport, PeerId peer, Player& player,
                     const ItemDef& weapon, glm::vec3 dir);
    void spawnProjectile(PeerId owner, glm::vec3 pos, glm::vec3 vel, const ItemDef& weapon);
    void updateProjectiles(Transport& transport);
    void applyBlast(Transport& transport, PeerId source, glm::vec3 center, float radius,
                    float damage);

    struct IVec3Hash {
        std::size_t operator()(const glm::ivec3& v) const {
            std::size_t h = static_cast<std::size_t>(v.x) * 73856093u;
            h ^= static_cast<std::size_t>(v.y) * 19349663u;
            h ^= static_cast<std::size_t>(v.z) * 83492791u;
            return h;
        }
    };

    JobQueue m_jobs;         // server-side meshing feeds colliders only
    PhysicsWorld m_physics;
    VoxelWorld m_voxels;

    void setupScripting();

    std::vector<WorldEntity> m_entities;
    std::vector<Projectile> m_projectiles;
    std::vector<Deployable> m_deployables;
    std::vector<Npc> m_npcs;
    std::vector<Turret> m_turrets;
    std::vector<Companion> m_companions;
    ScriptHost m_scripts;
    Transport* m_activeTransport = nullptr; // set each pump/tick for script callbacks
    std::uint64_t m_scriptRng = 0x2545F4914F6CDD1Dull; // seeded in init()
    std::string m_scriptDir = "assets/scripts";
    std::uint32_t m_nextEntityId = 1;
    // Sparse chip-damage: only voxels that have been shot, remaining hp. Entries
    // die with the block; pristine blocks are implicit full-hp.
    std::unordered_map<glm::ivec3, float, IVec3Hash> m_voxelDamage;
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
