#include "game/ServerSim.h"
#include "game/ServerSimInternal.h"
#include "engine/asset/ModelLoader.h"
#include "engine/core/Log.h"
#include "engine/core/ViewMath.h"
#include "engine/net/DeltaSnapshot.h"
#include "game/DungeonGen.h"
#include "game/Environment.h"
#include "game/Pathfinder.h"
#include "game/ShipControl.h"
#include "game/ShipHulls.h"
#include "game/WeaponFire.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <fstream>
#include <system_error>
#include <thread>


namespace meat {
namespace {
// kFixedDtServer lives in ServerSimInternal.h (shared across ServerSim TUs).
constexpr int kSnapshotEvery = 3; // 60 Hz sim → 20 Hz snapshots
// Save-file schema version. Bump when the on-disk layout changes; the loader
// rejects a save from a NEWER engine (it can't know the layout) and treats a
// versionless file as the pre-versioning v0 (best-effort read). Same discipline
// as the wire's kProtocolVersion so a content change can't silently misread old saves.
constexpr int kSaveVersion = 1;
// defaultSpawnPos() lives in ServerSimInternal.h (shared with ServerSimCombat.cpp).

// World-authoring limits. These bound what a peer may ask for even when it is
// allowed to author at all: permission answers "may you edit", these answer
// "is what you sent a thing a real editor would send".
constexpr int kMaxEditCoord = 100000;    // voxel cells from origin
constexpr std::size_t kMaxProps = 20000; // props in the world, total
constexpr float kMaxPropCoord = 1.0e6f;  // metres from origin
constexpr float kMinPropScale = 1.0e-3f;
constexpr float kMaxPropScale = 1.0e3f;

// A transform arrives as 16 floats from the network. Any of them may be NaN or
// infinity, which propagate into physics and rendering and are not recoverable
// once they are in a broadphase — so they are refused at the edge rather than
// clamped. Scale and position are then bounded to values a real editor emits.
bool isSaneTransform(const glm::mat4& m) {
    const float* f = glm::value_ptr(m);
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(f[i])) return false;
    }
    if (std::abs(m[3][0]) > kMaxPropCoord || std::abs(m[3][1]) > kMaxPropCoord ||
        std::abs(m[3][2]) > kMaxPropCoord)
        return false;
    // Column lengths are the axis scales. A zero scale collapses a collider to a
    // degenerate shape; a huge one is a physics bomb.
    for (int c = 0; c < 3; ++c) {
        const float len = glm::length(glm::vec3(m[c]));
        if (!std::isfinite(len) || len < kMinPropScale || len > kMaxPropScale)
            return false;
    }
    return true;
}

// 128 bits of hex from the platform entropy source. std::random_device is the
// only OS-backed generator the standard gives us; it is seeded from the system
// CSPRNG on the platforms this engine targets. Deliberately not seeded from a
// clock — a token derived from the start time is guessable by anyone who knows
// roughly when the server came up.
std::string makeEditorToken() {
    std::random_device rd;
    std::string out;
    out.reserve(32);
    static constexpr char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        const std::uint32_t word = rd();
        for (int nibble = 7; nibble >= 0; --nibble)
            out.push_back(kHex[(word >> (nibble * 4)) & 0xFu]);
    }
    return out;
}

constexpr float kPlaceInterval = 0.20f;
// Combat constants + raySegmentDistance live in ServerSimInternal.h (shared with
// ServerSimCombat.cpp).
} // namespace

void ServerSim::rebuildGravityField() {
    const EnvSettings env = envSettings(m_rules.environment);
    const bool space = m_rules.environment == GameRules::Environment::Space;
    configureDefaultGravityField(m_gravity, env.gravity, space);
    for (const GravityBoxVolume& box : m_extraGravityBoxes) m_gravity.addBox(box);
    // Rigid-body Jolt gravity stays the ambient Y component; characters sample the field.
    m_physics.setGravity(env.gravity);
}

void ServerSim::setExtraGravityBoxes(std::vector<GravityBoxVolume> boxes) {
    m_extraGravityBoxes = std::move(boxes);
    rebuildGravityField();
}

GravityVolumesMsg ServerSim::makeGravityVolumesMsg() const {
    GravityVolumesMsg msg;
    msg.volumes.reserve(m_extraGravityBoxes.size());
    for (const GravityBoxVolume& b : m_extraGravityBoxes) {
        GravityVolumeEntry e;
        e.min = b.min;
        e.max = b.max;
        e.gravity = b.gravity;
        e.priority = b.priority;
        msg.volumes.push_back(e);
    }
    return msg;
}

void ServerSim::broadcastGravityVolumes(Transport& transport) const {
    const GravityVolumesMsg msg = makeGravityVolumesMsg();
    const auto packet = pack(msg);
    for (const auto& [peer, player] : m_players) {
        if (!player || !player->helloDone) continue;
        transport.send(peer, packet, true);
    }
}

void ServerSim::reseedWorld(std::uint32_t seed, GameRules::Terrain terrain,
                            GameRules::Environment environment,
                            GameRules::Template gameTemplate) {
    m_seed = seed;
    m_rules.terrain = terrain;
    m_rules.environment = environment;
    m_rules.gameTemplate = gameTemplate;
    rebuildGravityField();

    // Tear down dynamic world objects first (props drop colliders).
    for (WorldProp& prop : m_props) {
        if (prop.body != PhysicsWorld::kInvalidBody) m_physics.removeStaticBox(prop.body);
    }
    m_props.clear();
    m_nextPropId = 1;
    m_entities.clear();
    m_projectiles.clear();
    m_deployables.clear();
    m_npcs.clear();
    m_turrets.clear();
    m_companions.clear();
    m_voxelDamage.clear();
    m_nextEntityId = 1;
    m_clientBaselines.clear();
    m_frags.clear();
    m_matchOver = false;
    m_matchWinner = 0;

    // Chunks + colliders + navmesh (unload callback removes both).
    m_voxels.clearWorld();
    m_voxels.setGenerator(makeTerrainGenerator(m_seed, m_palette, m_rules.terrain));

    // Players: teleport to the new pad. Controllers already exist for anyone who
    // had joined; setState is enough (no re-init). Unspawned peers still init later.
    const glm::vec3 spawn = defaultSpawnPos();
    for (auto& [peer, player] : m_players) {
        if (!player) continue;
        if (player->spawned) {
            player->controller.setState(spawn, glm::vec3(0));
            player->controller.setGravity(m_gravity.sample(spawn));
        } else {
            player->spawnOverride = spawn;
        }
        player->health = 100.0f;
        player->fireCooldown = 0.0f;
        player->placeCooldown = 0.0f;
        player->useCooldown = 0.0f;
        player->prevFire = false;
        player->prevReload = false;
        player->burstRemaining = 0;
        player->reloadCooldown = 0.0f;
        player->reloadingWeapon = 0;
        player->modifiers.clear();
        player->ackedSnapshotTick = 0; // force a keyframe after the world swap
    }

    for (Ship& s : m_ships) clearShipBody(s);
    m_ships.clear();
    for (auto& [peer, player] : m_players) {
        if (!player) continue;
        player->pilotingShip = 0;
        player->shipRole = 0;
    }
    spawnDungeonLoot();
    spawnDungeonNpcs();
    spawnDemoShip();
    m_scripts.onInit(m_seed);
    log::info("server: New Map — seed {} terrain {} env {} template {}", m_seed,
              static_cast<int>(terrain), static_cast<int>(environment),
              static_cast<int>(gameTemplate));
}

ServerSim::~ServerSim() { m_jobs.stop(); }

bool ServerSim::init(std::uint32_t worldSeed) {
    m_seed = worldSeed;
    // One editor token per boot. Restarting the server invalidates the previous
    // one, so a token that leaks is worthless by the next session. It is handed
    // to the owner's own client in-process and never logged.
    if (m_netPolicy.editorToken.empty()) m_netPolicy.editorToken = makeEditorToken();
    if (!m_physics.init()) return false;
    // World Environment preset drives gravity field base + fog/ambient (client). Characters
    // sample m_gravity each tick (B3b volumes / orbital bodies).
    rebuildGravityField();
    m_jobs.start(std::thread::hardware_concurrency());

    m_palette = registerDefaultBlocks(m_voxels.blockRegistry());
    m_defaultItems = registerDefaultItems(m_items, m_palette.stone);
    m_voxels.setGenerator(makeTerrainGenerator(m_seed, m_palette, m_rules.terrain));
    m_voxels.setMeshReadyCallback([this](ChunkPos pos, ChunkMeshData data) {
        if (!data.indices.empty()) {
            m_physics.syncChunkCollider(pos, data);
            m_navmesh.addChunk(pos, data); // same geometry feeds the optional navmesh
        } else {
            m_physics.removeChunkCollider(pos);
            m_navmesh.removeChunk(pos);
        }
    });
    m_voxels.setChunkUnloadedCallback([this](ChunkPos pos) {
        m_physics.removeChunkCollider(pos);
        m_navmesh.removeChunk(pos);
    });
    loadMeshLevelColliders();
    spawnDungeonLoot();
    spawnDungeonNpcs();
    spawnDemoShip();
    setupScripting();
    m_scripts.onInit(m_seed);
    return true;
}

void ServerSim::setMeshLevel(std::string assetPath, float scale) {
    m_meshLevelDesc = makeMeshLevelDesc(std::move(assetPath), scale > 1e-4f ? scale : 1.0f);
}

void ServerSim::setMeshLevelDesc(MeshLevelDesc desc) {
    m_meshLevelDesc = std::move(desc);
}

void ServerSim::loadMeshLevelColliders() {
    for (PhysicsWorld::BodyHandle b : m_meshLevelBodies) {
        if (b != PhysicsWorld::kInvalidBody) m_physics.removeStaticBox(b);
    }
    m_meshLevelBodies.clear();
    if (m_meshLevelDesc.instances.empty()) return;

    int ok = 0;
    for (const MeshLevelInstance& inst : m_meshLevelDesc.instances) {
        if (inst.assetPath.empty()) continue;
        ModelImportOptions opts;
        opts.scale = inst.scale > 1e-4f ? inst.scale : 1.0f;
        const auto model = loadStaticModel(inst.assetPath, opts);
        if (!model) {
            log::error("server MeshLevel: failed to load '{}'", inst.assetPath);
            continue;
        }
        std::vector<glm::vec3> positions;
        positions.reserve(model->mesh.vertices.size());
        for (const VoxelVertex& v : model->mesh.vertices) {
            const glm::vec4 w = inst.transform * glm::vec4(v.pos, 1.0f);
            positions.emplace_back(w.x, w.y, w.z);
        }
        const auto body = m_physics.addStaticTriangleMesh(positions, model->mesh.indices);
        if (body == PhysicsWorld::kInvalidBody) {
            log::error("server MeshLevel: collider failed for '{}'", inst.assetPath);
            continue;
        }
        m_meshLevelBodies.push_back(body);
        ++ok;
        log::info("server MeshLevel: '{}' ({} tris, scale {})", inst.assetPath,
                  model->mesh.indices.size() / 3, opts.scale);
    }
    log::info("server MeshLevel: {} part(s) active", ok);
}

