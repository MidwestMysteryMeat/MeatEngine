#pragma once
#include "engine/core/JobQueue.h"
#include "engine/level/MeshLevel.h"
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
#include "game/PeerPermissions.h"
#include "game/ShipControl.h"
#include "game/WorldGen.h"

#include <nlohmann/json_fwd.hpp>

#include <algorithm>
#include <array>
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
    // Workers may still be meshing streamed chunks when the sim dies; stop them
    // before the members they feed (voxels/physics/navmesh) are torn down.
    ~ServerSim();

    // Where gameplay scripts load from. Default is the engine's built-in assets;
    // a game project points this at its own scripts dir (see --project).
    void setScriptDir(std::string dir) { m_scriptDir = std::move(dir); }
    // B2: optional static mesh level (authoritative triangle colliders). Call before init.
    void setMeshLevel(std::string assetPath, float scale = 1.0f);
    void setMeshLevelDesc(MeshLevelDesc desc);

    // Networking policy: who may author the world. Set before players connect.
    // Left at its default, no peer can edit anything — which is what a packaged
    // game and a dedicated server both want.
    void setNetPolicy(NetPolicy policy) { m_netPolicy = std::move(policy); }
    const NetPolicy& netPolicy() const { return m_netPolicy; }
    // The owner's own client is handed this in-process and echoes it in Hello.
    // Generated per boot in init(); never written to disk or logs.
    const std::string& editorToken() const { return m_netPolicy.editorToken; }

    bool init(std::uint32_t worldSeed);
    bool initFromSave(const std::string& path); // reads seed, then init + replay
    bool saveTo(const std::string& path) const;
    // Periodic autosave so a crash doesn't lose everything since the last manual
    // save. seconds <= 0 disables it (the default).
    void setAutosave(std::string path, float seconds) {
        m_autosavePath = std::move(path);
        m_autosaveIntervalTicks =
            seconds > 0.0f ? static_cast<std::uint64_t>(seconds * 60.0f) : 0; // 60 Hz tick
    }
    void pump(Transport& transport);  // drain net events, queue commands
    void tick(Transport& transport);  // one 60 Hz step; snapshots every 3rd tick

    bool reloadScripts() { return m_scripts.reload(); } // live editing (host/SP)
    std::uint32_t seed() const { return m_seed; }
    std::uint64_t currentTick() const { return m_tick; }
    // Read-only view of the authoritative world, for headless tests and
    // tooling. A test that asserts on the world itself cannot be fooled by a
    // permission check that reports "denied" while the block changed anyway.
    // (propCount() lives further down, where WorldProp is in scope.)
    const VoxelWorld& voxels() const { return m_voxels; }
    // True when no chunk-mesh/collider jobs are queued or running. Tests wait on
    // this to know the spawn-area colliders are built before checking grounding,
    // instead of guessing a wall-clock budget (which is unreliable under ASan).
    bool meshingIdle() const { return m_jobs.idle(); }

    // GameMode (Deathmatch) scoring. registerFrag credits a player-vs-player kill
    // and, once someone reaches fragLimit, ends the match. Public so the kill
    // paths and tests can drive it. Sandbox mode ignores scoring.
    void registerFrag(PeerId killer, PeerId victim);
    // Apply a damage-over-time (Ignite) to a player: dps for `seconds`, credited
    // to `source` on kill. Public so abilities/scripts (and tests) can drive it.
    void applyDamageOverTime(PeerId target, float dps, float seconds, PeerId source);
    // Chain (arc) damage: deal `damage` to up to `maxTargets` players, each within
    // `range` of the previous, starting from the one nearest `origin`. Kills credit
    // `source`. Public so abilities/scripts (and tests) can drive it.
    void applyChainDamage(PeerId source, glm::vec3 origin, float damage, int maxTargets,
                          float range);
    int fragsOf(PeerId p) const {
        const auto it = m_frags.find(p);
        return it == m_frags.end() ? 0 : it->second;
    }
    // Authoritative health of a player (0 if unknown). For scripts/HUD/tests.
    float playerHealth(PeerId p) const {
        const auto it = m_players.find(p);
        return (it == m_players.end() || !it->second) ? 0.0f : it->second->health;
    }
    bool matchOver() const { return m_matchOver; }
    PeerId matchWinner() const { return m_matchWinner; }
    const GameRules& rules() const { return m_rules; }
    int playerCount() const { return static_cast<int>(m_players.size()); }
    // B3b: authoritative gravity field (env base + volumes + orbital bodies).
    const GravityField& gravityField() const { return m_gravity; }
    // B3b-e: replace editor-authored gravity boxes and rebuild the field (host/SP).
    void setExtraGravityBoxes(std::vector<GravityBoxVolume> boxes);
    // B3b-net: push current extras to all peers (join replay also sends via sendOverlayTo).
    void broadcastGravityVolumes(Transport& transport) const;
    const std::vector<GravityBoxVolume>& extraGravityBoxes() const { return m_extraGravityBoxes; }

    // B4 New Map: clear voxels/props/entities, switch terrain+environment+seed, and
    // regenerate dungeon content. Connected players are respawned at the default pad.
    // Does not tear down the transport or peer map — Hello stays valid.
    void reseedWorld(std::uint32_t seed, GameRules::Terrain terrain,
                     GameRules::Environment environment,
                     GameRules::Template gameTemplate = GameRules::Template::Fps);

