#include "game/WorldGen.h"
#include "engine/core/Log.h"
#include "game/DungeonGen.h"

#include <memory>

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
    p.stone = blocks.add({"stone", {1, 1, 1, 1, 1, 1}, true, 100.0f, 45.0f});
    p.dirt = blocks.add({"dirt", {2, 2, 2, 2, 2, 2}, true, 40.0f, 15.0f});
    p.grass = blocks.add({"grass", {4, 4, 3, 2, 4, 4}, true, 30.0f, 12.0f});
    // Emissive lamp: a solid block whose lightEmission seeds the torch flood-fill
    // at the max level (15). Atlas tile 5 is the warm glow tile (tools/gen_atlas.py).
    p.lamp = blocks.add({"lamp", {5, 5, 5, 5, 5, 5}, true, 20.0f, 8.0f, /*lightEmission*/ 15});
    return p;
}

std::function<void(Chunk&, ChunkPos)> makeTerrainGenerator(std::uint32_t seed,
                                                           BlockPalette palette) {
    // The dungeon is derived from the same seed as the terrain, so every peer
    // carves identical rooms with zero network traffic. shared_ptr because the
    // generator std::function must stay copyable.
    auto dungeon = std::make_shared<DungeonLayout>(DungeonLayout::generate(seed, {}));
    log::info("worldgen: dungeon has {} rooms, entrance shaft at ({}, {}, {})",
              dungeon->rooms().size(), dungeon->entranceTop().x, dungeon->entranceTop().y,
              dungeon->entranceTop().z);

    return [seed, palette, dungeon](Chunk& chunk, ChunkPos pos) {
        const glm::ivec3 chunkLo{pos.x * kChunkSize, pos.y * kChunkSize, pos.z * kChunkSize};
        const auto carveBoxes =
            dungeon->boxesIntersecting(chunkLo, chunkLo + glm::ivec3(kChunkSize - 1));

        for (int z = 0; z < kChunkSize; ++z) {
            for (int x = 0; x < kChunkSize; ++x) {
                const int wx = chunkLo.x + x, wz = chunkLo.z + z;
                const float h = valueNoise(static_cast<float>(wx), static_cast<float>(wz),
                                           24.0f, seed);
                const int surface = 6 + static_cast<int>(h * 6.0f); // world-voxel y of grass
                for (int y = 0; y < kChunkSize; ++y) {
                    const int wy = chunkLo.y + y;
                    if (wy > surface) break;
                    if (!carveBoxes.empty() && dungeon->isAir({wx, wy, wz}, carveBoxes))
                        continue; // dungeon air wins over solid ground
                    const BlockId id = wy == surface          ? palette.grass
                                       : wy >= surface - 2    ? palette.dirt
                                                              : palette.stone;
                    chunk.set(x, y, z, id);
                }
                // Plant a 2-voxel lamp pillar just ahead of the spawn view
                // (camera ~(8,9.6,8) looking -Z) so the block-light glow is
                // framed in --shot. Column chosen ~9 m in front, on the surface.
                if (wx == 16 && wz == -2) {
                    for (int up = 1; up <= 2; ++up) {
                        const int ly = surface + up - chunkLo.y;
                        if (ly >= 0 && ly < kChunkSize) chunk.set(x, ly, z, palette.lamp);
                    }
                }
            }
        }
        chunk.clearDirty(); // generation isn't an edit; streaming queues the mesh
    };
}

} // namespace meat
