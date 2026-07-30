#include "engine/core/Engine.h"
#include "engine/core/Log.h"

#include <GLFW/glfw3.h>
#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

namespace meat {
namespace {
constexpr float kFallResetY = -30.0f; // below any terrain: teleport back to spawn
constexpr glm::vec3 kClientSpawn{8.0f, 8.0f, 8.0f};
} // namespace

bool Engine::initClientSystems() {
    if (!m_window.init({})) return false;
    m_input.attach(m_window);
    m_window.setRelativeMouse(true);
    if (!m_renderer.init(m_window)) return false;
    if (!m_physics.init()) return false;
    m_jobs.start(std::thread::hardware_concurrency());

    m_voxels.setMeshReadyCallback([this](ChunkPos pos, ChunkMeshData data) {
        if (auto it = m_chunkMeshes.find(pos); it != m_chunkMeshes.end()) {
            m_renderer.destroyMesh(it->second);
            m_chunkMeshes.erase(it);
        }
        if (!data.indices.empty()) {
            m_chunkMeshes[pos] = m_renderer.uploadChunkMesh(data);
            m_physics.syncChunkCollider(pos, data);
        } else {
            m_physics.removeChunkCollider(pos);
        }
    });
    m_voxels.setChunkUnloadedCallback([this](ChunkPos pos) {
        if (auto it = m_chunkMeshes.find(pos); it != m_chunkMeshes.end()) {
            m_renderer.destroyMesh(it->second);
            m_chunkMeshes.erase(it);
        }
        m_physics.removeChunkCollider(pos);
    });

    const TextureHandle atlas = m_renderer.loadTexture("assets/textures/atlas.png");
    if (atlas == 0) return false;
    m_renderer.setAtlas(atlas);
    m_renderer.setDirectionalLight(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)),
                                   glm::vec3(1.0f, 0.96f, 0.88f));
    return true;
}

bool Engine::initNetwork(const EngineConfig& config) {
    using Mode = EngineConfig::Mode;
    if (config.mode == Mode::Game) {
        m_loopback = std::make_unique<LoopbackPair>();
        m_server = std::make_unique<ServerSim>();
        if (!m_server->init(config.seed)) return false;
        m_serverTransport = &m_loopback->serverEnd();
        m_clientTransport = &m_loopback->clientEnd();
    } else if (config.mode == Mode::Host) {
        // The host's own client joins over real UDP to localhost: one code
        // path for every player, and the net layer gets exercised constantly.
        m_enetHost = std::make_unique<EnetServerTransport>();
        if (!m_enetHost->listen(config.port)) return false;
        m_server = std::make_unique<ServerSim>();
        if (!m_server->init(config.seed)) return false;
        m_serverTransport = m_enetHost.get();
        m_enetJoin = std::make_unique<EnetClientTransport>();
        if (!m_enetJoin->connect("127.0.0.1", config.port)) return false;
        m_clientTransport = m_enetJoin.get();
    } else if (config.mode == Mode::Join) {
        m_enetJoin = std::make_unique<EnetClientTransport>();
        if (!m_enetJoin->connect(config.address, config.port)) return false;
        m_clientTransport = m_enetJoin.get();
    }
    m_client.attach(*m_clientTransport, "player");
    return true;
}

void Engine::setupClientWorld() {
    const BlockPalette palette = registerDefaultBlocks(m_voxels.blockRegistry());
    m_voxels.setGenerator(makeTerrainGenerator(m_client.worldSeed(), palette));
    if (!m_player.init(m_physics, kClientSpawn)) {
        log::error("client character init failed");
        return;
    }
    m_prevPlayerPos = m_currPlayerPos = kClientSpawn;
    m_clientWorldReady = true;
    log::info("client world ready (seed {})", m_client.worldSeed());
}

void Engine::simulateClientTick(const PlayerCommand& frameCmd) {
    PlayerCommand cmd = frameCmd;
    cmd.tick = m_tick; // unique per tick even when one frame spans several ticks
    m_client.sendCommand(cmd);
    m_player.update(cmd, kFixedDt, m_physics);
    m_physics.step(kFixedDt);
    if (m_player.position().y < kFallResetY)
        m_player.setState(kClientSpawn, glm::vec3(0)); // fell out (colliders pending)
    m_prevPlayerPos = m_currPlayerPos;
    m_currPlayerPos = m_player.position();
}

void Engine::render(float alpha) {
    Camera camera;
    camera.pos = glm::mix(m_prevPlayerPos, m_currPlayerPos, alpha) +
                 glm::vec3(0.0f, m_player.eyeHeight(), 0.0f);
    camera.yaw = m_lastCmd.yaw; // freshest mouse sample, not the tick's
    camera.pitch = m_lastCmd.pitch;

    m_renderer.beginFrame(camera, alpha);
    for (const auto& [pos, mesh] : m_chunkMeshes)
        m_renderer.submitChunk(mesh, glm::vec3(pos.x, pos.y, pos.z) * (kChunkSize * kVoxelSize));
    m_renderer.drawCrosshair();
    m_renderer.endFrame();
}

int Engine::run(const EngineConfig& config) {
    if (config.mode == EngineConfig::Mode::Dedicated) return runDedicated(config);

    if (!initClientSystems() || !initNetwork(config)) {
        log::error("engine init failed");
        return 1;
    }
    log::info("MeatEngine up — mode {}, 60 Hz tick, {}³ chunks @ {} m voxels",
              static_cast<int>(config.mode), kChunkSize, kVoxelSize);

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    double accumulator = 0.0;

    while (!m_window.shouldClose()) {
        m_input.beginFrame(); // before pollEvents so pressed() edges last one frame
        m_window.pollEvents();
        m_jobs.drainMainThread();

        if (m_input.pressed(GLFW_KEY_ESCAPE)) break;
        if (m_input.pressed(GLFW_KEY_F6)) m_renderer.reloadShaders();

        if (m_server) m_server->pump(*m_serverTransport);
        m_client.pump(m_voxels, m_physics, m_player);
        if (!m_clientWorldReady && m_client.welcomed()) setupClientWorld();

        const auto now = Clock::now();
        const double frameDt =
            std::min(std::chrono::duration<double>(now - last).count(), 0.25);
        last = now;
        accumulator += frameDt;

        m_lastCmd = m_input.sampleCommand(m_tick); // per-frame: look stays fresh
        while (accumulator >= kFixedDt) {
            if (m_clientWorldReady) simulateClientTick(m_lastCmd);
            if (m_server) m_server->tick(*m_serverTransport);
            ++m_tick;
            accumulator -= kFixedDt;
        }

        if (m_clientWorldReady) m_voxels.update(m_currPlayerPos, m_jobs);
        render(static_cast<float>(accumulator / kFixedDt));
        m_window.swap();
    }
    return 0;
}

int Engine::runDedicated(const EngineConfig& config) {
    m_enetHost = std::make_unique<EnetServerTransport>();
    if (!m_enetHost->listen(config.port)) return 1;
    m_server = std::make_unique<ServerSim>();
    if (!m_server->init(config.seed)) return 1;
    log::info("dedicated server on port {} (seed {})", config.port, config.seed);

    using Clock = std::chrono::steady_clock;
    auto next = Clock::now();
    for (;;) { // terminated externally; graceful shutdown comes with the save system
        m_server->pump(*m_enetHost);
        m_server->tick(*m_enetHost);
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
    }
}

} // namespace meat