bool ServerSim::propBounds(const std::string& asset, glm::vec3& outMin, glm::vec3& outMax) const {
    if (const auto it = m_propBoundsCache.find(asset); it != m_propBoundsCache.end()) {
        outMin = it->second.first;
        outMax = it->second.second;
        return true;
    }
    // center=true matches the client's editorPropMesh load, so both derive the
    // same local AABB → identical world colliders on server and client.
    const auto model = loadStaticModel(asset, {.center = true});
    if (!model) return false;
    m_propBoundsCache.emplace(asset, std::make_pair(model->boundsMin, model->boundsMax));
    outMin = model->boundsMin;
    outMax = model->boundsMax;
    return true;
}

bool ServerSim::addProp(Transport* transport, const std::string& asset,
                        const glm::mat4& transform, std::uint32_t id) {
    glm::vec3 lo, hi;
    if (!propBounds(asset, lo, hi)) {
        log::warn("server: prop '{}' failed to load — not placed", asset);
        return false;
    }
    WorldProp prop;
    prop.id = id != 0 ? id : m_nextPropId++;
    prop.asset = asset;
    prop.transform = transform;
    glm::vec3 center, half;
    transformedAabb(transform, lo, hi, center, half);
    prop.body = m_physics.addStaticBox(center, half);
    if (id != 0) m_nextPropId = std::max(m_nextPropId, id + 1); // keep ids monotonic post-load
    m_props.push_back(std::move(prop));
    if (transport) broadcastPropAdded(*transport, m_props.back());
    log::info("server: prop {} '{}' placed with collider", m_props.back().id, asset);
    return true;
}

bool ServerSim::moveProp(Transport* transport, std::uint32_t id, const glm::mat4& transform) {
    auto it = std::find_if(m_props.begin(), m_props.end(),
                           [id](const WorldProp& p) { return p.id == id; });
    if (it == m_props.end()) return false;
    glm::vec3 lo, hi;
    if (!propBounds(it->asset, lo, hi)) return false;
    if (it->body != PhysicsWorld::kInvalidBody) m_physics.removeStaticBox(it->body);
    it->transform = transform;
    glm::vec3 center, half;
    transformedAabb(transform, lo, hi, center, half);
    it->body = m_physics.addStaticBox(center, half);
    if (transport) broadcastPropAdded(*transport, *it);
    return true;
}

bool ServerSim::removeProp(Transport* transport, std::uint32_t id) {
    auto it = std::find_if(m_props.begin(), m_props.end(),
                           [id](const WorldProp& p) { return p.id == id; });
    if (it == m_props.end()) return false;
    if (it->body != PhysicsWorld::kInvalidBody) m_physics.removeStaticBox(it->body);
    m_props.erase(it);
    if (transport) broadcastPropRemoved(*transport, id);
    log::info("server: prop {} removed", id);
    return true;
}

void ServerSim::broadcastPropAdded(Transport& transport, const WorldProp& prop) const {
    const PropAddedMsg msg{prop.id, prop.asset, prop.transform};
    for (const auto& [peer, unused] : m_players) transport.send(peer, pack(msg), true);
}

void ServerSim::broadcastPropRemoved(Transport& transport, std::uint32_t id) const {
    const RemovePropMsg msg{id};
    for (const auto& [peer, unused] : m_players) transport.send(peer, pack(msg), true);
}

void ServerSim::setupScripting() {
    m_scriptRng ^= m_seed * 0x9E3779B97F4A7C15ull; // per-world deterministic stream
    // Capability table: scripts get exactly these server-authoritative actions.
    ScriptApi api;
    api.log = [](const std::string& s) { log::info("[lua] {}", s); };
    api.setBlock = [this](int x, int y, int z, int b) {
        const auto id = static_cast<BlockId>(b < 0 ? 0 : b);
        if (!m_voxels.blockRegistry().isValid(id)) return;
        if (std::abs(x) > 100000 || std::abs(y) > 100000 || std::abs(z) > 100000)
            return; // same guard as the network path; unbounded coords → OOM
        // Edits go through applyVoxelOp when a client is connected (broadcasts);
        // otherwise into the overlay, which overlay-on-join replays to joiners.
        if (m_activeTransport)
            applyVoxelOp(*m_activeTransport, {glm::ivec3(x, y, z), id});
        else
            m_voxels.setBlock(glm::ivec3(x, y, z), id);
    };
    api.getBlock = [this](int x, int y, int z) {
        return static_cast<int>(m_voxels.blockAt(glm::ivec3(x, y, z)));
    };
    api.spawnPickup = [this](float x, float y, float z, int item, int count) {
        if (item <= 0 || count <= 0) return;
        if (m_entities.size() >= 4096) return; // entity cap: a runaway loop can't OOM us
        WorldEntity e;
        e.id = m_nextEntityId++;
        e.type = EntityArchetype::ItemPickup;
        e.pos = {x, y, z};
        e.item = static_cast<ItemId>(item);
        e.count = static_cast<std::uint16_t>(std::min(count, 0xFFFF)); // clamp, no wrap
        m_entities.push_back(e);
    };
    api.playerCount = [this] { return playerCount(); };
    api.tick = [this] { return m_tick; };
    // Deterministic seeded RNG so scripted content matches across peers/replays.
    api.randi = [this](int lo, int hi) {
        if (hi < lo) std::swap(lo, hi);
        m_scriptRng = m_scriptRng * 6364136223846793005ull + 1442695040888963407ull;
        // Widen before subtracting: (hi - lo) in int would overflow on a wide range.
        const std::uint64_t span =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(hi) - lo) + 1;
        return static_cast<int>(lo + static_cast<std::int64_t>((m_scriptRng >> 33) % span));
    };
    api.itemId = [this](const std::string& n) -> int {
        if (n == "pistol") return m_defaultItems.pistol;
        if (n == "ammo9mm") return m_defaultItems.ammo9mm;
        if (n == "shells") return m_defaultItems.shells;
        if (n == "rockets") return m_defaultItems.rockets;
        if (n == "medkit") return m_defaultItems.medkit;
        if (n == "stone") return m_defaultItems.stoneBlock;
        if (n == "rpg") return m_defaultItems.rpg;
        if (n == "shotgun") return m_defaultItems.shotgun;
        return 0;
    };
    // C6: prop helpers for node graphs (Get World Object / Highlight Object).
    api.highlightProp = [this](int propId, float seconds) {
        if (propId <= 0) return;
        const float dur = std::clamp(seconds, 0.1f, 30.0f);
        bool found = false;
        for (const WorldProp& p : m_props) {
            if (static_cast<int>(p.id) == propId) {
                found = true;
                break;
            }
        }
        if (!found) return;
        ScriptFxMsg fx;
        fx.kind = 0;
        fx.id = static_cast<std::uint32_t>(propId);
        fx.duration = dur;
        fx.r = 0.15f;
        fx.g = 0.65f;
        fx.b = 1.0f;
        if (m_activeTransport) {
            for (const auto& [peer, pl] : m_players) {
                if (pl) m_activeTransport->send(peer, pack(fx), true);
            }
        }
        log::info("[lua] highlight_prop {} for {:.1f}s", propId, dur);
    };
    api.propPos = [this](int propId, float& x, float& y, float& z) -> bool {
        for (const WorldProp& p : m_props) {
            if (static_cast<int>(p.id) == propId) {
                const glm::vec3 pos = glm::vec3(p.transform[3]);
                x = pos.x;
                y = pos.y;
                z = pos.z;
                return true;
            }
        }
        return false;
    };
    api.propCount = [this] { return static_cast<int>(m_props.size()); };
    // C6-b: damage / announce / health for node graphs.
    api.damagePlayer = [this](int peerId, float amount) {
        if (amount <= 0.0f) return;
        const auto peer = static_cast<PeerId>(peerId);
        auto it = m_players.find(peer);
        if (it == m_players.end() || !it->second || !it->second->spawned) return;
        Player& pl = *it->second;
        const float dealt = std::min(amount, 500.0f);
        pl.health -= dealt;
        log::info("[lua] damage_player {} for {:.1f} (hp now {:.0f})", peerId, dealt, pl.health);
        if (pl.health > 0.0f) return;
        log::info("[lua] player {} killed by script", peerId);
        killPlayer(pl, peer, 0); // script/environment kill: no frag credit
    };
    api.announce = [this](const std::string& s) {
        if (s.empty()) return;
        const std::string msg = s.size() > 200 ? s.substr(0, 200) : s;
        log::info("[lua] announce: {}", msg);
        ScriptFxMsg fx;
        fx.kind = 1;
        fx.id = 0;
        fx.duration = 4.0f;
        fx.r = 1.0f;
        fx.g = 0.9f;
        fx.b = 0.35f;
        fx.text = msg;
        if (m_activeTransport) {
            for (const auto& [peer, pl] : m_players) {
                if (pl) m_activeTransport->send(peer, pack(fx), true);
            }
        }
    };
    api.playerHealth = [this](int peerId) -> float {
        const auto peer = static_cast<PeerId>(peerId);
        auto it = m_players.find(peer);
        if (it == m_players.end() || !it->second) return 0.0f;
        return it->second->health;
    };
    // C6-c watches: update live table + a log line (filter Output Log with [watch]).
    api.watch = [](const std::string& name, const std::string& value) {
        log::setWatch(name, value);
        log::info("[watch] {} = {}", name, value);
    };
    // Effect primitives for Lua-defined effects (shared with the built-in kinds).
    api.ignite = [this](int peer, float dps, float seconds, int source) {
        applyDamageOverTime(static_cast<PeerId>(peer), dps, seconds,
                            static_cast<PeerId>(source));
    };
    api.chainDamage = [this](float x, float y, float z, float dmg, int maxTargets,
                             float range, int source) {
        applyChainDamage(static_cast<PeerId>(source), glm::vec3(x, y, z), dmg, maxTargets,
                         range);
    };
    api.spawnTurret = [this](int source, float x, float y, float z) -> int {
        return static_cast<int>(spawnTurret(static_cast<PeerId>(source), glm::vec3(x, y, z)));
    };
    api.spawnCompanion = [this](int source, float x, float y, float z) -> int {
        return static_cast<int>(
            spawnCompanion(static_cast<PeerId>(source), glm::vec3(x, y, z)));
    };
    m_scripts.bind(std::move(api));
    m_scripts.loadDir(m_scriptDir);
}

