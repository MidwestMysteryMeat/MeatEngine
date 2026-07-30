#pragma once
#include "engine/core/EntityRegistry.h"
#include "engine/core/EventBus.h"
#include "engine/core/JobQueue.h"
#include "engine/core/TickRate.h"
#include "engine/net/EnetTransport.h"
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

#include <memory>
#include <string>
#include <unordered_map>

namespace meat {

struct EngineConfig {
    enum class Mode { Game, Host, Join, Dedicated };
    Mode mode = Mode::Game;
    std::string address = "127.0.0.1"; // Join target
    std::uint16_t port = 26000;
    std::uint32_t seed = 1337;
};

// Composition root. The simulation authority is always a ServerSim; this class
// wires one up (in-process or remote) and runs the client loop against it.
// Construction order = ownership diagram; destruction is the reverse.
class Engine {
public:
    int run(const EngineConfig& config);

private:
    bool initClientSystems();
    bool initNetwork(const EngineConfig& config);
    void setupClientWorld();                    // after Welcome: seed-matched mirror
    int runDedicated(const EngineConfig& config);
    void simulateClientTick(const PlayerCommand& frameCmd);
    void render(float alpha);

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
    bool m_clientWorldReady = false;

    std::unordered_map<ChunkPos, MeshHandle> m_chunkMeshes;
    MeshHandle m_remotePlayerMesh = 0; // box proxy until character meshes land
    PlayerCommand m_lastCmd{};
    std::uint64_t m_tick = 0;
    glm::vec3 m_prevPlayerPos{0};
    glm::vec3 m_currPlayerPos{0};
    float m_localFireCooldown = 0.0f; // cosmetic mirror of the server's cooldown
    float m_muzzleFlash = 0.0f;
    bool m_imguiReady = false;
};

} // namespace meat
