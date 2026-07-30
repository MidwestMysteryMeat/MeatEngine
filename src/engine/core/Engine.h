#pragma once
#include "engine/core/EditorHost.h"
#include "engine/core/EntityRegistry.h"
#include "engine/core/EventBus.h"
#include "engine/core/JobQueue.h"
#include "engine/core/TickRate.h"
#include "engine/asset/SkeletalModel.h"
#include "engine/audio/AudioEngine.h"
#include "engine/net/EnetTransport.h"
#include "engine/net/LanDiscovery.h"
#include "engine/net/LoopbackTransport.h"
#include "engine/physics/CharacterController.h"
#include "engine/physics/PhysicsWorld.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/render/Renderer.h"
#include "engine/voxel/VoxelWorld.h"
#include "game/Client.h"
#include "game/GameRules.h"
#include "game/ServerSim.h"
#include "game/WorldGen.h"

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
    std::string master;     // --master host[:port] — announce/browse internet list
    GameRules rules;        // from a project's game.json (or defaults)
    std::string autoShot;   // --shot <png>: capture after a few seconds, then quit
    std::string projectDir; // --project <dir>: load a game (game.json + scripts/)
    bool startEditor = false; // --editor: enter the Room Designer on spawn
    bool animBooth = false;   // --animshot: lock a fixed close camera on the anim actor
};

// Composition root. The simulation authority is always a ServerSim; this class
// wires one up (in-process or remote) and runs the client loop against it.
// Construction order = ownership diagram; destruction is the reverse.
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
    int runDedicated(const EngineConfig& config);
    void simulateClientTick(const PlayerCommand& frameCmd);
    void render(float alpha);
    void drawInventoryUi();

    Window m_window;
    Input m_input;
    JobQueue m_jobs;
    Renderer m_renderer;
    AudioEngine m_audio;
    PhysicsWorld m_physics;   // client mirror
    VoxelWorld m_voxels;      // client mirror
    EntityRegistry m_entities;
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
    };
    std::unique_ptr<AnimActor> m_animActor;
    void loadAnimTestActor();
    bool m_animBooth = false; // fixed camera framing the anim actor (VLM capture)
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
    void saveEditorExtras() const;
    void loadEditorExtras();
};

} // namespace meat