void ServerSim::spawnDungeonNpcs() {
    // Void / Space templates have no underground dungeon rooms worth filling.
    if (m_rules.terrain == GameRules::Terrain::Void ||
        m_rules.gameTemplate == GameRules::Template::Space) {
        log::info("server: skipping dungeon NPCs (void/space template)");
        return;
    }
    const DungeonLayout layout = DungeonLayout::generate(m_seed, {});
    std::size_t i = 0;
    for (const auto& room : layout.rooms()) {
        // Zombies shamble every 4th room, chasers every 3rd, shooters every 5th; the
        // entrance room stays clear. A room that matches several takes the first here.
        const bool zombie = i % 4 == 2, chaser = i % 3 == 1, shooter = i % 5 == 4;
        if (zombie || chaser || shooter) {
            Npc npc;
            npc.id = m_nextEntityId++;
            npc.type = zombie    ? EntityArchetype::NpcZombie
                       : chaser  ? EntityArchetype::NpcChaser
                                 : EntityArchetype::NpcShooter;
            npc.health = zombie ? 120.0f : chaser ? 60.0f : 40.0f;
            const glm::ivec3 c{std::min((room.min.x + room.max.x) / 2 + 1, room.max.x),
                               room.min.y,
                               std::min((room.min.z + room.max.z) / 2 + 1, room.max.z)};
            npc.pos = (glm::vec3(c) + glm::vec3(0.5f, 0.0f, 0.5f)) * kVoxelSize;
            m_npcs.push_back(std::move(npc));
        }
        ++i;
    }
    log::info("server: spawned {} dungeon NPCs", m_npcs.size());
}

void ServerSim::ensureShipBody(Ship& ship) {
    if (ship.body != PhysicsWorld::kInvalidBody) {
        m_physics.setBodyTransform(ship.body, ship.pos, shipOrientation(ship.yaw, ship.pitch));
        return;
    }
    ship.body = m_physics.addKinematicBox(ship.pos, ship.halfExtents);
    if (ship.body != PhysicsWorld::kInvalidBody)
        m_physics.setBodyTransform(ship.body, ship.pos, shipOrientation(ship.yaw, ship.pitch));
}

void ServerSim::clearShipBody(Ship& ship) {
    if (ship.body == PhysicsWorld::kInvalidBody) return;
    m_physics.removeStaticBox(ship.body);
    ship.body = PhysicsWorld::kInvalidBody;
}

void ServerSim::spawnSpaceDecor() {
    const glm::vec3 pad = defaultSpawnPos();
    // Junkyard wreckage — small landmark near the pad (© Sebastian Sosnowski, CC-BY 4.0).
    if (const std::string junk = resolveDecorPath(kJunkyardStaged, kJunkyardVault); !junk.empty()) {
        if (const auto t = decorTransform(junk, pad + glm::vec3(-16.0f, 0.0f, 12.0f), 14.0f, 0.6f)) {
            if (addProp(nullptr, junk, *t, 0))
                log::info("server: space decor — junkyard wreck (© {})", kJunkyardAuthor);
        }
    }
    // Spacestation — large distant landmark (© Gerardo Justel, CC-BY 4.0; vault ~31 MB).
    if (const std::string station = resolveDecorPath(kStationStaged, kStationVault);
        !station.empty()) {
        if (const auto t =
                decorTransform(station, pad + glm::vec3(0.0f, 8.0f, -70.0f), 55.0f, 0.2f)) {
            if (addProp(nullptr, station, *t, 0))
                log::info("server: space decor — station landmark (© {})", kStationAuthor);
        } else {
            log::warn("server: station mesh at '{}' failed to load", station);
        }
    }
}

void ServerSim::spawnDemoShip() {
    // H4: empty ships near the pad. Space template gets one of each CC-BY hull.
    // H1 Racer: one ground car on the pad (box collider, no thrusters).
    const bool space = m_rules.gameTemplate == GameRules::Template::Space;
    const bool racer = m_rules.gameTemplate == GameRules::Template::Racer;
    if (racer) {
        const glm::vec3 pad = defaultSpawnPos();
        // Two player cars on the grid + one AI pace car on a loop.
        for (int i = 0; i < 2; ++i) {
            Ship car;
            car.id = m_nextEntityId++;
            car.groundVehicle = true;
            car.hullVariant = i % kShipHullCount;
            car.halfExtents = kRacerHalfExtents;
            car.seatOffset = kRacerSeatOffset;
            car.passengerOffset = glm::vec3(0.55f, 0.25f, 0.1f);
            car.health = 400.0f;
            car.pos = pad + glm::vec3(4.0f + static_cast<float>(i) * 3.5f, kRacerHalfExtents.y,
                                      2.0f);
            car.yaw = 0.0f;
            car.pitch = 0.0f;
            ensureShipBody(car);
            m_ships.push_back(car);
            log::info("server: racer car {} — Use (E) to drive (WASD + Space hop / Ctrl brake)",
                      car.id);
        }
        // AI pace car orbits the pad on the ground plane (not boardable).
        Ship ai;
        ai.id = m_nextEntityId++;
        ai.groundVehicle = true;
        ai.ai = true;
        ai.hullVariant = 2 % kShipHullCount;
        ai.halfExtents = kRacerHalfExtents;
        ai.seatOffset = kRacerSeatOffset;
        ai.health = 300.0f;
        ai.patrolCenter = pad + glm::vec3(0.0f, kRacerHalfExtents.y, 0.0f);
        ai.patrolRadius = 22.0f;
        ai.patrolPhase = 0.0f;
        ai.patrolOmega = 0.55f;
        ai.patrolAltitude = 0.0f;
        ai.pos = ai.patrolCenter + glm::vec3(ai.patrolRadius, 0.0f, 0.0f);
        m_ships.push_back(ai);
        log::info("server: AI pace car {} on loop", ai.id);
        return;
    }
    const int count = space ? kShipHullCount : 1;
    for (int i = 0; i < count; ++i) {
        Ship ship;
        ship.id = m_nextEntityId++;
        ship.hullVariant = i % kShipHullCount;
        // Size collider from the real mesh when possible (same import as the client).
        glm::vec3 half = kShipHalfExtents;
        if (loadShipHull(ship.hullVariant, half)) ship.halfExtents = half;
        else ship.halfExtents = kShipHalfExtents;
        // Seat slightly above center, forward of midships (local -Z is forward).
        ship.seatOffset =
            glm::vec3(0.0f, ship.halfExtents.y * 0.25f, -ship.halfExtents.z * 0.2f);
        // Park above the pad so longer hulls clear the ground; space out by length.
        const float side = 8.0f + static_cast<float>(i) * (ship.halfExtents.x * 2.0f + 3.0f);
        ship.pos = defaultSpawnPos() + glm::vec3(side, ship.halfExtents.y + 0.4f,
                                                 static_cast<float>(i) * 2.5f);
        ship.yaw = -0.35f * static_cast<float>(i);
        ship.pitch = 0.0f;
        ensureShipBody(ship);
        m_ships.push_back(ship);
        const auto& hull = shipHullDefs()[static_cast<std::size_t>(ship.hullVariant)];
        log::info("server: ship {} hull={} (© {}) half=({:.1f},{:.1f},{:.1f}) — Use (E) to board",
                  ship.id, hull.id, hull.author, ship.halfExtents.x, ship.halfExtents.y,
                  ship.halfExtents.z);
    }
    if (space) {
        spawnSpaceDecor();
        spawnAiTraffic();
    }
}

void ServerSim::spawnAiTraffic() {
    // 4 patrol craft orbiting the pad — not boardable, light hostile if a player
    // comes within range. Deterministic phase offsets from the world seed.
    const glm::vec3 pad = defaultSpawnPos();
    constexpr int kTraffic = 4;
    for (int i = 0; i < kTraffic; ++i) {
        Ship ship;
        ship.id = m_nextEntityId++;
        ship.ai = true;
        ship.hullVariant = i % kShipHullCount;
        glm::vec3 half = kShipHalfExtents;
        if (loadShipHull(ship.hullVariant, half)) ship.halfExtents = half;
        ship.health = 180.0f;
        ship.patrolCenter = pad + glm::vec3(0.0f, 14.0f, 0.0f);
        ship.patrolRadius = 28.0f + 6.0f * static_cast<float>(i);
        ship.patrolPhase = static_cast<float>((m_seed + static_cast<std::uint32_t>(i) * 97u) % 628) *
                           0.01f;
        ship.patrolOmega = 0.18f + 0.04f * static_cast<float>(i % 3);
        ship.patrolAltitude = 10.0f + 3.0f * static_cast<float>(i);
        const float c = std::cos(ship.patrolPhase), s = std::sin(ship.patrolPhase);
        ship.pos = ship.patrolCenter +
                   glm::vec3(c * ship.patrolRadius, ship.patrolAltitude * 0.15f * s,
                             s * ship.patrolRadius);
        // No kinematic body — AI ships move every tick; board blocked by ai flag.
        m_ships.push_back(ship);
    }
    log::info("server: spawned {} AI patrol ships", kTraffic);
}

const ServerSim::Ship* ServerSim::findShip(std::uint32_t id) const {
    for (const Ship& s : m_ships)
        if (s.id == id) return &s;
    return nullptr;
}
ServerSim::Ship* ServerSim::findShip(std::uint32_t id) {
    for (Ship& s : m_ships)
        if (s.id == id) return &s;
    return nullptr;
}

glm::vec3 ServerSim::combatMuzzle(const Player& player) const {
    // Only the pilot uses twin hardpoints; passengers shoot from the eye (gunner seat).
    if (player.pilotingShip != 0 && player.shipRole == 1) {
        if (const Ship* ship = findShip(player.pilotingShip)) {
            const float side = (player.shipHardpoint & 1) == 0 ? -1.0f : 1.0f;
            return shipHardpointWorld(ship->pos, ship->yaw, ship->pitch, ship->halfExtents, side);
        }
    }
    return player.controller.position() + player.controller.up() * player.controller.eyeHeight();
}

