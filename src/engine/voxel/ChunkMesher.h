#pragma once
#include "engine/voxel/Block.h"
#include "engine/voxel/Chunk.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_precision.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace meat {

struct VoxelVertex {
    glm::vec3 pos;
    glm::i8vec3 normal;
    glm::vec2 uv;
    std::uint16_t tex;
    std::uint8_t light; // block-light level 0..15 of the air voxel this face faces
};

struct ChunkMeshData {
    std::vector<VoxelVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// PURE, thread-safe: reads only its arguments, runs on workers. Positions are
// local chunk-space meters. Neighbor order +X,-X,+Y,-Y,+Z,-Z; null = air.
ChunkMeshData buildChunkMesh(const Chunk& chunk, const std::array<const Chunk*, 6>& neighbors,
                             const BlockRegistry& registry);

} // namespace meat
