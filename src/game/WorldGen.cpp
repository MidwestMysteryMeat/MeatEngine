#include "game/WorldGen.h"

namespace meat {
namespace {

// Deterministic integer hash → [0,1). No std::rand, no state: replays and
// remote peers must reproduce terrain bit-for-bit from (seed, coordinates).
float hashNoise(int x, int z, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u +
                      static_cast<std::uint32_t>(z) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}

// Value noise with bilinear interpolation over a coarse lattice.
float valueNoise(float x, float z, float cellSize, std::uint32_t seed) {
    const float fx = x / cellSize, fz = z / cellSize;
    const int x0 = static_cast<int>(std::floor(fx)), z0 = static_cast<int>(std::floor(fz));
    const float tx = fx - static_cast<float>(x0), tz = fz - static_cast<float>(z0);
    const float sx = tx * tx * (3.f - 2.f * tx), sz = tz * tz * (3.f - 2.f * tz);
    const float a = hashNoise(x0, z0, seed), b = hashNoise(x0 + 1, z0, seed);
    const float c = hashNoise(x0, z0 + 1, seed), d = hashNoise(x0 + 1, z0 + 1, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sz;
}

} // namespace

BlockPalette registerDefaultBlocks(BlockRegistry& blocks) {
    BlockPalette p;
    p.stone = blocks.add({"stone", {1, 1, 1, 1, 1, 1}, true});
    p.dirt = blocks.add({"dirt", {2, 2, 2, 2, 2, 2}, true});
    p.grass = blocks.add({"grass", {4, 4, 3, 2, 4, 4}, true});
    return p;
}

std::function<void(Chunk&, ChunkPos)> makeTerrainGenerator(std::uint32_t seed,
                                                           BlockPalette palette) {
    return [seed, palette](Chunk& chunk, ChunkPos pos) {
        for (int z = 0; z < kChunkSize; ++z) {
            for (int x = 0; x < kChunkSize; ++x) {
                const int wx = pos.x * kChunkSize + x, wz = pos.z * kChunkSize + z;
                const float h = valueNoise(static_cast<float>(wx), static_cast<float>(wz),
                                           24.0f, seed);
                const int surface = 6 + static_cast<int>(h * 6.0f); // world-voxel y of grass
                for (int y = 0; y < kChunkSize; ++y) {
                    const int wy = pos.y * kChunkSize + y;
                    if (wy > surface) break;
                    const BlockId id = wy == surface          ? palette.grass
                                       : wy >= surface - 2    ? palette.dirt
                                                              : palette.stone;
                    chunk.set(x, y, z, id);
                }
            }
        }
        chunk.clearDirty(); // generation isn't an edit; streaming queues the mesh
    };
}

} // namespace meat
