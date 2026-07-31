#pragma once
#include "engine/core/JobQueue.h"
#include "engine/net/Messages.h"
#include "engine/net/Transport.h"
#include "engine/script/ScriptHost.h"
#include "engine/physics/CharacterController.h"
#include "engine/physics/GravityField.h"
#include "engine/physics/PhysicsWorld.h"
#include "engine/voxel/VoxelWorld.h"
#include "game/Effects.h"
#include "game/EntityTypes.h"
#include "game/GameRules.h"
#include "game/Inventory.h"
#include "game/NavMesh.h"
#include "game/ShipControl.h"
#include "game/WorldGen.h"

#include <nlohmann/json_fwd.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
    // B3b: authoritative gravity field (env base + volumes + orbital bodies).
    const GravityField& gravityField() const { return m_gravity; }

    // B4 New Map: clear voxels/props/entities, switch terrain+environment+seed, and
    // regenerate dungeon content. Connected players are respawned at the default pad.
    // Does not tear down the transport or peer map — Hello stays valid.
    void reseedWorld(std::uint32_t seed, GameRules::Terrain terrain,
                     GameRules::Environment environment,
                     GameRules::Template gameTemplate = GameRules::Template::Fps);

private:
    struct Player {
        CharacterController controller;
        PlayerCommand lastCmd{};
        std::uint64_t lastCmdTick = 0;
        // Newest snapshot tick this client has acked (piggybacked on CommandMsg).
        // Monotonic. Selects the delta baseline in broadcastSnapshot; 0 => keyframe.
        std::uint64_t ackedSnapshotTick = 0;
        bool hasCmd = false; // false until the first command; closes tick-0 replay
        bool spawned = false;
        bool helloDone = false; // guards Welcome/loadout against Hello replay
        float health = 100.0f;
        float fireCooldown = 0.0f;
        float placeCooldown = 0.0f;
        float useCooldown = 0.0f;
        // H4: ship entity id while piloting (0 = on foot). Character controller is
        // frozen; thrusters drive the ship instead.
        std::uint32_t pilotingShip = 0;
        // H4: alternate twin hardpoints while piloting (0 = left, 1 = right).
        int shipHardpoint = 0;
        // Fire-mode trigger discipline (H2): the previous tick's button states let
        // the combat step detect a PRESS EDGE, so SemiAuto/Burst fire once per pull
        // and can't auto-repeat on hold.
        bool prevFire = false;
        bool prevReload = false;
        bool prevUse = false;
        int burstRemaining = 0;        // rounds left in an in-flight burst
        // Reload (H3): a mag reload runs on a timer, then pulls reserve into the mag.
        float reloadCooldown = 0.0f;   // seconds left on the active reload
        ItemId reloadingWeapon = 0;    // weapon whose reload is in progress (0 = none)
        Inventory inventory;
        bool inventoryDirty = false;
        std::optional<glm::vec3> spawnOverride; // from a save file
        // Active timed modifiers (ApplyModifier effects). Ticked down each fixed
        // tick; the product of damageMult scales this player's outgoing damage.
        // Small (kits stack a handful) — a flat vector, no per-tick allocation.
        struct ActiveModifier {
            float damageMult = 1.0f;
            float speedMult = 1.0f; // stored; enforcement is a follow-up (engine-owned)
            float remaining = 0.0f; // seconds left
        };
        std::vector<ActiveModifier> modifiers;
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
        EffectList onImpact; // composed detonation effects (copied from the weapon)
    };
    struct Deployable {
        std::uint32_t id = 0;
        PeerId owner = 0;
        glm::vec3 pos{0};
        float radius = 0.0f, damage = 0.0f;
        float armTime = 1.0f; // won't trigger on its own owner while arming
        float triggerRange = 2.2f;
        EffectList onTrigger; // composed detonation effects (copied from the weapon)
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
    // A server-authoritative mesh prop placed from the editor. Owns a static box
    // collider (players collide with it), is broadcast to every client, replayed
    // to joiners (sendOverlayTo), and persisted in the world save.
    struct WorldProp {
        std::uint32_t id = 0;
        std::string asset;
        glm::mat4 transform{1.0f};
        PhysicsWorld::BodyHandle body = PhysicsWorld::kInvalidBody;
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
        float animSpeed = 0.0f;     // authoritative walk weight (0=idle..1=full walk); sent to
                                    // clients as EntityState.anim so they don't guess from
                                    // interpolated positions (which read ~0 and froze the blend).
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
        float animSpeed = 0.0f;       // see Npc::animSpeed
    };
    // H4: thruster vehicle + kinematic Jolt hull (disabled while piloted so the
    // seat capsule doesn't explode out of the box).
    struct Ship {
        std::uint32_t id = 0;
        glm::vec3 pos{0};
        glm::vec3 vel{0};
        float yaw = 0.0f;
        float pitch = 0.0f;
        PeerId pilot = 0; // 0 = empty seat
        float health = 500.0f;
        int hullVariant = 0; // ShipHulls catalog index
        glm::vec3 halfExtents{kShipHalfExtents};
        glm::vec3 seatOffset{kShipSeatOffset}; // local seat relative to hull center
        PhysicsWorld::BodyHandle body = PhysicsWorld::kInvalidBody;
    };

    void handlePacket(Transport& transport, PeerId peer, std::span<const std::byte> data);
    void broadcastSnapshot(Transport& transport);
    void applyVoxelOp(Transport& transport, const VoxelOpMsg& op);
    // Create a world prop: sizes a static box collider from the model bounds,
    // stores it, and (when transport != null) broadcasts PropAddedMsg to all
    // clients. id==0 mints a fresh id (live place); a non-zero id is reused (save
    // reload). Returns false if the model can't be loaded, in which case nothing
    // is added (a prop without a collider is never created).
    bool addProp(Transport* transport, const std::string& asset, const glm::mat4& transform,
                 std::uint32_t id);
    // Editor gizmo / delete intents. False if the id is unknown (stale client).
    bool moveProp(Transport* transport, std::uint32_t id, const glm::mat4& transform);
    bool removeProp(Transport* transport, std::uint32_t id);
    void broadcastPropAdded(Transport& transport, const WorldProp& prop) const;
    void broadcastPropRemoved(Transport& transport, std::uint32_t id) const;
    // Model bounds (post-center), cached by asset path. False if the model fails.
    bool propBounds(const std::string& asset, glm::vec3& outMin, glm::vec3& outMax) const;
    void processCombat(Transport& transport, PeerId peer, Player& player);
    void sendInventory(Transport& transport, PeerId peer, const Player& player) const;
    void sendOverlayTo(Transport& transport, PeerId peer) const; // replay world edits
    void giveStartingLoadout(Player& player);

    // Plan a voxel-cell path fromPos→toPos. Tries the optional Detour navmesh
    // (fromCell/toCell are the standable-snapped endpoints) and falls back to the
    // voxel A* on any miss, so NPC/companion pathing never regresses.
    std::vector<glm::ivec3> planPath(glm::vec3 fromPos, glm::vec3 toPos, glm::ivec3 fromCell,
                                     glm::ivec3 toCell);

    void spawnDungeonLoot();
    void spawnDungeonNpcs();
    void spawnDemoShip(); // H4: one+ ships near the spawn pad
    void spawnSpaceDecor(); // station + junkyard landmarks (Space template)
    void ensureShipBody(Ship& ship);   // create/update kinematic hull when empty
    void clearShipBody(Ship& ship);    // drop hull while piloted
    void updateShips();
    bool tryBoardOrLeaveShip(Player& player); // true if handled (Use consumed)
    // Aim origin for combat: eye on foot, twin hardpoints when piloting.
    glm::vec3 combatMuzzle(const Player& player) const;
    const Ship* findShip(std::uint32_t id) const;
    Ship* findShip(std::uint32_t id);
    void updateNpcs(Transport& transport);
    void updateTurrets(Transport& transport);
    void updateCompanions(Transport& transport);
    void damageNpc(Transport& transport, Npc& npc, float damage); // death → loot drop
    void spawnPickup(ItemId item, std::uint16_t count, glm::vec3 pos); // ItemPickup world entity
    void dropPlayerLoot(Player& player, glm::vec3 pos); // drop-on-death scatter (GameRules-gated)
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

    // Effect-composition core (GAS-lite). runEffects executes a whole list at a
    // target; applyEffect is the single-effect switch it dispatches to (also used
    // directly so a caller can run one stack Effect with no allocation). `source`
    // is the acting player (for outgoing-damage scaling / self-heal); targetPos is
    // used by AreaDamage, targetPlayer/targetNpc by the single-target kinds.
    void runEffects(Transport& transport, const EffectList& effects, PeerId source,
                    glm::vec3 targetPos, Player* targetPlayer, Npc* targetNpc);
    void applyEffect(Transport& transport, const Effect& effect, PeerId source,
                     glm::vec3 targetPos, Player* targetPlayer, Npc* targetNpc);
    void tickModifiers(Player& player, float dt); // decay active timed modifiers
    static float damageMultOf(const Player& player); // product of active damage mults

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
    NavMesh m_navmesh;       // optional Detour path provider (A* is the fallback)
    GravityField m_gravity;  // B3b: field sampled each player tick
    void rebuildGravityField();

    void setupScripting();

    std::vector<WorldEntity> m_entities;
    std::vector<WorldProp> m_props;
    std::uint32_t m_nextPropId = 1;
    // Model bounds cache so repeated props / reloads don't re-hit the disk loader.
    mutable std::unordered_map<std::string, std::pair<glm::vec3, glm::vec3>> m_propBoundsCache;
    std::vector<Projectile> m_projectiles;
    std::vector<Deployable> m_deployables;
    std::vector<Npc> m_npcs;
    std::vector<Turret> m_turrets;
    std::vector<Companion> m_companions;
    std::vector<Ship> m_ships;
    ScriptHost m_scripts;
    Transport* m_activeTransport = nullptr; // set each pump/tick for script callbacks
    std::uint64_t m_scriptRng = 0x2545F4914F6CDD1Dull; // seeded in init()
    std::string m_scriptDir = "assets/scripts";
    std::uint32_t m_nextEntityId = 1;
    // Sparse chip-damage: only voxels that have been shot, remaining hp. Entries
    // die with the block; pristine blocks are implicit full-hp.
    std::unordered_map<glm::ivec3, float, IVec3Hash> m_voxelDamage;
    std::unordered_map<PeerId, std::unique_ptr<Player>> m_players;
    // Ring of the last N emitted snapshots (tick -> full state), shared across
    // clients because there is no interest management yet. Each client's delta
    // baseline is ring[player->ackedSnapshotTick]. Ordered map so we can evict
    // the oldest (begin()) cheaply. N = 32 (~1.6 s at 20 Hz).
    std::map<std::uint64_t, SnapshotMsg> m_snapshotRing;
    BlockPalette m_palette;
    GameRules m_rules;
    ItemRegistry m_items;
    DefaultItems m_defaultItems;
    std::optional<SavedPlayer> m_pendingRestore; // applied to the next Hello
    std::uint32_t m_seed = 0;
    std::uint64_t m_tick = 0;
};

} // namespace meat
