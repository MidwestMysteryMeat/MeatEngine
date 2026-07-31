#pragma once

#include "engine/core/EntityRegistry.h"
#include "engine/voxel/ChunkMesher.h" // ChunkPos, ChunkMeshData

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>

// Forward declarations only — Jolt headers never leak past src/engine/physics/*.cpp.
namespace JPH {
class PhysicsSystem;
class TempAllocator;
} // namespace JPH

namespace meat {

// Object layers, mirrored into JPH::ObjectLayer inside the physics .cpp files.
// Static chunk colliders live on kNonMoving, everything that moves on kMoving;
// non-moving never tests against non-moving.
namespace objlayer {
inline constexpr std::uint16_t kNonMoving = 0;
inline constexpr std::uint16_t kMoving = 1;
inline constexpr std::uint16_t kCount = 2;
} // namespace objlayer

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    bool init();
    // World gravity in m/s² along -Y (e.g. -18 surface, -6 underwater, -1.5 space). Applied from
    // the world's Environment preset; safe to call after init(). No-op before init().
    void setGravity(float gravityY);
    void step(float fixedDt); // one PhysicsSystem::Update, 1 collision step

    // Builds a static MeshShape from the render mesh. Vertex positions are
    // chunk-local meters; the body sits at the chunk's world origin
    // (ChunkPos * 16 m). Replaces any existing collider for this chunk;
    // empty mesh data just removes it.
    void syncChunkCollider(ChunkPos pos, const ChunkMeshData& mesh);
    void removeChunkCollider(ChunkPos pos);

    // Opaque handle to a standalone static body (used for world props). It is the
    // Jolt body id; kInvalidBody marks a failed create. Unlike chunk colliders
    // (keyed by ChunkPos), these are addressed by the handle the caller keeps.
    using BodyHandle = std::uint32_t;
    static constexpr BodyHandle kInvalidBody = 0xFFFFFFFFu;
    // A static, axis-aligned box collider at world `center` with `halfExtents` (m).
    // Lives on the same non-moving layer as chunk colliders, so a CharacterVirtual
    // (player) collides with it exactly like terrain. halfExtents are floored to a
    // small positive value so a degenerate box never trips a Jolt assert.
    BodyHandle addStaticBox(glm::vec3 center, glm::vec3 halfExtents);
    void removeStaticBox(BodyHandle body);
    // Kinematic box (H4 ships): CharacterVirtual collides with it like terrain, but
    // the server moves it each tick via setBodyTransform. halfExtents local AABB.
    BodyHandle addKinematicBox(glm::vec3 center, glm::vec3 halfExtents);
    void setBodyTransform(BodyHandle body, glm::vec3 center, glm::quat rotation);

    struct RayHit {
        bool hit = false;
        glm::vec3 pos{0.0f};
        glm::vec3 normal{0.0f};
        EntityId entity = 0; // stays 0 until bodies carry entity user data
    };
    RayHit raycast(glm::vec3 from, glm::vec3 dir, float maxDist) const; // dir normalized

    // For CharacterController only — the rest of the engine must not touch
    // these. Null until init() succeeds.
    JPH::PhysicsSystem* joltSystem() const;
    JPH::TempAllocator* joltTempAllocator() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace meat
