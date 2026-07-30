#pragma once
#include "engine/core/EntityRegistry.h"
#include "engine/core/EventBus.h"
#include "engine/core/JobQueue.h"
#include "engine/physics/CharacterController.h"
#include "engine/physics/PhysicsWorld.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/render/Renderer.h"
#include "engine/voxel/VoxelWorld.h"

#include <unordered_map>

namespace meat {

inline constexpr float kFixedDt = 1.0f / 60.0f;

// Owns every subsystem as a value member; construction order here is the
// ownership diagram in ARCHITECTURE.md, destruction is the reverse. JobQueue
// precedes VoxelWorld so workers can still post() while VoxelWorld tears down.
class Engine {
public:
    int run();

private:
    bool init();
    void simulate(const PlayerCommand& cmd);
    void render(float alpha);

    Window m_window;
    Input m_input;
    JobQueue m_jobs;
    Renderer m_renderer;
    PhysicsWorld m_physics;
    VoxelWorld m_voxels;
    EntityRegistry m_entities;
    EventBus m_events;
    CharacterController m_player;

    std::unordered_map<ChunkPos, MeshHandle> m_chunkMeshes;
    PlayerCommand m_lastCmd{};
    std::uint64_t m_tick = 0;
    glm::vec3 m_prevPlayerPos{0};
    glm::vec3 m_currPlayerPos{0};
};

} // namespace meat
