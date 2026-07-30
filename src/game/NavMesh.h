#pragma once
#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace meat {

struct ChunkMeshData;
struct ChunkPos;

// OPTIONAL Detour navmesh path provider, built from the world's chunk collision
// meshes (the same triangle soup ServerSim hands the physics colliders). It is a
// host-authoritative ADVISORY: ServerSim tries queryPath() and falls back to the
// hand-rolled voxel A* (game/Pathfinder) whenever the navmesh isn't built yet or
// a query misses, so NPC behaviour never regresses. NPC pathing runs only on the
// authoritative server (clients receive snapshots), so using Detour here adds no
// cross-peer nondeterminism.
//
// The build is LAZY and THROTTLED — chunks stream in via addChunk()/removeChunk()
// which only mark the mesh dirty; the actual Recast rasterize+build happens on the
// next query, at most once per cooldown window, so a tick is never blocked by more
// than a single (bounded, near-spawn) build. A static navmesh is sufficient for
// this pass: an in-place voxel edit won't repath around the change until the next
// throttled rebuild sees the updated chunk mesh (A* remains instantly edit-aware).
//
// Technique reference: RecastDemo Sample_SoloMesh (zlib) — solo (single-tile)
// rasterize → compact heightfield → regions → contours → poly mesh → Detour.
class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // Feed / withdraw a chunk's collision geometry. Vertices are transformed to
    // world space here (chunk origin = pos * chunkWorldSize()). Marks dirty.
    void addChunk(ChunkPos pos, const ChunkMeshData& mesh);
    void removeChunk(ChunkPos pos);

    // World-space query. On success fills outCorners with string-pulled waypoints
    // (start..goal) and returns true. Returns false — the "use A*" signal — when
    // no navmesh is available or start/goal don't map onto it. Triggers a lazy,
    // throttled rebuild if the geometry changed; cheap when already built + clean.
    bool queryPath(glm::vec3 start, glm::vec3 goal, std::vector<glm::vec3>& outCorners);

    bool ready() const; // a navmesh is currently built and queryable

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace meat
