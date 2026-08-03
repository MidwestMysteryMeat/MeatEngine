#pragma once
#include "engine/core/EditorHost.h"
#include "engine/core/EventBus.h"
#include "engine/core/JobQueue.h"
#include "engine/core/TickRate.h"
#include "engine/anim/FootIk.h"
#include "engine/asset/SkeletalModel.h"
#include "engine/audio/AudioEngine.h"
#include "engine/net/EnetTransport.h"
#include "engine/net/LanDiscovery.h"
#include "engine/net/LoopbackTransport.h"
#include "engine/level/MeshLevel.h"
#include "engine/physics/CharacterController.h"
#include "engine/physics/GravityField.h"
#include "engine/physics/PhysicsWorld.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/render/Renderer.h"
#include "engine/voxel/VoxelWorld.h"
#include "game/Client.h"
#include "game/GameRules.h"
#include "game/ServerSim.h"
#include "game/WorldGen.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace meat {

struct EngineConfig {
    enum class Mode { Browse, Game, Host, Join, Dedicated };
    Mode mode = Mode::Browse; // no CLI mode → server-browser menu
    std::string address = "127.0.0.1"; // Join target
    std::uint16_t port = 26000;
    std::uint32_t seed = 1337;
    std::string loadPath;   // --load <file>: start the server from a save
    std::string serverName = "MeatEngine Server";
    std::string serverPassword; // --password: gate who may join (empty = open)
    int maxPlayers = 64;        // --maxplayers: concurrent-player cap
    std::string master;     // --master host[:port] — announce/browse internet list
    GameRules rules;        // from a project's game.json (or defaults)
    std::string autoShot;   // --shot <png>: capture after a few seconds, then quit
    std::string projectDir; // --project <dir>: load a game (game.json + scripts/)
    bool startEditor = false; // --editor: enter the Room Designer on spawn
    bool animBooth = false;   // --animshot: lock a fixed close camera on the anim actor
    std::string animModel;    // --animmodel <path>: booth-load ANY skeletal file (FBX/glTF)
    std::string animClip;     // --animclip <path>: merge an animation-only file's clips
                              // onto the loaded model by bone name (identical-bind Mixamo)
    std::string animRetarget; // --animretarget <path>: bake a foreign-skeleton clip onto
                              // the model (rest-pose-compensated; UE5/MoCap different bind)
    // B2 MeshLevel: optional static mesh world (triangle colliders). Empty = voxel-only.
    // Prefer meshLevelDesc when non-empty; meshLevelAsset is the single-mesh shortcut.
    std::string meshLevelAsset;
    float meshLevelScale = 1.0f;
    MeshLevelDesc meshLevelDesc;
};

// Composition root. The simulation authority is always a ServerSim; this class
// wires one up (in-process or remote) and runs the client loop against it.
// Construction order = ownership diagram; destruction is the reverse.
//
// Lives in game/, not engine/, on purpose: it depends on ServerSim, Client,
// GameRules and WorldGen, so it is application code that composes the reusable
// engine/ subsystems with this game's logic. Keeping it here is what lets
// engine/ stay free of any game/ include (the layering only points downward).
class Engine {
public:
    int run(const EngineConfig& config);
    void setEditor(std::unique_ptr<IEditor> editor) { m_editor = std::move(editor); }

private:
    bool initClientSystems();
    bool runMenu(EngineConfig& config); // Browse mode; false = user quit
    bool initNetwork(const EngineConfig& config);
    void startHosting(const EngineConfig& config); // beacon + master heartbeat
    void stopHosting();
    void setupClientWorld();                    // after Welcome: seed-matched mirror
    void applyEnvironment(const GameRules& rules); // world preset → gravity + fog + ambient
    void rebuildClientGravityField(const GameRules& rules);
    int runDedicated(const EngineConfig& config);
    GameRules::Perspective m_perspective = GameRules::Perspective::First;
    bool m_hemisphereAmbient = true; // A3 toggle (F7); applied with environment
    void simulateClientTick(const PlayerCommand& frameCmd);
    void render(float alpha);
    void drawInventoryUi();
    // B4: rebuild host world (terrain + environment + game template + seed). Host/SP only.
    bool rebuildWorld(GameRules::Terrain terrain, GameRules::Environment environment,
                      GameRules::Template gameTemplate, std::uint32_t seed);