void ServerSim::ejectFromShip(Player& player, Ship& ship, float sideSign) {
    const glm::quat q = shipOrientation(ship.yaw, ship.pitch);
    const glm::vec3 right = q * glm::vec3(1.0f, 0.0f, 0.0f);
    const float leaveDist = std::max(kShipLeaveOffset, ship.halfExtents.x + 1.5f);
    const glm::vec3 leave =
        ship.pos + right * (leaveDist * sideSign) + glm::vec3(0.0f, 0.5f, 0.0f);
    if (player.shipRole == 1) ship.pilot = 0;
    else if (player.shipRole == 2) ship.passenger = 0;
    player.pilotingShip = 0;
    player.shipRole = 0;
    // Promote passenger to pilot when the pilot leaves.
    if (ship.pilot == 0 && ship.passenger != 0 && !ship.ai) {
        if (auto it = m_players.find(ship.passenger); it != m_players.end() && it->second) {
            it->second->shipRole = 1;
            ship.pilot = ship.passenger;
            ship.passenger = 0;
            log::info("server: passenger promoted to pilot on ship {}", ship.id);
        }
    }
    if (ship.pilot == 0 && ship.passenger == 0) ensureShipBody(ship);
    else clearShipBody(ship);
    player.controller.setState(leave, glm::vec3(0));
}

void ServerSim::damageShip(Transport& transport, Ship& ship, float damage, PeerId source) {
    if (ship.health <= 0.0f) return;
    ship.health -= damage;
    if (ship.health > 0.0f) return;
    log::info("server: ship {} destroyed by peer {}", ship.id, source);
    auto ejectPeer = [&](PeerId p, float side) {
        if (p == 0) return;
        if (auto it = m_players.find(p); it != m_players.end() && it->second) {
            it->second->pilotingShip = 0;
            it->second->shipRole = 0;
            it->second->controller.setState(ship.pos + glm::vec3(side, 2, 0), glm::vec3(0));
            it->second->health = std::max(10.0f, it->second->health - 30.0f);
        }
    };
    ejectPeer(ship.pilot, 1.5f);
    ejectPeer(ship.passenger, -1.5f);
    ship.pilot = 0;
    ship.passenger = 0;
    clearShipBody(ship);
    spawnPickup(m_defaultItems.rockets, 2, ship.pos + glm::vec3(0, 1, 0));
    spawnPickup(m_defaultItems.medkit, 1, ship.pos + glm::vec3(0.5f, 1, 0));
    (void)transport;
}

void ServerSim::updateShips(Transport& transport) {
    constexpr float kAiEngage = 42.0f;
    constexpr float kAiDamage = 8.0f;
    constexpr float kAiFireInterval = 0.55f;
    constexpr float kAiCruise = 11.0f;

    for (Ship& ship : m_ships) {
        if (ship.health <= 0.0f) continue;

        if (ship.ai) {
            ship.fireCooldown -= kFixedDtServer;
            ship.patrolPhase += ship.patrolOmega * kFixedDtServer;
            const float c = std::cos(ship.patrolPhase), s = std::sin(ship.patrolPhase);
            const glm::vec3 target =
                ship.groundVehicle
                    ? ship.patrolCenter +
                          glm::vec3(c * ship.patrolRadius, 0.0f, s * ship.patrolRadius)
                    : ship.patrolCenter +
                          glm::vec3(c * ship.patrolRadius, ship.patrolAltitude * 0.2f * s,
                                    s * ship.patrolRadius);
            ShipPose pose{ship.pos, ship.vel, ship.yaw, ship.pitch};
            if (ship.groundVehicle) {
                // Flat-track AI: chase the orbit point with racer-style planar motion.
                integrateShipAi(pose, target, kFixedDtServer, kAiCruise * 0.85f,
                                m_gravity.sample(ship.pos));
                pose.pitch = 0.0f;
                pose.pos.y = ship.patrolCenter.y;
                pose.vel.y = 0.0f;
            } else {
                integrateShipAi(pose, target, kFixedDtServer, kAiCruise,
                                m_gravity.sample(ship.pos));
            }
            ship.pos = pose.pos;
            ship.vel = pose.vel;
            ship.yaw = pose.yaw;
            ship.pitch = pose.pitch;

            // Hostile space traffic only — ground pace cars just race.
            if (ship.groundVehicle) continue;
            if (ship.fireCooldown <= 0.0f) {
                PeerId bestPeer = 0;
                Player* best = nullptr;
                float bestD = kAiEngage;
                for (auto& [peer, pl] : m_players) {
                    if (!pl || !pl->spawned || pl->health <= 0.0f) continue;
                    const float d = glm::length(pl->controller.position() - ship.pos);
                    if (d < bestD) {
                        bestD = d;
                        best = pl.get();
                        bestPeer = peer;
                    }
                }
                if (best) {
                    ship.fireCooldown = kAiFireInterval;
                    ship.hardpoint ^= 1;
                    const float side = (ship.hardpoint & 1) == 0 ? -1.0f : 1.0f;
                    const glm::vec3 muzzle =
                        shipHardpointWorld(ship.pos, ship.yaw, ship.pitch, ship.halfExtents, side);
                    // Lead: hitscan is instant, but thruster pilots slide between fire ticks.
                    // Lead scales with range (far targets get more prediction time).
                    const glm::vec3 tgtFeet = best->controller.position();
                    const float rangeLead = std::clamp(bestD * 0.012f, 0.10f, 0.45f);
                    const glm::vec3 aim =
                        tgtFeet + best->controller.velocity() * rangeLead +
                        best->controller.up() * (best->controller.eyeHeight() * 0.55f);
                    const glm::vec3 to = aim - muzzle;
                    const float dist = glm::length(to);
                    if (dist > 0.5f) {
                        const glm::vec3 dir = to / dist;
                        const glm::vec3 feet = best->controller.position();
                        const float height = best->controller.crouched() ? 0.95f : 1.8f;
                        const glm::vec3 a = feet + best->controller.up() * kCapsuleRadius;
                        const glm::vec3 b =
                            feet + best->controller.up() * (height - kCapsuleRadius);
                        float tRay = 0;
                        if (raySegmentDistance(muzzle, dir, dist + 2.0f, a, b, tRay) <=
                            kCapsuleRadius) {
                            best->health -= kAiDamage;
                            if (best->health <= 0.0f) {
                                log::info("server: AI ship {} downed player {}", ship.id, bestPeer);
                                killPlayer(*best, bestPeer, 0); // AI/environment kill
                            }
                        }
                    }
                }
            }
            continue;
        }

        // Validate pilot / passenger peer refs.
        if (ship.pilot != 0) {
            auto it = m_players.find(ship.pilot);
            if (it == m_players.end() || !it->second || it->second->pilotingShip != ship.id ||
                it->second->shipRole != 1) {
                ship.pilot = 0;
            }
        }
        if (ship.passenger != 0) {
            auto it = m_players.find(ship.passenger);
            if (it == m_players.end() || !it->second || it->second->pilotingShip != ship.id ||
                it->second->shipRole != 2) {
                ship.passenger = 0;
            }
        }

        if (ship.pilot == 0 && ship.passenger == 0) {
            const glm::vec3 g = m_gravity.sample(ship.pos);
            ship.vel += g * 0.15f * kFixedDtServer;
            ship.vel *= std::pow(kShipLinearDamp, kFixedDtServer * 60.0f);
            ship.pos += ship.vel * kFixedDtServer;
            ensureShipBody(ship);
            continue;
        }

        clearShipBody(ship);
        if (ship.pilot != 0) {
            Player& pilot = *m_players[ship.pilot];
            ShipPose pose{ship.pos, ship.vel, ship.yaw, ship.pitch};
            const glm::vec3 g = m_gravity.sample(ship.pos);
            if (ship.groundVehicle)
                integrateRacer(pose, pilot.lastCmd, kFixedDtServer, g);
            else
                integrateShip(pose, pilot.lastCmd, kFixedDtServer, g);
            ship.pos = pose.pos;
            ship.vel = pose.vel;
            ship.yaw = pose.yaw;
            ship.pitch = pose.pitch;
            // Ground vehicles: keep the hull from sinking (simple floor clamp at pad level).
            if (ship.groundVehicle && ship.pos.y < defaultSpawnPos().y) {
                ship.pos.y = defaultSpawnPos().y;
                if (ship.vel.y < 0.0f) ship.vel.y = 0.0f;
            }
            const glm::vec3 seat =
                ship.pos + shipOrientation(ship.yaw, ship.pitch) * ship.seatOffset;
            pilot.controller.setState(seat, ship.vel);
        } else if (ship.passenger != 0) {
            // No pilot: passenger still rides; no thruster input until promoted.
            const glm::vec3 g = m_gravity.sample(ship.pos);
            ship.vel += g * 0.1f * kFixedDtServer;
            ship.vel *= std::pow(kShipLinearDamp, kFixedDtServer * 60.0f);
            ship.pos += ship.vel * kFixedDtServer;
        }
        if (ship.passenger != 0) {
            if (auto it = m_players.find(ship.passenger); it != m_players.end() && it->second) {
                const glm::vec3 seat =
                    ship.pos + shipOrientation(ship.yaw, ship.pitch) * ship.passengerOffset;
                it->second->controller.setState(seat, ship.vel);
            }
        }
    }

    // Remove destroyed ships after the tick (stable erase).
    std::erase_if(m_ships, [](const Ship& s) { return s.health <= 0.0f; });
    (void)transport;
}

bool ServerSim::tryBoardOrLeaveShip(Player& player) {
    if (player.pilotingShip != 0) {
        if (Ship* ship = findShip(player.pilotingShip)) {
            const int wasRole = static_cast<int>(player.shipRole);
            const float side = player.shipRole == 2 ? -1.0f : 1.0f;
            ejectFromShip(player, *ship, side);
            log::info("server: player left ship {} (was role {})", ship->id, wasRole);
        } else {
            player.pilotingShip = 0;
            player.shipRole = 0;
        }
        return true;
    }

    const glm::vec3 feet = player.controller.position();
    Ship* nearest = nullptr;
    float best = 1.0e9f;
    for (Ship& s : m_ships) {
        if (s.ai || s.health <= 0.0f) continue;
        // Boardable if pilot free OR passenger free (multi-seat).
        if (s.pilot != 0 && s.passenger != 0) continue;
        const float reach =
            std::max(kShipBoardRange, glm::length(glm::vec2(s.halfExtents.x, s.halfExtents.z)) + 1.5f);
        const float d = glm::length(s.pos - feet);
        if (d < reach && d < best) {
            best = d;
            nearest = &s;
        }
    }
    if (!nearest) return false;
    PeerId peer = 0;
    for (const auto& [p, pl] : m_players) {
        if (pl.get() == &player) {
            peer = p;
            break;
        }
    }
    if (peer == 0) return false;

    clearShipBody(*nearest);
    player.pilotingShip = nearest->id;
    if (nearest->pilot == 0) {
        nearest->pilot = peer;
        player.shipRole = 1;
        nearest->passengerOffset =
            glm::vec3(nearest->halfExtents.x * 0.55f, nearest->halfExtents.y * 0.2f,
                      -nearest->halfExtents.z * 0.05f);
        const glm::vec3 seat =
            nearest->pos + shipOrientation(nearest->yaw, nearest->pitch) * nearest->seatOffset;
        player.controller.setState(seat, nearest->vel);
        log::info("server: player {} boarded ship {} as PILOT", peer, nearest->id);
    } else {
        nearest->passenger = peer;
        player.shipRole = 2;
        const glm::vec3 seat =
            nearest->pos + shipOrientation(nearest->yaw, nearest->pitch) * nearest->passengerOffset;
        player.controller.setState(seat, nearest->vel);
        log::info("server: player {} boarded ship {} as PASSENGER", peer, nearest->id);
    }
    return true;
}

