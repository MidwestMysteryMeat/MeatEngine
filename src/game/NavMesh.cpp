#include "game/NavMesh.h"

#include "engine/core/Log.h"
#include "engine/voxel/Chunk.h"       // ChunkPos, chunkWorldSize(), kVoxelSize
#include "engine/voxel/ChunkMesher.h" // ChunkMeshData, VoxelVertex

#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace meat {
namespace {

// Rebuilds share one throttle window so streaming chunks can't trigger a Recast
// build every query — at most one bounded build per this interval.
constexpr std::int64_t kRebuildCooldownMs = 2000;
// Sanity cap on the rasterization grid; a runaway bounds computation would
// otherwise allocate a huge heightfield. Near-spawn geometry stays well under.
constexpr std::int64_t kMaxGridCells = 4'000'000;

// One Detour query area/flag — every walkable poly gets area 0, flag 1, and the
// default query filter (includeFlags 0xffff) accepts it.
constexpr unsigned short kWalkableFlag = 0x01;

} // namespace

struct NavMesh::Impl {
    struct Geom {
        std::vector<float> verts; // world-space x,y,z triples
        std::vector<int> tris;    // triangle indices into verts
    };

    std::unordered_map<ChunkPos, Geom> chunks;
    bool dirty = false;

    dtNavMesh* navMesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    bool built = false;
    std::chrono::steady_clock::time_point lastBuild{}; // epoch => "never built"

    ~Impl() { release(); }

    void release() {
        if (query) {
            dtFreeNavMeshQuery(query);
            query = nullptr;
        }
        if (navMesh) {
            dtFreeNavMesh(navMesh);
            navMesh = nullptr;
        }
        built = false;
    }

    // Runs the Recast solo build over all accumulated chunk geometry. On success
    // swaps in a fresh Detour navmesh + query and returns true; on any failure
    // the previously built mesh (if any) is left intact. Technique reference:
    // RecastDemo Sample_SoloMesh::handleBuild (zlib) — original code here.
    bool rebuild();
};

