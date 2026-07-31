#include "engine/physics/PhysicsWorld.h"

#include "engine/core/Log.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

JPH_SUPPRESS_WARNINGS

#include <cstddef>
#include <map>
#include <utility>

namespace meat {

namespace {

namespace layers {
constexpr JPH::ObjectLayer kNonMoving = objlayer::kNonMoving;
constexpr JPH::ObjectLayer kMoving = objlayer::kMoving;
} // namespace layers

namespace bp {
constexpr JPH::BroadPhaseLayer kNonMoving(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
constexpr JPH::uint kCount = 2;
} // namespace bp

// One broad-phase layer per object layer: the (many, rarely-changing) static
// chunk bodies get their own tree so moving bodies never churn it.
class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return bp::kCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == layers::kNonMoving ? bp::kNonMoving : bp::kMoving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == bp::kNonMoving ? "NON_MOVING" : "MOVING";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer object, JPH::BroadPhaseLayer broadPhase) const override {
        // Static vs static never collides; anything involving a mover does.
        return object == layers::kMoving || broadPhase == bp::kMoving;
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == layers::kMoving || b == layers::kMoving;
    }
};

} // namespace

struct PhysicsWorld::Impl {
    // Init() stores references to the three interfaces — they must outlive system.
    BroadPhaseLayerMap broadPhaseLayers;
    ObjectVsBroadPhaseFilter objectVsBroadPhase;
    ObjectPairFilter objectPairs;
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
    std::unique_ptr<JPH::PhysicsSystem> system;
    std::map<ChunkPos, JPH::BodyID> chunkBodies;
};

PhysicsWorld::PhysicsWorld() : m_impl(std::make_unique<Impl>()) {}

PhysicsWorld::~PhysicsWorld() {
    if (!m_impl->system)
        return;
    JPH::BodyInterface& bodies = m_impl->system->GetBodyInterface();
    for (const auto& entry : m_impl->chunkBodies) {
        bodies.RemoveBody(entry.second);
        bodies.DestroyBody(entry.second);
    }
    m_impl->chunkBodies.clear();
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

bool PhysicsWorld::init() {
    if (m_impl->system) {
        log::warn("PhysicsWorld: init() called twice");
        return true;
    }
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    m_impl->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
    // Jolt's own pool, capped at 2 threads. The engine JobQueue is meshing/gen
    // only and is never used for physics.
    m_impl->jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs,
                                                                   JPH::cMaxPhysicsBarriers, 2);

    constexpr JPH::uint kMaxBodies = 8192;
    constexpr JPH::uint kNumBodyMutexes = 0; // 0 = Jolt default
    constexpr JPH::uint kMaxBodyPairs = 8192;
    constexpr JPH::uint kMaxContactConstraints = 4096;
    m_impl->system = std::make_unique<JPH::PhysicsSystem>();
    m_impl->system->Init(kMaxBodies, kNumBodyMutexes, kMaxBodyPairs, kMaxContactConstraints,
                         m_impl->broadPhaseLayers, m_impl->objectVsBroadPhase,
                         m_impl->objectPairs);
    // Contract gravity: -18 m/s² for a snappier FPS feel than earth's -9.81.
    m_impl->system->SetGravity(JPH::Vec3(0.0f, -18.0f, 0.0f));

    log::info("PhysicsWorld: Jolt initialized ({} max bodies, 2 physics threads)", kMaxBodies);
    return true;
}

void PhysicsWorld::setGravity(float gravityY) {
    if (!m_impl->system)
        return;
    m_impl->system->SetGravity(JPH::Vec3(0.0f, gravityY, 0.0f));
}

void PhysicsWorld::step(float fixedDt) {
    if (!m_impl->system)
        return;
    const JPH::EPhysicsUpdateError err = m_impl->system->Update(
        fixedDt, 1, m_impl->tempAllocator.get(), m_impl->jobSystem.get());
    if (err != JPH::EPhysicsUpdateError::None)
        log::warn("PhysicsWorld: update error {:#x}", static_cast<std::uint32_t>(err));
}