// Spawn one ItemPickup world entity (shared by dungeon/NPC/death loot). Rides the
// existing WorldEntity snapshot path — no new wire type. Same 4096 entity cap the
// script spawnPickup uses so a runaway drop can never OOM the server.
void ServerSim::spawnPickup(ItemId item, std::uint16_t count, glm::vec3 pos) {
    if (item == 0 || count == 0) return;
    if (m_entities.size() >= 4096) return;
    WorldEntity e;
    e.id = m_nextEntityId++;
    e.type = EntityArchetype::ItemPickup;
    e.pos = pos;
    e.item = item;
    e.count = count;
    m_entities.push_back(e);
}

void ServerSim::spawnDungeonLoot() {
    if (m_rules.terrain == GameRules::Terrain::Void ||
        m_rules.gameTemplate == GameRules::Template::Space) {
        log::info("server: skipping dungeon loot (void/space template)");
        return;
    }
    // Same pure function the terrain generator uses — identical layout for free.
    const DungeonLayout layout = DungeonLayout::generate(m_seed, {});
    std::size_t roomIndex = 0;
    for (const auto& room : layout.rooms()) {
        if (roomIndex >= 16) break;
        const glm::ivec3 c{(room.min.x + room.max.x) / 2, room.min.y,
                           (room.min.z + room.max.z) / 2};
        WorldEntity e;
        e.id = m_nextEntityId++;
        e.type = EntityArchetype::ItemPickup;
        e.pos = (glm::vec3(c) + glm::vec3(0.5f, 0.2f, 0.5f)) * kVoxelSize;
        e.pos.y = static_cast<float>(c.y) * kVoxelSize + 0.3f; // resting on the floor
        if (roomIndex % 3 == 2) {
            e.item = m_defaultItems.medkit;
            e.count = 1;
        } else {
            e.item = m_defaultItems.ammo9mm;
            e.count = 24;
        }
        m_entities.push_back(e);
        ++roomIndex;
    }
    log::info("server: spawned {} loot pickups in dungeon rooms", m_entities.size());
}

bool ServerSim::tryPickup(Transport& transport, PeerId peer, Player& player) {
    const glm::vec3 feet = player.controller.position();
    for (auto it = m_entities.begin(); it != m_entities.end(); ++it) {
        if (it->type != EntityArchetype::ItemPickup) continue;
        const glm::vec3 d = it->pos - (feet + glm::vec3(0, 0.9f, 0));
        if (glm::dot(d, d) > 1.5f * 1.5f) continue;
        const std::uint16_t leftover = player.inventory.add(it->item, it->count, m_items);
        if (leftover == it->count) continue; // bag full for THIS item; try other loot
        if (leftover == 0) {
            m_entities.erase(it); // absent from the next snapshot = despawned
        } else {
            it->count = leftover;
        }
        player.inventory.initMags(m_items); // a newly-looted weapon starts with a full mag
        sendInventory(transport, peer, player);
        return true; // one pickup per press
    }
    return false;
}

void ServerSim::pump(Transport& transport) {
    m_activeTransport = &transport; // script callbacks may broadcast
    std::vector<NetEvent> events;
    transport.poll(events);
    for (NetEvent& e : events) {
        switch (e.type) {
        case NetEvent::Type::Connected: {
            // Refuse when full, before allocating any per-peer state — a
            // connection flood must not be able to exhaust the server.
            if (static_cast<int>(m_players.size()) >= m_netPolicy.maxPlayers) {
                log::info("server: peer {} refused — server full ({}/{})", e.peer,
                          m_players.size(), m_netPolicy.maxPlayers);
                transport.disconnect(e.peer);
                break;
            }
            log::info("server: peer {} connected", e.peer);
            auto player = std::make_unique<Player>();
            // Ordinary player until a Hello proves otherwise, and metered from
            // the moment it connects rather than from its first accepted edit.
            player->permissions.role = PeerRole::Player;
            player->voxelEdits.configure(m_netPolicy.voxelEditsPerSecond,
                                         m_netPolicy.voxelEditsPerSecond * 2.0f);
            player->propEdits.configure(m_netPolicy.propEditsPerSecond,
                                        m_netPolicy.propEditsPerSecond * 2.0f);
            m_players.emplace(e.peer, std::move(player));
            break;
        }
        case NetEvent::Type::Disconnected: {
            log::info("server: peer {} disconnected", e.peer);
            // Free any ship seat this peer held so it doesn't stay locked.
            if (auto it = m_players.find(e.peer); it != m_players.end() && it->second) {
                const std::uint32_t sid = it->second->pilotingShip;
                if (sid != 0) {
                    // Free the seat this peer actually held. Clearing the pilot
                    // unconditionally meant a passenger disconnecting kicked the
                    // pilot out of control until they re-boarded.
                    for (Ship& s : m_ships) {
                        if (s.id != sid)
                            continue;
                        if (s.pilot == e.peer)
                            s.pilot = 0;
                        if (s.passenger == e.peer)
                            s.passenger = 0;
                    }
                }
            }
            m_players.erase(e.peer);
            m_rejectLog.erase(e.peer);       // no state kept for a peer that is gone
            m_clientBaselines.erase(e.peer); // its snapshot ring goes with it
            m_frags.erase(e.peer);           // a reconnect starts from a clean score
            break;
        }
        case NetEvent::Type::Packet:
            handlePacket(transport, e.peer, e.data);
            break;
        }
    }
}

void ServerSim::handlePacket(Transport& transport, PeerId peer,
                             std::span<const std::byte> data) {
    const auto type = peekType(data);
    if (!type) return;
    auto it = m_players.find(peer);
    if (it == m_players.end()) return;
    Player& player = *it->second;
    ByteReader reader(data.subspan(1));

    switch (*type) {
    case MsgType::Hello: {
        HelloMsg hello;
        if (!decode(hello, reader)) return;
        if (player.helloDone) return; // replayed Hello must not re-grant loadout
        // A client built against a different wire format is refused rather than
        // half-understood: fields it does not know about would decode as garbage.
        if (hello.protocol != kProtocolVersion) {
            log::info("server: peer {} refused, protocol {} != {}", peer,
                      hello.protocol, kProtocolVersion);
            transport.disconnect(peer);
            return;
        }
        // Connection auth: a password-protected server admits only peers that
        // present the right password. Checked before helloDone so a rejected
        // peer is never admitted and gains no player. (Access control, not wire
        // secrecy — that needs an encrypted transport.)
        if (!m_netPolicy.serverPassword.empty() &&
            hello.password != m_netPolicy.serverPassword) {
            noteRejected(peer, "join with wrong/absent server password");
            transport.disconnect(peer);
            return;
        }
        player.helloDone = true;
        // The only place a peer's rights are decided. Everything downstream just
        // reads permissions. Comparison is against a per-boot token that reached
        // the owner's client in-process, so possessing it is the proof; an empty
        // configured token can never match because the check requires non-empty.
        if (m_netPolicy.allowRemoteEditing && !m_netPolicy.editorToken.empty() &&
            hello.editorToken == m_netPolicy.editorToken) {
            player.permissions.role = PeerRole::Host;
            log::info("server: peer {} authenticated as {}", peer,
                      toString(player.permissions.role));
        } else if (!hello.editorToken.empty()) {
            // Said out loud: a wrong token is far more interesting than no token.
            log::info("server: peer {} presented an editor token that was not "
                      "accepted; staying {}", peer,
                      toString(player.permissions.role));
        }
        WelcomeMsg welcome{peer, m_seed, m_tick,
                           static_cast<std::uint8_t>(m_rules.inventoryModel),
                           m_rules.flagsByte(),
                           m_rules.voxelSize,
                           static_cast<std::uint8_t>(m_rules.environment)};
        transport.send(peer, pack(welcome), true);
        if (m_pendingRestore) { // save-file state goes to the first arrival
            player.health = m_pendingRestore->health;
            player.inventory = m_pendingRestore->inventory;
            player.inventory.initMags(m_items); // saves don't store mags; load full
            player.spawnOverride = m_pendingRestore->pos; // applied at spawn in tick()
            m_pendingRestore.reset();
        } else {
            giveStartingLoadout(player);
        }
        sendInventory(transport, peer, player);
        sendOverlayTo(transport, peer); // world edits (editor/script/save) → this client
        log::info("server: '{}' joined as player {}", hello.name, peer);
        m_scripts.onPlayerJoin(peer);
        break;
    }
    case MsgType::Command: {
        CommandMsg msg;
        if (!decode(msg, reader)) return;
        // Snapshot ack is independent of the command tick — advance it even for a
        // stale/duplicate command so a resent packet never rolls the baseline back.
        player.ackedSnapshotTick = std::max(player.ackedSnapshotTick, msg.ackSnapshotTick);
        // Strictly-increasing after the first accepted command; hasCmd closes
        // the tick-0 replay window (tick 0 would else re-pass the != 0 guard).
        if (player.hasCmd && msg.cmd.tick <= player.lastCmdTick) return; // stale/replayed
        player.lastCmd = msg.cmd;
        player.lastCmdTick = msg.cmd.tick;
        player.hasCmd = true;
        break;
    }
    // The four world-authoring messages. Each one asks the same three questions
    // in the same order: may this peer edit at all, is the payload sane, and is
    // it inside its allowance. Permission is checked first so that an
    // unauthorised peer cannot even spend a validation cycle.
    case MsgType::VoxelOp: {
        VoxelOpMsg op;
        if (!decode(op, reader)) return;
        if (!player.permissions.canEditVoxels()) {
            noteRejected(peer, "voxel edit without permission");
            return;
        }
        // A hostile block id would hit the registry assert server-side.
        if (!m_voxels.blockRegistry().isValid(op.block)) {
            noteRejected(peer, "voxel edit with invalid block id");
            return;
        }
        // Compare without negating: abs(INT_MIN) overflows back to INT_MIN
        // (negative), which would slip a hostile coordinate past the check.
        if (op.voxel.x < -kMaxEditCoord || op.voxel.x > kMaxEditCoord ||
            op.voxel.y < -kMaxEditCoord || op.voxel.y > kMaxEditCoord ||
            op.voxel.z < -kMaxEditCoord || op.voxel.z > kMaxEditCoord) {
            noteRejected(peer, "voxel edit out of range");
            return;
        }
        if (!player.voxelEdits.consume()) {
            noteRejected(peer, "voxel edit rate limit");
            return;
        }
        applyVoxelOp(transport, op);
        break;
    }
    case MsgType::PlaceProp: {
        PlacePropMsg msg;
        if (!decode(msg, reader)) return;
        if (!player.permissions.canEditProps()) {
            noteRejected(peer, "prop place without permission");
            return;
        }
        // Project-relative assets/ path only, and no traversal out of it.
        if (msg.asset.rfind("assets/", 0) != 0 ||
            msg.asset.find("..") != std::string::npos) {
            noteRejected(peer, "prop place with bad asset path");
            return;
        }
        if (!isSaneTransform(msg.transform)) {
            noteRejected(peer, "prop place with invalid transform");
            return;
        }
        if (m_props.size() >= kMaxProps) {
            noteRejected(peer, "prop place over the world prop cap");
            return;
        }
        if (!player.propEdits.consume()) {
            noteRejected(peer, "prop place rate limit");
            return;
        }
        // addProp rejects a model that won't load, so a bad asset never creates
        // a phantom prop.
        addProp(&transport, msg.asset, msg.transform, 0);
        break;
    }
    case MsgType::MoveProp: {
        MovePropMsg msg;
        if (!decode(msg, reader)) return;
        if (!player.permissions.canEditProps()) {
            noteRejected(peer, "prop move without permission");
            return;
        }
        if (msg.id == 0) return;
        if (!isSaneTransform(msg.transform)) {
            noteRejected(peer, "prop move with invalid transform");
            return;
        }
        if (!player.propEdits.consume()) {
            noteRejected(peer, "prop move rate limit");
            return;
        }
        moveProp(&transport, msg.id, msg.transform);
        break;
    }
    case MsgType::RemoveProp: {
        RemovePropMsg msg;
        if (!decode(msg, reader)) return;
        if (!player.permissions.canEditProps()) {
            noteRejected(peer, "prop remove without permission");
            return;
        }
        if (msg.id == 0) return;
        if (!player.propEdits.consume()) {
            noteRejected(peer, "prop remove rate limit");
            return;
        }
        removeProp(&transport, msg.id);
        break;
    }
    default:
        break;
    }
}