bool NavMesh::Impl::rebuild() {
    // Concatenate every chunk's world-space triangle soup.
    std::vector<float> verts;
    std::vector<int> tris;
    for (const auto& [pos, geom] : chunks) {
        const int base = static_cast<int>(verts.size() / 3);
        verts.insert(verts.end(), geom.verts.begin(), geom.verts.end());
        for (int idx : geom.tris)
            tris.push_back(base + idx);
    }
    const int nverts = static_cast<int>(verts.size() / 3);
    const int ntris = static_cast<int>(tris.size() / 3);
    if (nverts < 3 || ntris < 1)
        return false;

    // Agent + rasterization params scale with the dev-chosen voxel size. Radius
    // is left at 0 (no erosion) so single-voxel-wide corridors stay walkable —
    // the A* fallback still guards anything Detour declines to path.
    const float cs = kVoxelSize * 0.5f;             // cell size (~0.25 m @ 0.5)
    const float ch = kVoxelSize * 0.25f;            // cell height (~0.125 m)
    const float agentHeight = kVoxelSize * 3.6f;    // ~1.8 m
    const float agentRadius = 0.0f;                 // no erosion (see above)
    const float agentMaxClimb = kVoxelSize * 1.25f; // step up ~1 voxel

    rcConfig cfg{};
    cfg.cs = cs;
    cfg.ch = ch;
    cfg.walkableSlopeAngle = 46.0f;
    cfg.walkableHeight = static_cast<int>(std::ceil(agentHeight / ch));
    cfg.walkableClimb = static_cast<int>(std::floor(agentMaxClimb / ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(agentRadius / cs));
    cfg.maxEdgeLen = static_cast<int>(12.0f / cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = static_cast<int>(rcSqr(8));
    cfg.mergeRegionArea = static_cast<int>(rcSqr(20));
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = cs * 6.0f;
    cfg.detailSampleMaxError = ch * 1.0f;

    rcCalcBounds(verts.data(), nverts, cfg.bmin, cfg.bmax);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cs, &cfg.width, &cfg.height);
    if (static_cast<std::int64_t>(cfg.width) * cfg.height > kMaxGridCells) {
        log::warn("navmesh: grid {}x{} exceeds cap, skipping build", cfg.width, cfg.height);
        return false;
    }

    rcContext ctx(false); // no perf timers

    // RAII for the Recast scratch structures — freed on every return path.
    auto hfDel = [](rcHeightfield* p) { rcFreeHeightField(p); };
    auto chfDel = [](rcCompactHeightfield* p) { rcFreeCompactHeightfield(p); };
    auto csetDel = [](rcContourSet* p) { rcFreeContourSet(p); };
    auto pmDel = [](rcPolyMesh* p) { rcFreePolyMesh(p); };
    auto dmDel = [](rcPolyMeshDetail* p) { rcFreePolyMeshDetail(p); };
    std::unique_ptr<rcHeightfield, decltype(hfDel)> solid(rcAllocHeightfield(), hfDel);
    std::unique_ptr<rcCompactHeightfield, decltype(chfDel)> chf(rcAllocCompactHeightfield(), chfDel);
    std::unique_ptr<rcContourSet, decltype(csetDel)> cset(rcAllocContourSet(), csetDel);
    std::unique_ptr<rcPolyMesh, decltype(pmDel)> pmesh(rcAllocPolyMesh(), pmDel);
    std::unique_ptr<rcPolyMeshDetail, decltype(dmDel)> dmesh(rcAllocPolyMeshDetail(), dmDel);
    if (!solid || !chf || !cset || !pmesh || !dmesh)
        return false;

    if (!rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cs, ch))
        return false;

    std::vector<unsigned char> areas(static_cast<std::size_t>(ntris), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), nverts, tris.data(), ntris,
                            areas.data());
    if (!rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), areas.data(), ntris, *solid,
                              cfg.walkableClimb))
        return false;

    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

    if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf))
        return false;
    if (cfg.walkableRadius > 0 && !rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf))
        return false;
    if (!rcBuildDistanceField(&ctx, *chf))
        return false;
    if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea))
        return false;

    if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset))
        return false;
    if (cset->nconts == 0)
        return false; // nothing walkable
    if (!rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh))
        return false;
    if (!rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError,
                               *dmesh))
        return false;
    if (pmesh->npolys == 0)
        return false;

    // Flag every walkable poly so the default query filter accepts it.
    for (int i = 0; i < pmesh->npolys; ++i) {
        if (pmesh->areas[i] == RC_WALKABLE_AREA)
            pmesh->areas[i] = 0;
        pmesh->flags[i] = kWalkableFlag;
    }

    dtNavMeshCreateParams params{};
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.walkableHeight = agentHeight;
    params.walkableRadius = agentRadius;
    params.walkableClimb = agentMaxClimb;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    unsigned char* navData = nullptr;
    int navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize)) {
        log::warn("navmesh: dtCreateNavMeshData failed");
        return false;
    }

    dtNavMesh* mesh = dtAllocNavMesh();
    if (!mesh) {
        dtFree(navData);
        return false;
    }
    // DT_TILE_FREE_DATA: the navmesh takes ownership of navData.
    if (dtStatusFailed(mesh->init(navData, navDataSize, DT_TILE_FREE_DATA))) {
        dtFree(navData);
        dtFreeNavMesh(mesh);
        return false;
    }
    dtNavMeshQuery* q = dtAllocNavMeshQuery();
    if (!q || dtStatusFailed(q->init(mesh, 2048))) {
        if (q)
            dtFreeNavMeshQuery(q);
        dtFreeNavMesh(mesh);
        return false;
    }

    // Success — swap in, releasing any prior mesh.
    release();
    navMesh = mesh;
    query = q;
    built = true;
    log::info("navmesh: built {} polys from {} tris ({} chunks)", pmesh->npolys, ntris,
              chunks.size());
    return true;
}

NavMesh::NavMesh() : m_impl(std::make_unique<Impl>()) {}
NavMesh::~NavMesh() = default;

