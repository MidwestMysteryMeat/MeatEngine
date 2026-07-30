#include "engine/voxel/ChunkMesher.h"

#include <cstddef>
#include <cstdint>

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

// Block-light level of the (air) voxel a face is exposed to. Same in-bounds /
// neighbor-fallback logic as blockAcrossFace; light of an unloaded neighbor
// reads as 0 (dark), so chunk seams gracefully fade rather than glow.
std::uint8_t lightAcrossFace(const Chunk& chunk, const std::array<const Chunk*, 6>& neighbors,
                             int face, glm::ivec3 p) {
    if (p.x >= 0 && p.x < kChunkSize && p.y >= 0 && p.y < kChunkSize && p.z >= 0 &&
        p.z < kChunkSize)
        return chunk.lightAt(p.x, p.y, p.z);
    const Chunk* n = neighbors[static_cast<std::size_t>(face)];
    if (!n) return 0;
    const glm::ivec3 local = p - kFaces[static_cast<std::size_t>(face)].dir * kChunkSize;
    return n->lightAt(local.x, local.y, local.z);
}

// Voxel-coordinate contribution of walking t mask cells along a signed unit
// axis. Negative directions count down from the far edge so that mask cell 0
// always sits where the face's corner table starts — that keeps the merged
// quad's winding and uv orientation identical to the single-voxel case.
glm::ivec3 axisSteps(glm::ivec3 dir, int t) {
    const int along = dir.x + dir.y + dir.z; // +1 or -1: dir is a signed unit axis
    return dir * dir * (along > 0 ? t : kChunkSize - 1 - t);
}

std::size_t cellIndex(int u, int v) {
    return static_cast<std::size_t>(u + v * kChunkSize);
}

void emitQuad(ChunkMeshData& mesh, const FaceSpec& spec, glm::ivec3 baseVoxel, glm::ivec3 uDir,
              glm::ivec3 vDir, int w, int h, std::uint16_t tex, std::uint8_t light) {
    // The quad is the single-voxel corner table stretched w cells along uDir
    // and h cells along vDir; with w == h == 1 it reproduces the old
    // per-voxel vertices exactly, so winding stays CCW from outside.
    const glm::vec3 p0 = glm::vec3(baseVoxel + spec.corners[0]) * kVoxelSize;
    const glm::vec3 uSpan = glm::vec3(uDir) * (kVoxelSize * static_cast<float>(w));
    const glm::vec3 vSpan = glm::vec3(vDir) * (kVoxelSize * static_cast<float>(h));
    const std::array<glm::vec3, 4> pos = {p0, p0 + uSpan, p0 + uSpan + vSpan, p0 + vSpan};

    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (std::size_t c = 0; c < 4; ++c) {
        VoxelVertex vert;
        vert.pos = pos[c];
        vert.normal = spec.normal;
        // uv spans (0..w, 0..h): the atlas shader applies fract() per fragment,
        // so the tile repeats once per voxel across the merged quad.
        vert.uv = kCornerUv[c] * glm::vec2(static_cast<float>(w), static_cast<float>(h));
        vert.tex = tex;
        vert.light = light; // uniform across the quad: light is part of the merge key
        mesh.vertices.push_back(vert);
    }
    mesh.indices.insert(mesh.indices.end(),
                        {base, base + 1, base + 2, base, base + 2, base + 3});
}

} // namespace

ChunkMeshData buildChunkMesh(const Chunk& chunk, const std::array<const Chunk*, 6>& neighbors,
                             const BlockRegistry& registry) {
    ChunkMeshData mesh;

    // Greedy meshing (0fps.net "Meshing in a Minecraft Game", Lysenko). For
    // each of the six face directions we sweep the chunk one slice at a time
    // along the face axis. A slice gets a 32x32 mask holding, per cell, the
    // atlas tile of the face that must be drawn there (solid block with a
    // non-solid block across the face) or kEmpty. The mask is then consumed
    // by merging maximal rectangles of one tile id: extend right along the
    // row while the tile repeats, extend that run downward while every full
    // row still matches, emit one quad for the rectangle, and clear its
    // cells. Merge key = (tile id, block-light level): only faces with the
    // SAME light merge, so the per-voxel torch gradient survives greedy
    // meshing (a lit face never fuses with a dark one of the same tile).
    constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;
    std::array<std::uint32_t, static_cast<std::size_t>(kChunkSize) * kChunkSize> mask;
    std::array<std::uint8_t, static_cast<std::size_t>(kChunkSize) * kChunkSize> lightMask{};

    for (int face = 0; face < 6; ++face) {
        const FaceSpec& spec = kFaces[static_cast<std::size_t>(face)];
        // Mask basis derived from the corner table: cell (u, v) advances one
        // voxel along the table's uv directions, so rectangles in mask space
        // are exactly the voxel spans the emitted quad must cover.
        const glm::ivec3 uDir = spec.corners[1] - spec.corners[0];
        const glm::ivec3 vDir = spec.corners[3] - spec.corners[0];
        const glm::ivec3 sliceAxis = spec.dir * spec.dir;

        for (int s = 0; s < kChunkSize; ++s) {
            bool sliceHasFaces = false;
            for (int v = 0; v < kChunkSize; ++v) {
                for (int u = 0; u < kChunkSize; ++u) {
                    const glm::ivec3 p = sliceAxis * s + axisSteps(uDir, u) + axisSteps(vDir, v);
                    std::uint32_t cell = kEmpty;
                    const BlockId id = chunk.at(p.x, p.y, p.z);
                    if (id != 0 && registry.get(id).solid) {
                        const BlockId other =
                            blockAcrossFace(chunk, neighbors, face, p + spec.dir);
                        if (!registry.get(other).solid) {
                            cell = registry.get(id).faceTex[static_cast<std::size_t>(face)];
                            lightMask[cellIndex(u, v)] =
                                lightAcrossFace(chunk, neighbors, face, p + spec.dir);
                            sliceHasFaces = true;
                        }
                    }
                    mask[cellIndex(u, v)] = cell;
                }
            }
            if (!sliceHasFaces) continue;

            for (int v = 0; v < kChunkSize; ++v) {
                for (int u = 0; u < kChunkSize;) {
                    const std::uint32_t tile = mask[cellIndex(u, v)];
                    if (tile == kEmpty) {
                        ++u;
                        continue;
                    }
                    const std::uint8_t light = lightMask[cellIndex(u, v)];
                    const auto matches = [&](int cu, int cv) {
                        return mask[cellIndex(cu, cv)] == tile &&
                               lightMask[cellIndex(cu, cv)] == light;
                    };
                    int w = 1;
                    while (u + w < kChunkSize && matches(u + w, v)) ++w;
                    int h = 1;
                    while (v + h < kChunkSize) {
                        bool rowMatches = true;
                        for (int k = 0; k < w && rowMatches; ++k)
                            rowMatches = matches(u + k, v + h);
                        if (!rowMatches) break;
                        ++h;
                    }
                    for (int dv = 0; dv < h; ++dv)
                        for (int du = 0; du < w; ++du) mask[cellIndex(u + du, v + dv)] = kEmpty;

                    const glm::ivec3 baseVoxel =
                        sliceAxis * s + axisSteps(uDir, u) + axisSteps(vDir, v);
                    emitQuad(mesh, spec, baseVoxel, uDir, vDir, w, h,
                             static_cast<std::uint16_t>(tile), light);
                    u += w;
                }
            }
        }
    }
    return mesh;
}

} // namespace meat