void PhysicsWorld::syncChunkCollider(ChunkPos pos, const ChunkMeshData& mesh) {
    if (!m_impl->system)
        return;
    removeChunkCollider(pos);
    if (mesh.indices.empty())
        return;

    JPH::VertexList vertices;
    vertices.reserve(mesh.vertices.size());
    for (const VoxelVertex& v : mesh.vertices)
        vertices.push_back(JPH::Float3(v.pos.x, v.pos.y, v.pos.z));

    JPH::IndexedTriangleList triangles;
    triangles.reserve(mesh.indices.size() / 3);
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        triangles.emplace_back(mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]);

    JPH::MeshShapeSettings shapeSettings(std::move(vertices), std::move(triangles));
    const JPH::ShapeSettings::ShapeResult result = shapeSettings.Create();
    if (result.HasError()) {
        log::error("PhysicsWorld: mesh shape for chunk ({},{},{}) failed: {}", pos.x, pos.y,
                   pos.z, result.GetError().c_str());
        return;
    }

    // Chunk world extent tracks the dev-chosen voxel size (mesh verts are already scaled).
    const float chunkExtent = chunkWorldSize();
    const JPH::RVec3 origin(static_cast<float>(pos.x) * chunkExtent,
                            static_cast<float>(pos.y) * chunkExtent,
                            static_cast<float>(pos.z) * chunkExtent);
    const JPH::BodyCreationSettings bodySettings(result.Get(), origin, JPH::Quat::sIdentity(),
                                                 JPH::EMotionType::Static, layers::kNonMoving);
    const JPH::BodyID id = m_impl->system->GetBodyInterface().CreateAndAddBody(
        bodySettings, JPH::EActivation::DontActivate);
    if (id.IsInvalid()) {
        log::error("PhysicsWorld: out of bodies syncing chunk ({},{},{})", pos.x, pos.y, pos.z);
        return;
    }
    m_impl->chunkBodies.emplace(pos, id);
}

void PhysicsWorld::removeChunkCollider(ChunkPos pos) {
    if (!m_impl->system)
        return;
    const auto it = m_impl->chunkBodies.find(pos);
    if (it == m_impl->chunkBodies.end())
        return;
    JPH::BodyInterface& bodies = m_impl->system->GetBodyInterface();
    bodies.RemoveBody(it->second);
    bodies.DestroyBody(it->second);
    m_impl->chunkBodies.erase(it);
}

PhysicsWorld::RayHit PhysicsWorld::raycast(glm::vec3 from, glm::vec3 dir, float maxDist) const {
    RayHit out;
    if (!m_impl->system)
        return out;
    const JPH::RRayCast ray(JPH::RVec3(from.x, from.y, from.z),
                            JPH::Vec3(dir.x, dir.y, dir.z) * maxDist);
    JPH::RayCastResult hit;
    if (!m_impl->system->GetNarrowPhaseQuery().CastRay(ray, hit))
        return out;

    JPH::BodyLockRead lock(m_impl->system->GetBodyLockInterface(), hit.mBodyID);
    if (!lock.Succeeded())
        return out;
    const JPH::Body& body = lock.GetBody();
    const JPH::RVec3 pos = ray.GetPointOnRay(hit.mFraction);
    const JPH::Vec3 normal = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, pos);

    out.hit = true;
    out.pos = glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
    out.normal = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
    out.entity = static_cast<EntityId>(body.GetUserData()); // 0 until bodies carry entities
    return out;
}

JPH::PhysicsSystem* PhysicsWorld::joltSystem() const { return m_impl->system.get(); }

JPH::TempAllocator* PhysicsWorld::joltTempAllocator() const {
    return m_impl->tempAllocator.get();
}

} // namespace meat
