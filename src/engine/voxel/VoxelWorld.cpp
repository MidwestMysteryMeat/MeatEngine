#include "engine/voxel/VoxelWorld.h"

#include "engine/core/JobQueue.h"

#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <utility>

namespace meat {
namespace {

constexpr int kStreamRadiusH = 6;
constexpr int kStreamRadiusV = 2;
constexpr int kUnloadPad = 2;

const std::array<glm::ivec3, 6> kFaceDirs = {{
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
}};

int floorDivChunk(int a) {
    return (a >= 0 ? a : a - (kChunkSize - 1)) / kChunkSize;
}

ChunkPos offsetChunk(ChunkPos p, glm::ivec3 d) {
    return {p.x + d.x, p.y + d.y, p.z + d.z};
}

glm::ivec3 localInChunk(glm::ivec3 voxel, ChunkPos c) {
    return voxel - glm::ivec3(c.x, c.y, c.z) * kChunkSize;
}

} // namespace

glm::ivec3 worldToVoxel(glm::vec3 p) {
    return glm::ivec3(glm::floor(p / kVoxelSize));
}

ChunkPos voxelToChunk(glm::ivec3 voxel) {
    return {floorDivChunk(voxel.x), floorDivChunk(voxel.y), floorDivChunk(voxel.z)};
}

VoxelWorld::VoxelWorld() {
    m_generator = [](Chunk& chunk, ChunkPos pos) {
        const int baseY = pos.y * kChunkSize;
        for (int y = 0; y < kChunkSize; ++y) {
            if (baseY + y >= 8) break; // rest stays air (zero-initialized)
            for (int z = 0; z < kChunkSize; ++z)
                for (int x = 0; x < kChunkSize; ++x) chunk.set(x, y, z, 1);
        }
    };
}

const Chunk* VoxelWorld::chunkAt(ChunkPos pos) const {
    const auto it = m_chunks.find(pos);
    return it != m_chunks.end() ? it->second.get() : nullptr;
}

Chunk* VoxelWorld::chunkAtMut(ChunkPos pos) {
    const auto it = m_chunks.find(pos);
    return it != m_chunks.end() ? it->second.get() : nullptr;
}

BlockId VoxelWorld::blockAt(glm::ivec3 voxel) const {
    const ChunkPos cp = voxelToChunk(voxel);
    const Chunk* chunk = chunkAt(cp);
    if (!chunk) return 0;
    const glm::ivec3 l = localInChunk(voxel, cp);
    return chunk->at(l.x, l.y, l.z);
}

Chunk& VoxelWorld::ensureChunk(ChunkPos pos) {
    if (const auto it = m_chunks.find(pos); it != m_chunks.end()) return *it->second;
    auto chunk = std::make_unique<Chunk>();
    m_generator(*chunk, pos);
    if (const auto ov = m_overlay.find(pos); ov != m_overlay.end()) {
        for (const auto& [index, block] : ov->second) {
            const int x = index % kChunkSize, z = (index / kChunkSize) % kChunkSize,
                      y = index / (kChunkSize * kChunkSize);
            chunk->set(x, y, z, block);
        }
    }
    chunk->markDirty();
    Chunk& ref = *chunk;
    m_chunks.emplace(pos, std::move(chunk));
    // Neighbors previously meshed against missing-chunk "air" here; their
    // border faces may now be occluded (or newly exposed), so remesh them.
    for (const glm::ivec3& d : kFaceDirs) {
        if (const auto it = m_chunks.find(offsetChunk(pos, d)); it != m_chunks.end())
            it->second->markDirty();
    }
    // Light this fresh chunk from its own emitters and from any already-lit
    // neighbor bleeding across the seam (done after emplace so the BFS can
    // reach it through chunkAtMut).
    computeChunkLightOnLoad(pos);
    return ref;
}

void VoxelWorld::setBlock(glm::ivec3 voxel, BlockId block) {
    const ChunkPos cp = voxelToChunk(voxel);
    Chunk& chunk = ensureChunk(cp);
    const glm::ivec3 l = localInChunk(voxel, cp);
    chunk.set(l.x, l.y, l.z, block);
    m_overlay[cp][static_cast<std::uint16_t>(l.x + l.z * kChunkSize +
                                             l.y * kChunkSize * kChunkSize)] = block;

    // A border edit changes which faces the adjacent chunk culls.
    const auto touch = [this](ChunkPos p) {
        if (const auto it = m_chunks.find(p); it != m_chunks.end()) it->second->markDirty();
    };
    const int m = kChunkSize - 1;
    if (l.x == 0) touch({cp.x - 1, cp.y, cp.z});
    if (l.x == m) touch({cp.x + 1, cp.y, cp.z});
    if (l.y == 0) touch({cp.x, cp.y - 1, cp.z});
    if (l.y == m) touch({cp.x, cp.y + 1, cp.z});
    if (l.z == 0) touch({cp.x, cp.y, cp.z - 1});
    if (l.z == m) touch({cp.x, cp.y, cp.z + 1});

    // Re-flow block-light for the edit: tear down light the old block carried,
    // seed light the new block emits, and refill any air the edit exposed.
    updateLightForEdit(voxel);
}

std::uint8_t VoxelWorld::voxelLight(glm::ivec3 v) const {
    const ChunkPos cp = voxelToChunk(v);
    const Chunk* chunk = chunkAt(cp);
    if (!chunk) return 0;
    const glm::ivec3 l = localInChunk(v, cp);
    return chunk->lightAt(l.x, l.y, l.z);
}

void VoxelWorld::setVoxelLight(glm::ivec3 v, std::uint8_t level) {
    const ChunkPos cp = voxelToChunk(v);
    Chunk* chunk = chunkAtMut(cp);
    if (!chunk) return; // never write light into an unloaded chunk
    const glm::ivec3 l = localInChunk(v, cp);
    chunk->setLight(l.x, l.y, l.z, level);
    chunk->markDirty(); // light changed -> the chunk must re-mesh
}

std::uint8_t VoxelWorld::blockEmission(BlockId id) const {
    // Tolerate ids the registry hasn't seen: chunk-load light runs inside
    // ensureChunk, which can fire on the client mirror before setupClientWorld
    // registers blocks (a server op applied during Client::pump). No registered
    // block => nothing emits, so an unknown id contributes no light.
    return m_blocks.isValid(id) ? m_blocks.get(id).lightEmission : 0;
}

bool VoxelWorld::voxelSolid(glm::ivec3 v) const {
    const BlockId id = blockAt(v);
    return m_blocks.isValid(id) && m_blocks.get(id).solid;
}

// Increasing BFS: each seeded voxel already holds its final light; spread it to
// non-solid neighbors at level-1 wherever that is brighter than what they hold.
// The max-plus rule makes the result independent of queue order (deterministic).
void VoxelWorld::propagateLight(std::queue<glm::ivec3>& open) {
    while (!open.empty()) {
        const glm::ivec3 v = open.front();
        open.pop();
        const std::uint8_t level = voxelLight(v);
        if (level <= 1) continue; // nothing left to give
        for (const glm::ivec3& d : kFaceDirs) {
            const glm::ivec3 n = v + d;
            if (!chunkAtMut(voxelToChunk(n))) continue; // stay inside loaded chunks
            if (voxelSolid(n)) continue;                // light stops at solids
            if (voxelLight(n) + 1 < level) {            // neighbor dimmer than level-1
                setVoxelLight(n, static_cast<std::uint8_t>(level - 1));
                open.push(n);
            }
        }
    }
}

// Removal BFS (classic un-light then re-light): zero every voxel that was fed by
// the removed source (strictly dimmer than the wave), and collect any voxel that
// is as-bright-or-brighter as a surviving source to re-flood from afterwards.
void VoxelWorld::relightAfterRemoval(glm::ivec3 voxel, std::uint8_t oldLevel) {
    std::queue<std::pair<glm::ivec3, std::uint8_t>> dark;
    std::queue<glm::ivec3> relight;
    setVoxelLight(voxel, 0);
    dark.push({voxel, oldLevel});
    while (!dark.empty()) {
        const auto [v, level] = dark.front();
        dark.pop();
        for (const glm::ivec3& d : kFaceDirs) {
            const glm::ivec3 n = v + d;
            if (!chunkAtMut(voxelToChunk(n))) continue;
            const std::uint8_t nl = voxelLight(n);
            if (nl != 0 && nl < level) {
                setVoxelLight(n, 0);
                dark.push({n, nl});
            } else if (nl >= level) {
                relight.push(n); // a competing/surviving source
            }
        }
    }
    propagateLight(relight);
}

void VoxelWorld::updateLightForEdit(glm::ivec3 voxel) {
    const std::uint8_t oldLevel = voxelLight(voxel);
    if (oldLevel > 0) relightAfterRemoval(voxel, oldLevel);

    std::queue<glm::ivec3> open;
    const std::uint8_t emission = blockEmission(blockAt(voxel));
    if (emission > 0) {
        setVoxelLight(voxel, emission);
        open.push(voxel);
    }
    // If the edit opened air (e.g. a solid was removed), let bright neighbors
    // flow back into the newly exposed voxel.
    for (const glm::ivec3& d : kFaceDirs) {
        const glm::ivec3 n = voxel + d;
        if (voxelLight(n) > 1) open.push(n);
    }
    propagateLight(open);
}

void VoxelWorld::computeChunkLightOnLoad(ChunkPos pos) {
    Chunk* chunk = chunkAtMut(pos);
    if (!chunk) return;
    const glm::ivec3 base{pos.x * kChunkSize, pos.y * kChunkSize, pos.z * kChunkSize};

    std::queue<glm::ivec3> open;
    // Seed every emissive voxel in the chunk at its emission level.
    for (int y = 0; y < kChunkSize; ++y)
        for (int z = 0; z < kChunkSize; ++z)
            for (int x = 0; x < kChunkSize; ++x) {
                const std::uint8_t emission = blockEmission(chunk->at(x, y, z));
                if (emission > 0) {
                    chunk->setLight(x, y, z, emission);
                    open.push(base + glm::ivec3(x, y, z));
                }
            }

    // Bleed light in from already-loaded neighbors: seed each lit voxel that sits
    // just across one of this chunk's six boundary planes; propagateLight then
    // spills it one step (level-1) into the fresh chunk and onward.
    const auto seedNeighbor = [&](glm::ivec3 nWorld) {
        if (voxelLight(nWorld) > 1) open.push(nWorld);
    };
    for (int a = 0; a < kChunkSize; ++a)
        for (int b = 0; b < kChunkSize; ++b) {
            seedNeighbor(base + glm::ivec3(-1, a, b));
            seedNeighbor(base + glm::ivec3(kChunkSize, a, b));
            seedNeighbor(base + glm::ivec3(a, -1, b));
            seedNeighbor(base + glm::ivec3(a, kChunkSize, b));
            seedNeighbor(base + glm::ivec3(a, b, -1));
            seedNeighbor(base + glm::ivec3(a, b, kChunkSize));
        }
    propagateLight(open);
}

void VoxelWorld::update(glm::vec3 playerPos, JobQueue& jobs) {
    const ChunkPos center = voxelToChunk(worldToVoxel(playerPos));

    for (int dy = -kStreamRadiusV; dy <= kStreamRadiusV; ++dy)
        for (int dz = -kStreamRadiusH; dz <= kStreamRadiusH; ++dz)
            for (int dx = -kStreamRadiusH; dx <= kStreamRadiusH; ++dx)
                ensureChunk({center.x + dx, center.y + dy, center.z + dz});

    for (auto it = m_chunks.begin(); it != m_chunks.end();) {
        const ChunkPos p = it->first;
        const bool out = std::abs(p.x - center.x) > kStreamRadiusH + kUnloadPad ||
                         std::abs(p.z - center.z) > kStreamRadiusH + kUnloadPad ||
                         std::abs(p.y - center.y) > kStreamRadiusV + kUnloadPad;
        // Erasing while a mesh job is in flight is safe: the job owns a copy.
        if (out) {
            if (m_chunkUnloaded) m_chunkUnloaded(p);
            it = m_chunks.erase(it);
        } else {
            ++it;
        }
    }

    for (auto& [pos, chunk] : m_chunks) {
        if (chunk->dirty() && !m_inFlight.contains(pos)) enqueueMeshJob(pos, *chunk, jobs);
    }
}

void VoxelWorld::enqueueMeshJob(ChunkPos pos, Chunk& chunk, JobQueue& jobs) {
    // Threading law: workers may touch only their job's own inputs, and the
    // main thread keeps mutating live chunks, so the job gets a full by-value
    // snapshot of the chunk and its six neighbors (~450 KB per job). shared_ptr
    // because std::function requires a copyable callable.
    struct Snapshot {
        Chunk chunk;
        std::array<std::optional<Chunk>, 6> neighbors;
    };
    auto snap = std::make_shared<Snapshot>();
    snap->chunk = chunk;
    for (std::size_t i = 0; i < 6; ++i) {
        if (const Chunk* n = chunkAt(offsetChunk(pos, kFaceDirs[i]))) snap->neighbors[i] = *n;
    }
    chunk.clearDirty();
    m_inFlight.insert(pos);

    const BlockRegistry* registry = &m_blocks; // immutable once streaming runs
    jobs.enqueue([snap, registry, pos, &jobs, this] {
        std::array<const Chunk*, 6> neighborPtrs{};
        for (std::size_t i = 0; i < 6; ++i)
            neighborPtrs[i] = snap->neighbors[i] ? &*snap->neighbors[i] : nullptr;
        auto result =
            std::make_shared<ChunkMeshData>(buildChunkMesh(snap->chunk, neighborPtrs, *registry));
        jobs.post([this, pos, result] { onMeshDone(pos, std::move(*result)); });
    });
}

void VoxelWorld::onMeshDone(ChunkPos pos, ChunkMeshData mesh) {
    m_inFlight.erase(pos);
    if (!m_chunks.contains(pos)) return; // unloaded while meshing; drop the result
    if (m_meshReady) m_meshReady(pos, std::move(mesh));
    // If the chunk was edited while in flight it is dirty again and no longer
    // in m_inFlight, so the next update() re-queues it.
}

void VoxelWorld::setGenerator(std::function<void(Chunk&, ChunkPos)> generator) {
    m_generator = std::move(generator);
}

void VoxelWorld::setMeshReadyCallback(std::function<void(ChunkPos, ChunkMeshData)> callback) {
    m_meshReady = std::move(callback);
}

void VoxelWorld::setChunkUnloadedCallback(std::function<void(ChunkPos)> callback) {
    m_chunkUnloaded = std::move(callback);
}

std::optional<VoxelWorld::RayHit> VoxelWorld::raycast(glm::vec3 origin, glm::vec3 dir,
                                                      float maxDist) const {
    const float dirLen = glm::length(dir);
    if (dirLen < 1e-8f || maxDist <= 0.f) return std::nullopt;
    const glm::vec3 d = dir / dirLen;

    // Amanatides & Woo DDA: cross exactly one voxel boundary per step, always
    // on the axis whose next boundary is nearest. tMax[i] = distance (world
    // meters along d) to the next boundary on axis i; tDelta[i] = distance
    // between boundaries. Axes with ~zero direction stay at +inf and never win.
    const glm::vec3 p = origin / kVoxelSize;
    glm::ivec3 voxel = glm::ivec3(glm::floor(p));

    glm::ivec3 step{0};
    glm::vec3 tMax{std::numeric_limits<float>::infinity()};
    glm::vec3 tDelta{std::numeric_limits<float>::infinity()};
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-8f) continue;
        step[i] = d[i] > 0.f ? 1 : -1;
        tDelta[i] = kVoxelSize / std::abs(d[i]);
        const float boundary = static_cast<float>(voxel[i] + (step[i] > 0 ? 1 : 0));
        tMax[i] = (boundary - p[i]) * kVoxelSize / d[i];
    }

    glm::ivec3 normal{0}; // zero when the ray starts inside a solid voxel
    float t = 0.f;
    for (;;) {
        const BlockId id = blockAt(voxel);
        if (m_blocks.get(id).solid) return RayHit{voxel, normal, t, id};
        const int axis =
            tMax.x <= tMax.y ? (tMax.x <= tMax.z ? 0 : 2) : (tMax.y <= tMax.z ? 1 : 2);
        if (tMax[axis] > maxDist) return std::nullopt;
        t = tMax[axis];
        voxel[axis] += step[axis];
        normal = glm::ivec3{0};
        normal[axis] = -step[axis];
        tMax[axis] += tDelta[axis];
    }
}

} // namespace meat
