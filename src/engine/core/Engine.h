#pragma once
#include "engine/core/EditorHost.h"
#include "engine/core/EntityRegistry.h"
#include "engine/core/EventBus.h"
#include "engine/core/JobQueue.h"
#include "engine/core/TickRate.h"
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
    PlayerCommand m_lastCmd{};
    std::uint64_t m_tick = 0;
    glm::vec3 m_prevPlayerPos{0};
    glm::vec3 m_currPlayerPos{0};
    float m_localFireCooldown = 0.0f; // cosmetic mirror of the server's cooldown
    float m_muzzleFlash = 0.0f;
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