private:
    // Token bucket. Refilled from wall-clock ticks rather than frames so a peer
    // cannot buy edits by making the server run slowly.
    struct RateLimiter {
        float tokens = 0.0f;
        float refillPerSecond = 20.0f;
        float capacity = 40.0f;

        void configure(float perSecond, float burst) {
            refillPerSecond = perSecond;
            capacity = burst;
            tokens = burst;
        }
        void refill(float dt) {
            tokens = std::min(capacity, tokens + refillPerSecond * dt);
        }
        // Returns false when the peer has spent its allowance; the caller drops
        // the message rather than queuing it, because a queue is the thing a
        // flood is trying to build.
        bool consume(float amount = 1.0f) {
            if (tokens < amount) return false;
            tokens -= amount;
            return true;
        }
    };

    struct Player {
        // How this peer authenticated, and therefore what it may do to the
        // world. Default Player: arriving without proof grants nothing.
        PeerPermissions permissions;
        // Separate buckets so a burst of voxel edits cannot starve prop edits
        // and vice versa; each is refilled in tick().
        RateLimiter voxelEdits;
        RateLimiter propEdits;
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
        // H4: ship entity id while aboard (0 = on foot).
        std::uint32_t pilotingShip = 0;
        // 0 = foot, 1 = pilot (thrusters + ship cannon), 2 = passenger (ride only).
        std::uint8_t shipRole = 0;
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
            float speedMult = 1.0f; // both halves enforced (damage + CharacterController speed)
            float remaining = 0.0f; // seconds left
        };
        std::vector<ActiveModifier> modifiers;
        // Ignite: damage-over-time ticks. Each deals dps every fixed tick until it
        // expires; a burn that kills credits its source (registerFrag).
        struct Burn {
            float dps = 0.0f;
            float remaining = 0.0f;
            PeerId source = 0;
        };
        std::vector<Burn> burns;
        // F2 lag compensation: where this capsule stood on recent ticks, recorded
        // at the same point snapshots read poses — so rewinding to an acked
        // snapshot tick reproduces exactly what that snapshot showed the shooter.
        struct PastPose {
            std::uint64_t tick = 0; // 0 = slot never written
            glm::vec3 feet{0};
            bool crouched = false;
            bool spawned = false;
        };
        static constexpr std::size_t kPoseHistorySize = 32; // ~0.53 s at 60 Hz
        std::array<PastPose, kPoseHistorySize> poseHistory{};
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

public:
    // Count of authored props. Public so headless tests can assert that a
    // refused PlaceProp created nothing; the props themselves stay private.
    std::size_t propCount() const { return m_props.size(); }

