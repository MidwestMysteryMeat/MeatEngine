#pragma once
#include "engine/voxel/VoxelWorld.h"

#include <cstdint>
#include <functional>

namespace meat {

// Block ids registered identically on server and every client — chunk content
// is derived, not transmitted, so both sides must agree exactly.
struct BlockPalette {
    BlockId stone = 0, dirt = 0, grass = 0;
};

BlockPalette registerDefaultBlocks(BlockRegistry& blocks);

// Pure function of (seed, chunk position): the same seed yields the same world
// on every peer. Placeholder rolling terrain until dungeons/rooms land.
std::function<void(Chunk&, ChunkPos)> makeTerrainGenerator(std::uint32_t seed,
                                                           BlockPalette palette);

} // namespace meat