void ServerSim::tick(Transport& transport) {
    m_activeTransport = &transport;
    m_jobs.drainMainThread(); // collider syncs from finished mesh jobs

    glm::vec3 streamCenter = defaultSpawnPos();
    bool first = true;
    for (auto& [peer, player] : m_players) {
        // Edit allowances refill on the fixed tick, so they track simulated
        // seconds and not how fast packets happen to arrive.
        player->voxelEdits.refill(kFixedDtServer);
        player->propEdits.refill(kFixedDtServer);
        if (!player->spawned) {
            if (!player->controller.init(m_physics, player->spawnOverride.value_or(defaultSpawnPos())))
                continue;
            player->spawned = true;
            assignTeam(*player); // TeamDeathmatch: balance onto a team at first spawn
        }
        if (player->pilotingShip == 0) {
            // B3b: sample the field at the feet before integrating so habitat/orbital SOI apply.
            // Local-up: feet plant opposite gravity when |g| is meaningful (planetoid / habitat).
            const glm::vec3 g = m_gravity.sample(player->controller.position());
            player->controller.setGravity(g);
            const float gLen = glm::length(g);
            if (gLen > 0.5f)
                player->controller.setUp(-g / gLen);
            else
                player->controller.setUp(glm::vec3(0.0f, 1.0f, 0.0f));
            player->controller.setSpeedScale(speedMultOf(*player)); // slow/haste effects
            player->controller.update(player->lastCmd, kFixedDtServer, m_physics);
            if (player->controller.position().y < -30.0f) // fell out (colliders pending)
                player->controller.setState(defaultSpawnPos(), glm::vec3(0));
        }
        // Pilots: thrusters run in updateShips after this loop; combat still runs (gunner seat).
        processCombat(transport, peer, *player);
        if (first) { // TODO: multi-center streaming once co-op players roam apart
            streamCenter = player->controller.position();
            first = false;
        }
    }
    updateShips(transport); // H4: thrusters, AI traffic, seat glue
    m_physics.step(kFixedDtServer);
    updateProjectiles(transport);
    updateNpcs(transport);
    updateTurrets(transport);
    updateCompanions(transport);
    m_crowd.step(kFixedDtServer); // Phase 7: ambient boids crowd (no-op when empty)
    m_voxels.update(streamCenter, m_jobs);

    ++m_tick;
    // After the increment, so history is keyed by the tick snapshots stamp:
    // rewinding to an acked snapshot tick reproduces what the shooter saw.
    recordPoseHistory();
    if (m_scripts.loaded() && m_tick % 20 == 0) m_scripts.onTick(m_tick); // ~3 Hz gameplay hook
    if (m_tick % kSnapshotEvery == 0 && !m_players.empty()) broadcastSnapshot(transport);
    // Periodic autosave: a crash (not a graceful quit) otherwise loses everything
    // since the last manual save. Disabled by default (interval 0).
    if (m_autosaveIntervalTicks > 0 && m_tick - m_lastAutosaveTick >= m_autosaveIntervalTicks) {
        m_lastAutosaveTick = m_tick;
        saveTo(m_autosavePath);
    }
}

void ServerSim::recordPoseHistory() {
    for (auto& [peer, player] : m_players) {
        Player::PastPose& slot = player->poseHistory[m_tick % Player::kPoseHistorySize];
        slot.tick = m_tick;
        slot.feet = player->controller.position();
        slot.crouched = player->controller.crouched();
        slot.spawned = player->spawned;
    }
}

bool ServerSim::rewoundPlayerPose(const Player& target, std::uint64_t tick, glm::vec3& feet,
                                  bool& crouched) const {
    const Player::PastPose& e = target.poseHistory[tick % Player::kPoseHistorySize];
    if (e.tick != tick || !e.spawned) return false; // never recorded, or pre-spawn
    feet = e.feet;
    crouched = e.crouched;
    return true;
}

// Rejections are worth seeing, but a peer that floods must not be able to turn
// the log into the denial of service. One line per peer per simulated second;
// everything in between is counted and reported with the next line.
void ServerSim::noteRejected(PeerId peer, const char* what) {
    constexpr std::uint64_t kLogEvery = 60; // ticks == 1 s at the fixed rate
    RejectLog& entry = m_rejectLog[peer];
    if (m_tick - entry.lastTick < kLogEvery && entry.lastTick != 0) {
        ++entry.suppressed;
        return;
    }
    if (entry.suppressed > 0) {
        log::info("server: peer {} rejected: {} (+{} more since the last line)",
                  peer, what, entry.suppressed);
    } else {
        log::info("server: peer {} rejected: {}", peer, what);
    }
    entry.lastTick = m_tick;
    entry.suppressed = 0;
}

void ServerSim::applyVoxelOp(Transport& transport, const VoxelOpMsg& op) {
    m_voxelDamage.erase(op.voxel); // chip damage dies with the block; fresh block = full hp
    m_voxels.setBlock(op.voxel, op.block);
    for (auto& [peer, unused] : m_players) transport.send(peer, pack(op), true);
}

void ServerSim::giveStartingLoadout(Player& player) {
    // Full reference arsenal so every weapon archetype is reachable in the slice.
    // Space ship template: lighter EVA kit (cannons are hull-mounted, not inventory).
    const bool space = m_rules.gameTemplate == GameRules::Template::Space;
    player.inventory.add(m_defaultItems.pistol, 1, m_items);
    if (!space) {
        player.inventory.add(m_defaultItems.apPistol, 1, m_items);
        player.inventory.add(m_defaultItems.hpPistol, 1, m_items);
        player.inventory.add(m_defaultItems.smg, 1, m_items);
        player.inventory.add(m_defaultItems.shotgun, 1, m_items);
        player.inventory.add(m_defaultItems.sniper, 1, m_items);
        player.inventory.add(m_defaultItems.claymore, 2, m_items);
        player.inventory.add(m_defaultItems.turret, 2, m_items);
        player.inventory.add(m_defaultItems.companionBeacon, 2, m_items);
        player.inventory.add(m_defaultItems.shells, 24, m_items);
        player.inventory.add(m_defaultItems.rifleAmmo, 30, m_items);
        player.inventory.add(m_defaultItems.stoneBlock, 32, m_items);
    }
    player.inventory.add(m_defaultItems.rpg, 1, m_items);
    player.inventory.add(m_defaultItems.grenade, space ? 2 : 3, m_items);
    player.inventory.add(m_defaultItems.ammo9mm, space ? 48 : 90, m_items);
    player.inventory.add(m_defaultItems.rockets, space ? 8 : 4, m_items);
    player.inventory.add(m_defaultItems.medkit, 2, m_items);
    player.inventory.add(m_defaultItems.stim, 2, m_items);
    player.inventory.initMags(m_items);
}

void ServerSim::sendOverlayTo(Transport& transport, PeerId peer) const {
    // Clients regenerate terrain from the seed, so any server-side edit (editor
    // brush, script, or a loaded save) must be replayed or the joiner desyncs.
    for (const auto& [cp, edits] : m_voxels.editOverlay()) {
        for (const auto& [index, block] : edits) {
            const int x = index % kChunkSize, z = (index / kChunkSize) % kChunkSize,
                      y = index / (kChunkSize * kChunkSize);
            const glm::ivec3 v(cp.x * kChunkSize + x, cp.y * kChunkSize + y,
                               cp.z * kChunkSize + z);
            transport.send(peer, pack(VoxelOpMsg{v, block}), true);
        }
    }
    // Props are server-authoritative world objects, not regenerated from the seed,
    // so a joiner must be told about every one (same reason as the voxel replay).
    for (const WorldProp& prop : m_props)
        transport.send(peer, pack(PropAddedMsg{prop.id, prop.asset, prop.transform}), true);
    // B3b-net: custom gravity boxes (env defaults are rebuilt from Welcome env).
    transport.send(peer, pack(makeGravityVolumesMsg()), true);
}