private:
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
    // seat capsule doesn't explode out of the box). AI traffic ships are not
    // boardable and fly patrol loops (Space template).
    struct Ship {
        std::uint32_t id = 0;
        glm::vec3 pos{0};
        glm::vec3 vel{0};
        float yaw = 0.0f;
        float pitch = 0.0f;
        PeerId pilot = 0;     // 0 = empty pilot seat (AI ships always 0)
        PeerId passenger = 0; // 0 = empty co-pilot seat
        bool ai = false;
        bool groundVehicle = false; // H1 racer: car physics (no thrusters / pitch)
        float health = 500.0f;
        int hullVariant = 0; // ShipHulls catalog index
        glm::vec3 halfExtents{kShipHalfExtents};
        glm::vec3 seatOffset{kShipSeatOffset}; // pilot seat local
        glm::vec3 passengerOffset{0.6f, 0.15f, 0.1f}; // co-pilot / gunner seat
        // AI patrol: orbit center + radius/phase; fireCooldown for hostile shots.
        glm::vec3 patrolCenter{0};
        float patrolRadius = 30.0f;
        float patrolPhase = 0.0f;
        float patrolOmega = 0.25f; // rad/s
        float patrolAltitude = 12.0f;
        float fireCooldown = 0.0f;
        int hardpoint = 0;
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
    GravityVolumesMsg makeGravityVolumesMsg() const;
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
    void spawnAiTraffic();  // H4: patrol ships (Space template)
    void ensureShipBody(Ship& ship);   // create/update kinematic hull when empty
    void clearShipBody(Ship& ship);    // drop hull while piloted
    void updateShips(Transport& transport);
    void damageShip(Transport& transport, Ship& ship, float damage, PeerId source);
    bool tryBoardOrLeaveShip(Player& player); // true if handled (Use consumed)
    void ejectFromShip(Player& player, Ship& ship, float sideSign); // sideSign: +1 right leave
    // Aim origin for combat: eye on foot / passenger, twin hardpoints when pilot.
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
    // F2 lag compensation. recordPoseHistory runs once per tick, at the same
    // point broadcastSnapshot reads poses; rewoundPlayerPose fetches a target's
    // capsule as it stood at `tick`, returning false (caller uses live pose)
    // when that tick was never recorded or the target wasn't spawned yet.
    void recordPoseHistory();
    bool rewoundPlayerPose(const Player& target, std::uint64_t tick, glm::vec3& feet,
                           bool& crouched) const;
    // How far into the past a shooter's acked snapshot may pull targets: 250 ms
    // at the fixed 60 Hz tick. Beyond that a high-ping peer fires at live poses.
    static constexpr std::uint64_t kMaxRewindTicks = 15;
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
    // Apply + age Ignite damage-over-time on a player; handles death/kill-credit.
    void tickBurns(Transport& transport, PeerId peer, Player& player, float dt);
    static float damageMultOf(const Player& player); // product of active damage mults
    static float speedMultOf(const Player& player);  // product of active speed mults

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
    std::vector<GravityBoxVolume> m_extraGravityBoxes; // B3b-e editor volumes
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
    // B2 mesh level (physics only; client mirrors for render).
    MeshLevelDesc m_meshLevelDesc;
    std::vector<PhysicsWorld::BodyHandle> m_meshLevelBodies;
    void loadMeshLevelColliders();
    std::uint32_t m_nextEntityId = 1;
    // Sparse chip-damage: only voxels that have been shot, remaining hp. Entries
    // die with the block; pristine blocks are implicit full-hp.
    std::unordered_map<glm::ivec3, float, IVec3Hash> m_voxelDamage;
    std::unordered_map<PeerId, std::unique_ptr<Player>> m_players;
    // Per-client ring of the last N snapshots WE SENT THAT CLIENT (tick -> the
    // interest-scoped view it received). The delta baseline for a client is its
    // own ring[ackedSnapshotTick] — it must diff against exactly what that client
    // saw, since scoping can give two clients different entity sets. Ordered inner
    // map evicts the oldest cheaply. N = 32 (~1.6 s at 20 Hz). Erased on disconnect.
    std::unordered_map<PeerId, std::map<std::uint64_t, SnapshotMsg>> m_clientBaselines;
    // Deathmatch scoring: frags per peer, plus the latched match-over result.
    std::unordered_map<PeerId, int> m_frags;
    bool m_matchOver = false;
    PeerId m_matchWinner = 0;
    // Periodic autosave (0 interval = disabled).
    std::string m_autosavePath;
    std::uint64_t m_autosaveIntervalTicks = 0;
    std::uint64_t m_lastAutosaveTick = 0;
    BlockPalette m_palette;
    GameRules m_rules;
    ItemRegistry m_items;
    DefaultItems m_defaultItems;
    std::optional<SavedPlayer> m_pendingRestore; // applied to the next Hello
    NetPolicy m_netPolicy;
    // Rejected packets are logged, but a flood must not become a disk-fill or a
    // log-spam amplifier: one line per peer per second, with a count.
    struct RejectLog {
        std::uint64_t lastTick = 0;
        std::uint32_t suppressed = 0;
    };
    std::unordered_map<PeerId, RejectLog> m_rejectLog;
    void noteRejected(PeerId peer, const char* what);
    std::uint32_t m_seed = 0;
    std::uint64_t m_tick = 0;
};

} // namespace meat
