#include "engine/voxel/ChunkMesher.h"

#include <cstddef>

namespace meat {
namespace {

struct FaceSpec {
    glm::ivec3 dir;
    glm::i8vec3 normal;
    std::array<glm::ivec3, 4> corners; // CCW seen from outside the face
};

// Order matches BlockDef::faceTex and the neighbor array: +X,-X,+Y,-Y,+Z,-Z.
const std::array<FaceSpec, 6> kFaces = {{
    {{1, 0, 0}, {1, 0, 0}, {{{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}}}},
    {{-1, 0, 0}, {-1, 0, 0}, {{{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}}},
    {{0, 1, 0}, {0, 1, 0}, {{{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}}}},
    {{0, -1, 0}, {0, -1, 0}, {{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}}},
    {{0, 0, 1}, {0, 0, 1}, {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}}},
    {{0, 0, -1}, {0, 0, -1}, {{{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}}},
}};

const std::array<glm::vec2, 4> kCornerUv = {{{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}}};

BlockId blockAcrossFace(const Chunk& chunk, const std::array<const Chunk*, 6>& neighbors,
                        int face, glm::ivec3 p) {
    if (p.x >= 0 && p.x < kChunkSize && p.y >= 0 && p.y < kChunkSize && p.z >= 0 &&
        p.z < kChunkSize)
        return chunk.at(p.x, p.y, p.z);
    const Chunk* n = neighbors[static_cast<std::size_t>(face)];
    if (!n) return 0;
    // p is out of bounds by exactly one voxel along the face axis; shift it
    // back one chunk to get the neighbor-local coordinate.
    const glm::ivec3 local = p - kFaces[static_cast<std::size_t>(face)].dir * kChunkSize;
    return n->at(local.x, local.y, local.z);
}

void emitFace(ChunkMeshData& mesh, glm::ivec3 voxel, int face, std::uint16_t tex) {
    const FaceSpec& spec = kFaces[static_cast<std::size_t>(face)];
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (std::size_t c = 0; c < 4; ++c) {
        VoxelVertex vert;
        vert.pos = glm::vec3(voxel + spec.corners[c]) * kVoxelSize;
        vert.normal = spec.normal;
        vert.uv = kCornerUv[c];
        vert.tex = tex;
        mesh.vertices.push_back(vert);
    }
    mesh.indices.insert(mesh.indices.end(),
                        {base, base + 1, base + 2, base, base + 2, base + 3});
}

} // namespace

ChunkMeshData buildChunkMesh(const Chunk& chunk, const std::array<const Chunk*, 6>& neighbors,
                             const BlockRegistry& registry) {
    ChunkMeshData mesh;
    for (int y = 0; y < kChunkSize; ++y) {
        for (int z = 0; z < kChunkSize; ++z) {
            for (int x = 0; x < kChunkSize; ++x) {
                const BlockId id = chunk.at(x, y, z);
                if (id == 0) continue;
                const BlockDef& def = registry.get(id);
                if (!def.solid) continue;
                for (int face = 0; face < 6; ++face) {
                    const glm::ivec3 across =
                        glm::ivec3(x, y, z) + kFaces[static_cast<std::size_t>(face)].dir;
                    const BlockId other = blockAcrossFace(chunk, neighbors, face, across);
                    if (registry.get(other).solid) continue;
                    emitFace(mesh, {x, y, z}, face,
                             def.faceTex[static_cast<std::size_t>(face)]);
                }
            }
        }
    }
    return mesh;
}

} // namespace meat