void ServerSim::sendInventory(Transport& transport, PeerId peer, const Player& player) const {
    ByteWriter w;
    w.write(static_cast<std::uint8_t>(MsgType::Inventory));
    player.inventory.encode(w);
    transport.send(peer, w.data(), true);
}

void ServerSim::processCombat(Transport& transport, PeerId peer, Player& player) {
    player.fireCooldown -= kFixedDtServer;
    player.placeCooldown -= kFixedDtServer;
    player.useCooldown -= kFixedDtServer;
    player.reloadCooldown -= kFixedDtServer;
    tickModifiers(player, kFixedDtServer); // decay active ApplyModifier buffs
    tickBurns(transport, peer, player, kFixedDtServer); // apply Ignite DoT ticks

    // H4: pilot hardpoints use ship cannon; passenger/EVA use inventory.
    const glm::vec3 eye = combatMuzzle(player);
    const glm::vec3 dir = viewForward(player.lastCmd.yaw, player.lastCmd.pitch);
    const bool aboard = player.pilotingShip != 0;
    const bool isPilot = player.shipRole == 1;
    const int slotIndex = player.lastCmd.selectedSlot % Inventory::kSlots;
    const ItemStack& held = player.inventory.slot(slotIndex);
    const ItemDef& heldDef =
        isPilot && m_defaultItems.shipCannon != 0 ? m_items.get(m_defaultItems.shipCannon)
                                                 : m_items.get(held.id);

    // --- Reload (H3): resolve before firing so a finished reload feeds this tick.
    const bool firePressed = player.lastCmd.fire && !player.prevFire;
    const bool reloadPressed = player.lastCmd.reload && !player.prevReload;
    if (player.reloadingWeapon != 0 && player.reloadCooldown <= 0.0f) {
        const ItemDef& reloadDef = m_items.get(player.reloadingWeapon);
        if (reloadWeaponMag(player.inventory, player.reloadingWeapon, reloadDef,
                            m_rules.finiteAmmo) > 0)
            player.inventoryDirty = true;
        player.reloadingWeapon = 0;
    }
    if (reloadPressed && player.reloadingWeapon == 0 && heldDef.type == ItemType::Weapon &&
        heldDef.magSize > 0) {
        const bool unfull = player.inventory.magOf(held.id) < heldDef.magSize;
        const bool haveReserve = !m_rules.finiteAmmo || heldDef.ammoItem == 0 ||
                                 player.inventory.countOf(heldDef.ammoItem) > 0;
        if (unfull && haveReserve) {
            player.reloadingWeapon = held.id;
            player.reloadCooldown = kReloadSeconds;
        }
    }

    // --- Fire (H2 fire modes + H3 magazines) ------------------------------
    if (heldDef.type == ItemType::Weapon) {
        const bool ready = player.fireCooldown <= 0.0f;
        // Trigger discipline: SemiAuto/Burst need a press EDGE (holding never
        // auto-repeats), Auto repeats on hold — all still capped by fireCooldown.
        const bool wantShot = triggerReleasesShot(heldDef.fireMode, player.lastCmd.fire,
                                                  firePressed, ready, heldDef.burstCount,
                                                  player.burstRemaining);
        if (wantShot) {
            // Magazine weapons draw reserve at reload time, so per shot they only
            // need a loaded round; magless weapons keep the reserve/self-consume path.
            const bool useMag = m_rules.finiteAmmo && heldDef.magSize > 0;
            const bool magOk = !useMag || player.inventory.magOf(held.id) > 0;
            const bool hasAmmo = useMag || !m_rules.finiteAmmo || heldDef.ammoItem == 0 ||
                                 player.inventory.countOf(heldDef.ammoItem) > 0;
            // Thrown/placed weapons (grenade/claymore) consume the weapon item
            // itself; hitscan/rpg consume their ammo item.
            const bool selfConsumed = heldDef.delivery != DeliveryKind::Hitscan &&
                                      heldDef.ammoItem == 0;
            const bool hasCharge = !m_rules.finiteAmmo || !selfConsumed || held.count > 0;
            if (magOk && hasAmmo && hasCharge) {
                const bool bursting =
                    heldDef.fireMode == FireMode::Burst && player.burstRemaining > 0;
                player.fireCooldown = bursting
                                          ? std::min(heldDef.fireInterval, kBurstIntraInterval)
                                          : heldDef.fireInterval;
                if (useMag) {
                    player.inventory.setMag(
                        held.id,
                        static_cast<std::uint16_t>(player.inventory.magOf(held.id) - 1));
                    player.inventoryDirty = true;
                } else if (m_rules.finiteAmmo && heldDef.ammoItem != 0) {
                    player.inventory.remove(heldDef.ammoItem, 1);
                    player.inventoryDirty = true;
                } else if (m_rules.finiteAmmo && selfConsumed) {
                    player.inventory.remove(held.id, 1);
                    player.inventoryDirty = true;
                }

                // Pilot: hardpoint is the muzzle. Passenger/foot: slight eye offset.
                const glm::vec3 muzzle = isPilot ? eye : (eye + dir * 0.6f);
                if (heldDef.delivery == DeliveryKind::Hitscan) {
                    fireHitscan(transport, peer, player, heldDef);
                    if (isPilot) player.shipHardpoint ^= 1;
                } else if (heldDef.delivery == DeliveryKind::Projectile) {
                    const glm::vec3 vel = dir * heldDef.projectileSpeed;
                    spawnProjectile(peer, muzzle, vel, heldDef);
                    if (isPilot) player.shipHardpoint ^= 1;
                } else if (heldDef.delivery == DeliveryKind::Deployable && !aboard) {
                    // Deployables stay EVA/on-foot only (no dropping mines from the cockpit).
                    glm::vec3 at = eye + dir * 2.5f;
                    if (const auto hit = m_voxels.raycast(eye, dir, 3.0f))
                        at = eye + dir * std::max(0.5f, hit->t - 0.2f);
                    if (heldDef.deploysTurret) {
                        spawnTurret(peer, at);
                        log::info("server: player {} placed a turret", peer);
                    } else if (heldDef.deploysCompanion) {
                        spawnCompanion(peer, at);
                        log::info("server: player {} summoned a companion", peer);
                    } else {
                        Deployable dep;
                        dep.id = m_nextEntityId++;
                        dep.owner = peer;
                        dep.pos = at;
                        dep.radius = heldDef.blastRadius;
                        dep.damage = heldDef.blastDamage;
                        dep.onTrigger =
                            heldDef.effects.empty()
                                ? EffectList{areaDamageEffect(dep.damage, dep.radius)}
                                : heldDef.effects;
                        m_deployables.push_back(std::move(dep));
                    }
                }
            }
        }
    } else if (!aboard && heldDef.type == ItemType::Block && player.lastCmd.fire &&
               player.fireCooldown <= 0.0f) {
        // Holding a block: LMB is the mining tool (held-repeat, no fire-mode discipline).
        player.fireCooldown = kPlaceInterval;
        if (const auto hit = m_voxels.raycast(eye, dir, 6.0f); hit && hit->block != 0) {
            applyVoxelOp(transport, {hit->voxel, 0});
            if (m_rules.minedBlockDrops) {
                player.inventory.add(m_defaultItems.stoneBlock, 1, m_items);
                player.inventoryDirty = true;
            }
        }
    }
    // Remember this tick's trigger states so next tick can detect the press edge.
    player.prevFire = player.lastCmd.fire;
    player.prevReload = player.lastCmd.reload;

    if (!aboard && player.lastCmd.place && player.placeCooldown <= 0.0f &&
        heldDef.type == ItemType::Block) {
        const bool hasBlocks = !m_rules.minedBlockDrops || held.count > 0;
        if (hasBlocks) {
            player.placeCooldown = kPlaceInterval;
            if (const auto hit = m_voxels.raycast(eye, dir, 8.0f);
                hit && hit->normal != glm::ivec3(0)) {
                const glm::ivec3 target = hit->voxel + hit->normal;
                // Don't entomb anyone: reject placement overlapping a player capsule.
                const glm::vec3 center = (glm::vec3(target) + 0.5f) * kVoxelSize;
                bool blocked = false;
                for (auto& [otherPeer, other] : m_players) {
                    if (!other->spawned) continue;
                    const glm::vec3 feet = other->controller.position();
                    if (glm::abs(center.x - feet.x) < 0.6f &&
                        glm::abs(center.z - feet.z) < 0.6f && center.y > feet.y - 0.5f &&
                        center.y < feet.y + 2.0f) {
                        blocked = true;
                        break;
                    }
                }
                if (!blocked && m_voxels.blockAt(target) == 0) {
                    applyVoxelOp(transport, {target, heldDef.blockId});
                    if (m_rules.minedBlockDrops) {
                        player.inventory.remove(held.id, 1);
                        player.inventoryDirty = true;
                    }
                }
            }
        }
    }

    // H4: Use edge boards/leaves a ship before loot/consumable so EVA is reliable.
    const bool usePressed = player.lastCmd.use && !player.prevUse;
    if (usePressed && player.useCooldown <= 0.0f) {
        player.useCooldown = 0.5f;
        if (!tryBoardOrLeaveShip(player)) {
            const bool grabbed = tryPickup(transport, peer, player); // loot wins over consuming
            if (!grabbed && heldDef.type == ItemType::Consumable && !heldDef.effects.empty()) {
                // Don't waste a pure-heal item at full health; a buff (or any non-heal
                // effect) is always worth applying, so a stim works even topped off.
                bool healOnly = true;
                for (const Effect& e : heldDef.effects)
                    if (e.kind != EffectKind::Heal) healOnly = false;
                if (!healOnly || player.health < 100.0f) {
                    runEffects(transport, heldDef.effects, peer,
                               player.controller.position(), &player, nullptr);
                    player.inventory.remove(held.id, 1);
                    player.inventoryDirty = true;
                }
            }
        }
    }
    player.prevUse = player.lastCmd.use;

    if (player.inventoryDirty) {
        sendInventory(transport, peer, player);
        player.inventoryDirty = false;
    }
}

