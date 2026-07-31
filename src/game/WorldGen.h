#pragma once
#include "engine/voxel/VoxelWorld.h"
#include "game/GameRules.h"

#include <cstdint>
#include <functional>

namespace meat {

// Block ids registered identically on server and every client — chunk content
// is derived, not transmitted, so both sides must agree exactly.
struct BlockPalette {
    BlockId stone = 0, dirt = 0, grass = 0, lamp = 0;
};

BlockPalette registerDefaultBlocks(BlockRegistry& blocks);

// Pure function of (seed, chunk position): the same seed + terrain mode yields the same world on
// every peer (both server and client build it locally — mode travels in the rules flags byte).
std::function<void(Chunk&, ChunkPos)> makeTerrainGenerator(
    std::uint32_t seed, BlockPalette palette,
    GameRules::Terrain terrain = GameRules::Terrain::Normal);

} // namespace meat