    Window m_window;
    Input m_input;
    Renderer m_renderer;
    AudioEngine m_audio;
    PhysicsWorld m_physics;   // client mirror
    GravityField m_gravity;   // client prediction mirror of server field (B3b)
    MeshLevelRuntime m_meshLevel; // B2 client render + prediction colliders
    MeshLevelDesc m_meshLevelDesc;
    VoxelWorld m_voxels;      // client mirror
    // Declared AFTER everything its jobs capture (VoxelWorld above especially):
    // members destroy in reverse order, so the queue joins its workers before
    // the data they hold references to is freed.
    JobQueue m_jobs;
    EventBus m_events;
    CharacterController m_player; // client prediction body

    std::unique_ptr<ServerSim> m_server;             // Game/Host/Dedicated
    std::unique_ptr<LoopbackPair> m_loopback;        // Game
    std::unique_ptr<EnetServerTransport> m_enetHost; // Host/Dedicated
    std::unique_ptr<EnetClientTransport> m_enetJoin; // Host/Join
    Transport* m_serverTransport = nullptr;
    Transport* m_clientTransport = nullptr;
    Client m_client;
    ItemRegistry m_items; // client-side names/types; ids match the server's
    bool m_clientWorldReady = false;

    std::unordered_map<ChunkPos, MeshHandle> m_chunkMeshes;
    MeshHandle m_remotePlayerMesh = 0; // box proxy until character meshes land
    MeshHandle m_pickupMesh = 0;       // small bobbing cube for item pickups
    MeshHandle m_shipMesh = 0;         // fallback box if no FBX staged
    MaterialHandle m_shipMaterial{0};
    // A6 blob shadow disc under feet. B3 water is renderer PsxOptions (not a mesh).
    MeshHandle m_blobMesh = 0;
    MaterialHandle m_blobMaterial{0};
    bool m_profilerOpen = false; // C8 lite (F3)
    // H4: per-hull GPU cache (cyber / star / lowpoly). mesh==0 means load failed.
    struct ShipHullGpu {
        MeshHandle mesh = 0;
        MaterialHandle material{0};
        glm::vec3 halfExtents{1.0f};
        bool attempted = false;
    };
    std::array<ShipHullGpu, 8> m_shipHulls{};
    const ShipHullGpu& shipHullGpu(int variant);
    struct PropInstance {
        MeshHandle mesh = 0;
        MaterialHandle material{0};
        glm::mat4 transform{1.0f};
    };
    std::vector<PropInstance> m_props; // static models placed in the world
    void loadWorldProps();
    // Phase 7b proof: one skinned actor near spawn looping clip 0. Staged from
    // the optional (gitignored) assets/models/anim_test.{fbx,glb}; null otherwise.
    struct AnimActor {
        SkinnedMeshHandle mesh = 0;
        MaterialHandle material{0};
        SkeletalModel model; // kept for clip sampling every frame
        glm::mat4 transform{1.0f};
        float time = 0.0f;
        bool hasRealClip = false; // false → drive the procedural idle (exact-matrix)
        int clipIndex = 0;        // which clip to play (the longest — a merged locomotion
                                  // clip beats a bundled 1-frame reference pose)
    };
    std::unique_ptr<AnimActor> m_animActor;
    void loadAnimTestActor();
    // Shared skinned character used to render every humanoid NPC / companion instead of a
    // box proxy. Its `transform` holds ONLY the normalize+orient (no world placement); the
    // render composes translate(entity.pos) * rotY(entity.yaw) per instance, and `time`
    // advances globally (each instance samples at a per-id phase so they aren't lock-stepped).
    std::unique_ptr<AnimActor> m_npcActor;
    void loadNpcActor();
    int m_npcIdleClip = -1; // clip indices in m_npcActor->model for the idle↔walk blend
    int m_npcWalkClip = -1; // (-1 = not present → fall back to the single-clip path)
    // Zombie clips live on the SAME m_npcActor model (one mesh, extra clips); NpcZombie
    // entities blend these instead of the regular locomotion. -1 → box/regular fallback.
    int m_zombieIdleClip = -1;
    int m_zombieWalkClip = -1;
    int m_pistolIdleClip = -1; // armed-shooter locomotion (pistol at the ready); same shared mesh
    int m_pistolWalkClip = -1;
    MaterialHandle m_zombieMaterial{0}; // green-tinted + faintly emissive variant of the NPC
                                        // material so zombies read as zombies (and stay visible
                                        // in the dark) using the same shared mesh.
    // Per-clip lowest-vertex height (in grounded model space) sampled over the clip cycle. The
    // render drops each pose by its current lowest point so feet stay planted instead of the whole
    // body floating on the walk cycle's airborne frames ("mid air"). Precomputed once at load.
    static constexpr int kFootCurveSamples = 32;
    std::unordered_map<int, std::array<float, kFootCurveSamples>> m_clipFootCurve;
    // Yaw correction so the rig faces its movement/target direction. Derived deterministically at
    // load from the bind-pose foot->toe direction (feet point forward), not guessed from shots.
    float m_humanoidYawOffset = 0.0f;
    FootIkRig m_footIkRig; // two-bone foot IK leg chains (planted feet on terrain); valid if legs found
    // Previous entity positions (client-side), to derive each humanoid's speed for the
    // idle↔walk blend weight without a server-side anim-state byte.
    std::unordered_map<std::uint32_t, glm::vec3> m_entityPrevPos;
    // E1: per-entity locomotion phase (seconds). Advanced by walk weight × rate so
    // walk clips don't skate when the blend is full but feet move at world speed.
    std::unordered_map<std::uint32_t, float> m_entityAnimPhase;
    // Per-remote-player audio pacing: derive each remote's speed from its
    // interpolated position (client-side, no net change) to pace positional
    // footstep SFX. Keyed by PeerId; grows with players seen (bounded, like above).
    struct RemoteAudioState {
        glm::vec3 prevPos{0.0f};
        float stepTimer = 0.0f;
        bool seen = false;
    };
    std::unordered_map<std::uint32_t, RemoteAudioState> m_remoteAudio;
    bool m_animBooth = false; // fixed camera framing the anim actor (VLM capture)
    std::string m_animModel;  // --animmodel override path (else the default proof asset)
    std::string m_animClip;   // --animclip: animation file merged onto the model by bone name
    std::string m_animRetarget; // --animretarget: foreign-skeleton clip baked onto the model
    TextureHandle m_atlasTexture = 0; // fallback albedo when a model ships none
    float m_frameDt = 0.0f;           // last frame's dt; advances the proof actor
    PlayerCommand m_lastCmd{};
    std::uint64_t m_tick = 0;
    glm::vec3 m_prevPlayerPos{0};
    glm::vec3 m_currPlayerPos{0};
    float m_localFireCooldown = 0.0f; // cosmetic mirror of the server's cooldown
    float m_muzzleFlash = 0.0f;
    float m_footstepTimer = 0.0f;     // paces footstep SFX while moving on ground
    float m_prevHealth = 100.0f;      // detect damage for hit SFX
    std::size_t m_prevInvHash = 0;    // detect inventory change for pickup SFX
    std::uint8_t m_selectedSlot = 0; // hotbar index, sent in every command
    bool m_showBackpack = false;     // Tab
    bool m_imguiReady = false;
    // H1 Racer: client-side lap timer (finish line = pad Z plane, |X| corridor).
    float m_racerLapTime = 0.0f;
    float m_racerBestLap = 0.0f;
    int m_racerLaps = 0;
    bool m_racerBehindLine = true;