bool ServerSim::saveTo(const std::string& path) const {
    nlohmann::json j;
    j["version"] = kSaveVersion;
    j["seed"] = m_seed;
    j["tick"] = m_tick;

    nlohmann::json chunks = nlohmann::json::array();
    for (const auto& [pos, edits] : m_voxels.editOverlay()) {
        nlohmann::json entry;
        entry["pos"] = {pos.x, pos.y, pos.z};
        nlohmann::json cells = nlohmann::json::array();
        for (const auto& [index, block] : edits) cells.push_back({index, block});
        entry["cells"] = std::move(cells);
        chunks.push_back(std::move(entry));
    }
    j["chunks"] = std::move(chunks);

    nlohmann::json players = nlohmann::json::array();
    for (const auto& [peer, player] : m_players) {
        if (!player->spawned) continue;
        const glm::vec3 p = player->controller.position();
        nlohmann::json entry;
        entry["pos"] = {p.x, p.y, p.z};
        entry["health"] = player->health;
        nlohmann::json inv = nlohmann::json::array();
        for (int i = 0; i < Inventory::kSlots; ++i)
            inv.push_back({player->inventory.slot(i).id, player->inventory.slot(i).count});
        entry["inventory"] = std::move(inv);
        players.push_back(std::move(entry));
    }
    j["players"] = std::move(players);

    nlohmann::json props = nlohmann::json::array();
    for (const WorldProp& prop : m_props) {
        nlohmann::json t = nlohmann::json::array();
        const float* m = glm::value_ptr(prop.transform);
        for (int i = 0; i < 16; ++i) t.push_back(m[i]);
        props.push_back({{"id", prop.id}, {"asset", prop.asset}, {"transform", std::move(t)}});
    }
    j["props"] = std::move(props);

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path);
    if (!out) {
        log::error("save: cannot open '{}'", path);
        return false;
    }
    out << j.dump();
    out.flush();
    if (!out.good()) { // disk full / write error must not report success
        log::error("save: write to '{}' failed", path);
        return false;
    }
    log::info("saved to '{}' ({} edited chunks, {} players)", path, j["chunks"].size(),
              j["players"].size());
    return true;
}

bool ServerSim::initFromSave(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        log::error("load: cannot open '{}'", path);
        return false;
    }
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.contains("seed")) {
        log::error("load: '{}' is not a valid save", path);
        return false;
    }
    // A versionless file predates versioning (v0); a version we don't recognise
    // is from a newer engine whose layout we can't safely parse — refuse it
    // rather than silently misread fields.
    const int version = j.value("version", 0);
    if (version > kSaveVersion) {
        log::error("load: '{}' is save version {} but this build understands up to {}", path,
                   version, kSaveVersion);
        return false;
    }
    if (!init(j["seed"].get<std::uint32_t>())) return false;
    m_tick = j.value("tick", std::uint64_t{0});

    // nlohmann throws on structural mismatch; a truncated or hand-edited save
    // must not crash the server, so the replay is exception-bounded.
    try {
        loadSaveBody(j);
    } catch (const nlohmann::json::exception& e) {
        log::error("load: '{}' is structurally invalid ({}) — starting fresh", path, e.what());
        m_pendingRestore.reset();
    }
    log::info("loaded '{}' (seed {}, {} edited chunks)", path, m_seed,
              j.value("chunks", nlohmann::json::array()).size());
    return true;
}

void ServerSim::loadSaveBody(const nlohmann::json& j) {
    for (const auto& entry : j.value("chunks", nlohmann::json::array())) {
        const auto& p = entry["pos"];
        const ChunkPos cp{p[0].get<int>(), p[1].get<int>(), p[2].get<int>()};
        for (const auto& cell : entry["cells"]) {
            const auto index = cell[0].get<std::uint16_t>();
            const int x = index % kChunkSize, z = (index / kChunkSize) % kChunkSize,
                      y = index / (kChunkSize * kChunkSize);
            m_voxels.setBlock(glm::ivec3(cp.x * kChunkSize + x, cp.y * kChunkSize + y,
                                         cp.z * kChunkSize + z),
                              cell[1].get<BlockId>());
        }
    }

    // Props: recreate the collider now (no transport → no broadcast; the join
    // replay in sendOverlayTo resends them to every client, editor or gameplay).
    for (const auto& entry : j.value("props", nlohmann::json::array())) {
        const std::string asset = entry.value("asset", std::string{});
        if (asset.empty()) continue;
        glm::mat4 transform(1.0f);
        if (const auto& t = entry["transform"]; t.is_array() && t.size() == 16) {
            float m[16];
            for (int i = 0; i < 16; ++i) m[i] = t[i].get<float>();
            transform = glm::make_mat4(m);
        }
        addProp(nullptr, asset, transform, entry.value("id", std::uint32_t{0}));
    }

    const auto players = j.value("players", nlohmann::json::array());
    if (!players.empty()) { // restored state goes to the first player who joins
        const auto& entry = players[0];
        SavedPlayer restored;
        restored.pos = {entry["pos"][0].get<float>(), entry["pos"][1].get<float>(),
                        entry["pos"][2].get<float>()};
        restored.health = entry.value("health", 100.0f);
        const auto inv = entry.value("inventory", nlohmann::json::array());
        for (int i = 0; i < Inventory::kSlots && i < static_cast<int>(inv.size()); ++i)
            restored.inventory.slot(i) = {inv[i][0].get<ItemId>(), inv[i][1].get<std::uint16_t>()};
        m_pendingRestore = restored;
    }
}

void ServerSim::broadcastSnapshot(Transport& transport) {
    SnapshotMsg snap;
    snap.tick = m_tick;
    for (auto& [peer, player] : m_players) {
        if (!player->spawned) continue;
        PlayerState ps;
        ps.playerId = peer;
        ps.pos = player->controller.position();
        ps.vel = player->controller.velocity();
        ps.yaw = player->lastCmd.yaw;
        ps.pitch = player->lastCmd.pitch;
        ps.onGround = player->controller.onGround();
        ps.crouched = player->controller.crouched();
        ps.health = player->health;
        ps.vehicleId = player->pilotingShip;
        ps.vehicleRole = player->shipRole;
        snap.players.push_back(ps);
    }
    for (const Ship& s : m_ships) {
        if (snap.entities.size() >= kMaxSnapshotEntities) break;
        // anim packs hull variant + occupied bit (see ShipHulls.h).
        if (s.health <= 0.0f) continue;
        snap.entities.push_back({s.id, static_cast<std::uint8_t>(EntityArchetype::Ship), s.pos,
                                 s.yaw, packShipAnim(s.pilot != 0, s.ai, s.hullVariant), s.health,
                                 packShipPitch(s.pitch)});
    }
    // Threats first: if the entity cap ever bites, invisible-but-lethal NPCs and
    // rockets are far worse than an unrendered ammo box.
    for (const Npc& n : m_npcs) {
        if (snap.entities.size() >= kMaxSnapshotEntities) break;
        const auto anim = static_cast<std::uint8_t>(glm::clamp(n.animSpeed, 0.0f, 1.0f) * 255.0f);
        snap.entities.push_back({n.id, static_cast<std::uint8_t>(n.type), n.pos, n.yaw, anim,
                                 n.health, 0});
    }
    for (const Turret& t : m_turrets) {
        if (snap.entities.size() >= kMaxSnapshotEntities) break;
        snap.entities.push_back({t.id, static_cast<std::uint8_t>(EntityArchetype::Turret),
                                 t.pos, t.yaw, 0, t.health, 0});
    }
    for (const Companion& c : m_companions) {
        if (snap.entities.size() >= kMaxSnapshotEntities) break;
        snap.entities.push_back({c.id, static_cast<std::uint8_t>(EntityArchetype::Companion),
                                 c.pos, c.yaw, 0, c.health, 0});
    }
    { // Phase 7: ambient crowd agents (stable ids from the reserved block).
        const auto& agents = m_crowd.agents();
        for (std::size_t i = 0; i < agents.size(); ++i) {
            if (snap.entities.size() >= kMaxSnapshotEntities) break;
            const glm::vec3 v = agents[i].vel;
            const float yaw = (v.x * v.x + v.z * v.z) > 1e-6f ? std::atan2(-v.x, -v.z) : 0.0f;
            snap.entities.push_back({m_crowdBaseId + static_cast<std::uint32_t>(i),
                                     static_cast<std::uint8_t>(EntityArchetype::Crowd),
                                     agents[i].pos, yaw, 0, 0.0f, 0});
        }
    }
    for (const Projectile& p : m_projectiles) {
        if (snap.entities.size() >= kMaxSnapshotEntities) break;
        snap.entities.push_back({p.id, static_cast<std::uint8_t>(EntityArchetype::Projectile),
                                 p.pos, 0, 0, 0.0f, 0});
    }
    for (const Deployable& d : m_deployables) {
        if (snap.entities.size() >= kMaxSnapshotEntities) break;
        snap.entities.push_back({d.id, static_cast<std::uint8_t>(EntityArchetype::Deployable),
                                 d.pos, 0, 0, 0.0f, 0});
    }
    for (const WorldEntity& e : m_entities) {
        if (snap.entities.size() >= kMaxSnapshotEntities) break;
        snap.entities.push_back({e.id, static_cast<std::uint8_t>(e.type), e.pos, e.yaw, 0,
                                 0.0f, e.item});
    }
    // Each client gets an INTEREST-SCOPED view: all players (always relevant —
    // you must see who can shoot you) plus only the entities near its own player
    // when interestRadius > 0. Its delta baseline is its own last-acked view (not
    // a shared one), because scoping can hand two clients different entity sets.
    static const SnapshotMsg kEmptyBaseline{}; // tick 0, no records => keyframe
    const float radius = m_rules.interestRadius;
    const float radius2 = radius * radius;
    for (auto& [peer, player] : m_players) {
        SnapshotMsg view;
        view.tick = snap.tick;
        view.lastCmdTick = player->lastCmdTick; // per-recipient ack of their own input
        view.players = snap.players;            // players are never scoped out
        if (radius <= 0.0f) {
            view.entities = snap.entities; // disabled: every entity, historical behaviour
        } else {
            const glm::vec3 center = player->controller.position();
            for (const EntityState& e : snap.entities) {
                const glm::vec3 d = e.pos - center;
                if (glm::dot(d, d) <= radius2) view.entities.push_back(e);
            }
        }

        auto& ring = m_clientBaselines[peer];
        const SnapshotMsg* base = &kEmptyBaseline;
        if (player->ackedSnapshotTick != 0) {
            const auto it = ring.find(player->ackedSnapshotTick);
            if (it != ring.end()) base = &it->second;
        }
        ByteWriter w;
        w.write(static_cast<std::uint8_t>(MsgType::DeltaSnapshot));
        encodeDelta(view, *base, w);
        transport.send(peer, std::move(w).take(), false); // still unreliable

        // Remember exactly what this client saw, for its next delta baseline.
        ring[view.tick] = std::move(view);
        while (ring.size() > 32) ring.erase(ring.begin());
    }
}

} // namespace meat
