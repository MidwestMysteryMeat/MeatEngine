#pragma once
#include "engine/voxel/Block.h"
#include "engine/voxel/Chunk.h"
#include "engine/voxel/ChunkMesher.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace meat {

class JobQueue;

glm::ivec3 worldToVoxel(glm::vec3 p);
ChunkPos voxelToChunk(glm::ivec3 voxel);

struct VoxelEdit {
    glm::ivec3 voxel;
    BlockId block;
};

class VoxelWorld {
public:
    VoxelWorld();

    BlockId blockAt(glm::ivec3 voxel) const;
    bool isChunkLoaded(ChunkPos pos) const { return m_chunks.contains(pos); }
    void setBlock(glm::ivec3 voxel, BlockId block); // marks dirty, records VoxelEdit
    void update(glm::vec3 playerPos, JobQueue& jobs); // stream in/out, enqueue remesh

    struct RayHit {
        glm::ivec3 voxel;
        glm::ivec3 normal;
        float t;
        BlockId block;
    };
    std::optional<RayHit> raycast(glm::vec3 origin, glm::vec3 dir, float maxDist) const;

    void setGenerator(std::function<void(Chunk&, ChunkPos)> generator);
    void setMeshReadyCallback(std::function<void(ChunkPos, ChunkMeshData)> callback);
    void setChunkUnloadedCallback(std::function<void(ChunkPos)> callback);
    // Drop every loaded chunk + the edit overlay (B4 New Map). Fires the unload
    // callback per chunk so physics/navmesh colliders are torn down first.
    void clearWorld();

    BlockRegistry& blockRegistry() { return m_blocks; }
    const BlockRegistry& blockRegistry() const { return m_blocks; }

    // Persistent edit overlay: every setBlock is recorded per chunk (last write
    // per voxel wins) and re-applied when a streamed-out chunk regenerates —
    // without this, edits silently revert on reload. Also the save payload.
    using ChunkEdits = std::unordered_map<std::uint16_t, BlockId>; // key: local index
    const std::unordered_map<ChunkPos, ChunkEdits>& editOverlay() const { return m_overlay; }

private:
    Chunk& ensureChunk(ChunkPos pos);
    const Chunk* chunkAt(ChunkPos pos) const;
    Chunk* chunkAtMut(ChunkPos pos);
    void enqueueMeshJob(ChunkPos pos, Chunk& chunk, JobQueue& jobs);
    void onMeshDone(ChunkPos pos, ChunkMeshData mesh);

    // --- Block-light flood-fill (torch light) --------------------------------
    // Runs on the main/edit thread only; writes each affected voxel's light into
    // its owning loaded chunk and marks that chunk dirty so it re-meshes. The
    // mesher then reads the light off its by-value chunk snapshot. Skylight is
    // out of scope — this is emissive-block light only. Technique: the standard
    // increasing-BFS + removal-BFS block-light spread (as in Luanti/Minetest's
    // voxel lighting; ideas only, original code here).
    std::uint8_t voxelLight(glm::ivec3 v) const;
    void setVoxelLight(glm::ivec3 v, std::uint8_t level); // sets + marks chunk dirty
    bool voxelSolid(glm::ivec3 v) const;
    std::uint8_t blockEmission(BlockId id) const; // 0 for ids the registry lacks
    void propagateLight(std::queue<glm::ivec3>& open);    // spread from seeded voxels
    void relightAfterRemoval(glm::ivec3 voxel, std::uint8_t oldLevel);
    void computeChunkLightOnLoad(ChunkPos pos);           // seed emitters + neighbor bleed-in
    void updateLightForEdit(glm::ivec3 voxel);            // add/remove a source or blocker

    std::unordered_map<ChunkPos, std::unique_ptr<Chunk>> m_chunks;
    std::unordered_set<ChunkPos> m_inFlight;
    std::unordered_map<ChunkPos, ChunkEdits> m_overlay;
    std::function<void(Chunk&, ChunkPos)> m_generator;
    std::function<void(ChunkPos, ChunkMeshData)> m_meshReady;
    std::function<void(ChunkPos)> m_chunkUnloaded;
    BlockRegistry m_blocks;
};

} // namespace meat