    LanBeacon m_beacon;                 // hosting: LAN presence
    std::thread m_masterHeartbeat;      // hosting: --master announce loop
    std::atomic<bool> m_stopHeartbeat{false};
    char m_menuName[64] = "MeatEngine Server";
    char m_menuAddr[64] = "127.0.0.1:26000";
    char m_menuMaster[128] = "";
    std::vector<ServerAd> m_internetServers; // last master refresh

    std::unique_ptr<IEditor> m_editor; // injected by main.cpp; may be null
    bool m_editorActive = false;
    Camera m_editorCamera;
    std::vector<EditorLight> m_editorLights;    // rendered every frame, saved as extras
    std::vector<SeedVolume> m_seedVolumes;      // consumed by dungeon gen later
    std::vector<EditorGravityVolume> m_gravityVolumes; // B3b-e editor habitat boxes
    std::vector<EditorProp> m_editorProps;      // placed mesh props, rendered + saved as extras
    void applyEditorGravityVolumes(); // push volumes into client (+ host server) field
    // Loaded-mesh cache for editor props, keyed by assetPath so repeated props /
    // per-frame re-renders don't reload from disk. A cached entry with mesh==0
    // marks a missing/failed load, so we don't retry (or spam logs) every frame.
    struct EditorPropMesh {
        MeshHandle mesh = 0;
        MaterialHandle material{0};
        glm::vec3 boundsMin{0.0f}, boundsMax{0.0f}; // local AABB, sizes the client box collider
    };
    std::unordered_map<std::string, EditorPropMesh> m_editorPropCache;
    const EditorPropMesh& editorPropMesh(const std::string& assetPath);
    // Client-mirror static box colliders for synced props, keyed by world-prop id,
    // so local prediction stops at a prop exactly where the server does.
    std::unordered_map<std::uint32_t, PhysicsWorld::BodyHandle> m_propBodies;
    // Drain server prop add/remove reports (m_client) into m_editorProps + colliders.
    void syncWorldProps();
    void saveEditorExtras() const;
    void loadEditorExtras();
};

} // namespace meat
