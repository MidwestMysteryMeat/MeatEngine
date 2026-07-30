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

// Placeholder terrain until dungeon/room content lands: stone, dirt, grass top.
constexpr int kStoneTop = 5, kDirtTop = 7, kGrassY = 7;

} // namespace

bool Engine::init() {
    if (!m_window.init({})) return false;
    m_input.attach(m_window);
    m_window.setRelativeMouse(true);
    if (!m_renderer.init(m_window)) return false;
    if (!m_physics.init()) return false;

    m_jobs.start(std::thread::hardware_concurrency());

    // Block palette; ids must match the generator below and the atlas tiles.
    auto& blocks = m_voxels.blockRegistry();
    const BlockId stone = blocks.add({"stone", {1, 1, 1, 1, 1, 1}, true});
    const BlockId dirt = blocks.add({"dirt", {2, 2, 2, 2, 2, 2}, true});
    [[maybe_unused]] const BlockId grass = blocks.add({"grass", {4, 4, 3, 2, 4, 4}, true});

    m_voxels.setGenerator([=](Chunk& chunk, ChunkPos pos) {
        for (int y = 0; y < kChunkSize; ++y) {
            const int worldY = pos.y * kChunkSize + y;
            if (worldY > kGrassY) continue;
            const BlockId id = worldY <= kStoneTop ? stone : worldY < kDirtTop ? dirt : grass;
            for (int z = 0; z < kChunkSize; ++z)
                for (int x = 0; x < kChunkSize; ++x) chunk.set(x, y, z, id);
        }
        chunk.clearDirty(); // generation isn't an edit; update() queues the mesh
    });

    // Mesh results arrive on the main thread (voxel module posts them): GPU
    // upload and collider sync stay in lockstep so you never walk on air.
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

    const TextureHandle atlas = m_renderer.loadTexture("assets/textures/atlas.png");
    if (atlas == 0) return false;
    m_renderer.setAtlas(atlas);
    m_renderer.setDirectionalLight(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)),
                                   glm::vec3(1.0f, 0.96f, 0.88f));

    const glm::vec3 spawn{8.0f, (kGrassY + 1) * kVoxelSize + 0.1f, 8.0f};
    if (!m_player.init(m_physics, spawn)) return false;
    m_prevPlayerPos = m_currPlayerPos = spawn;
    return true;
}

void Engine::simulate(const PlayerCommand& cmd) {
    // Colliders for freshly meshed chunks were synced in drainMainThread before
    // this; character before step is the CharacterVirtual pattern.
    m_player.update(cmd, kFixedDt, m_physics);
    m_physics.step(kFixedDt);
    m_prevPlayerPos = m_currPlayerPos;
    m_currPlayerPos = m_player.position();
}

void Engine::render(float alpha) {
    Camera camera;
    camera.pos = glm::mix(m_prevPlayerPos, m_currPlayerPos, alpha) +
                 glm::vec3(0.0f, m_player.eyeHeight(), 0.0f);
    camera.yaw = m_lastCmd.yaw; // look tracks the freshest mouse sample, not the tick
    camera.pitch = m_lastCmd.pitch;

    m_renderer.beginFrame(camera, alpha);
    for (const auto& [pos, mesh] : m_chunkMeshes)
        m_renderer.submitChunk(mesh, glm::vec3(pos.x, pos.y, pos.z) * (kChunkSize * kVoxelSize));
    m_renderer.drawCrosshair();
    m_renderer.endFrame();
}

int Engine::run() {
    if (!init()) {
        log::error("engine init failed");
        return 1;
    }
    log::info("MeatEngine up — tick {} Hz, chunk {} @ {} m voxels", 60, kChunkSize, kVoxelSize);

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    double accumulator = 0.0;

    while (!m_window.shouldClose()) {
        m_input.beginFrame(); // before pollEvents so pressed() edges last one frame
        m_window.pollEvents();
        m_jobs.drainMainThread();

        if (m_input.pressed(GLFW_KEY_ESCAPE)) break;
        if (m_input.pressed(GLFW_KEY_F6)) m_renderer.reloadShaders();

        const auto now = Clock::now();
        const double frameDt =
            std::min(std::chrono::duration<double>(now - last).count(), 0.25);
        last = now;
        accumulator += frameDt;

        m_lastCmd = m_input.sampleCommand(m_tick);
        while (accumulator >= kFixedDt) {
            simulate(m_lastCmd);
            ++m_tick;
            accumulator -= kFixedDt;
        }

        m_voxels.update(m_currPlayerPos, m_jobs);
        render(static_cast<float>(accumulator / kFixedDt));
        m_window.swap();
    }
    return 0;
}

} // namespace meat
