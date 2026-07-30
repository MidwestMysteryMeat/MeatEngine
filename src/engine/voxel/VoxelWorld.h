#pragma once
#include "engine/voxel/Block.h"
#include "engine/voxel/Chunk.h"
#include "engine/voxel/ChunkMesher.h"

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <optional>
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

    BlockRegistry& blockRegistry() { return m_blocks; }
    const BlockRegistry& blockRegistry() const { return m_blocks; }

    // Edit log for the save system: append-only until it consumes and clears it.
    const std::vector<VoxelEdit>& edits() const { return m_edits; }
    void clearEdits() { m_edits.clear(); }

private:
    Chunk& ensureChunk(ChunkPos pos);
    const Chunk* chunkAt(ChunkPos pos) const;
    void enqueueMeshJob(ChunkPos pos, Chunk& chunk, JobQueue& jobs);
    void onMeshDone(ChunkPos pos, ChunkMeshData mesh);

    std::unordered_map<ChunkPos, std::unique_ptr<Chunk>> m_chunks;
    std::unordered_set<ChunkPos> m_inFlight;
    std::vector<VoxelEdit> m_edits;
    std::function<void(Chunk&, ChunkPos)> m_generator;
    std::function<void(ChunkPos, ChunkMeshData)> m_meshReady;
    BlockRegistry m_blocks;
};

} // namespace meat