void NavMesh::addChunk(ChunkPos pos, const ChunkMeshData& mesh) {
    if (mesh.indices.empty()) {
        removeChunk(pos);
        return;
    }
    // Chunk mesh verts are local chunk-space metres; place them in world space.
    const float extent = chunkWorldSize();
    const glm::vec3 origin(static_cast<float>(pos.x) * extent, static_cast<float>(pos.y) * extent,
                           static_cast<float>(pos.z) * extent);
    Impl::Geom geom;
    geom.verts.reserve(mesh.vertices.size() * 3);
    for (const VoxelVertex& v : mesh.vertices) {
        geom.verts.push_back(v.pos.x + origin.x);
        geom.verts.push_back(v.pos.y + origin.y);
        geom.verts.push_back(v.pos.z + origin.z);
    }
    geom.tris.reserve(mesh.indices.size());
    for (std::uint32_t i : mesh.indices)
        geom.tris.push_back(static_cast<int>(i));
    m_impl->chunks[pos] = std::move(geom);
    m_impl->dirty = true;
}

void NavMesh::removeChunk(ChunkPos pos) {
    if (m_impl->chunks.erase(pos) > 0)
        m_impl->dirty = true;
}

bool NavMesh::ready() const { return m_impl->built; }

bool NavMesh::queryPath(glm::vec3 start, glm::vec3 goal, std::vector<glm::vec3>& outCorners) {
    // Lazy, throttled rebuild: only when geometry changed and either we've never
    // built or the cooldown elapsed — so a query never triggers back-to-back builds.
    if (m_impl->dirty) {
        const auto now = std::chrono::steady_clock::now();
        const bool never = !m_impl->built;
        const auto sinceMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_impl->lastBuild).count();
        if (never || sinceMs >= kRebuildCooldownMs) {
            m_impl->rebuild(); // failure leaves any prior mesh intact
            m_impl->lastBuild = now;
            m_impl->dirty = false;
        }
    }
    if (!m_impl->built || !m_impl->query)
        return false;

    dtNavMeshQuery* q = m_impl->query;
    dtQueryFilter filter; // default: includeFlags 0xffff, unit area costs

    const float halfExtents[3] = {2.0f * kVoxelSize, 4.0f * kVoxelSize, 2.0f * kVoxelSize};
    const float s[3] = {start.x, start.y, start.z};
    const float e[3] = {goal.x, goal.y, goal.z};

    dtPolyRef startRef = 0, endRef = 0;
    float startPt[3], endPt[3];
    if (dtStatusFailed(q->findNearestPoly(s, halfExtents, &filter, &startRef, startPt)) || !startRef)
        return false;
    if (dtStatusFailed(q->findNearestPoly(e, halfExtents, &filter, &endRef, endPt)) || !endRef)
        return false;

    constexpr int kMaxPolys = 256;
    dtPolyRef polys[kMaxPolys];
    int npolys = 0;
    if (dtStatusFailed(
            q->findPath(startRef, endRef, startPt, endPt, &filter, polys, &npolys, kMaxPolys)) ||
        npolys == 0)
        return false;

    // If the goal poly wasn't reached (partial path), aim at the closest point on
    // the last poly so the straight path still ends somewhere valid.
    float target[3] = {endPt[0], endPt[1], endPt[2]};
    if (polys[npolys - 1] != endRef) {
        float closest[3];
        if (dtStatusSucceed(q->closestPointOnPoly(polys[npolys - 1], endPt, closest, nullptr))) {
            target[0] = closest[0];
            target[1] = closest[1];
            target[2] = closest[2];
        }
    }

    float straight[kMaxPolys * 3];
    int nstraight = 0;
    if (dtStatusFailed(q->findStraightPath(startPt, target, polys, npolys, straight, nullptr,
                                           nullptr, &nstraight, kMaxPolys)) ||
        nstraight == 0)
        return false;

    outCorners.clear();
    outCorners.reserve(static_cast<std::size_t>(nstraight));
    for (int i = 0; i < nstraight; ++i)
        outCorners.emplace_back(straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]);
    return true;
}

} // namespace meat
