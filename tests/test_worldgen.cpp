// Worldgen determinism: chunk content is DERIVED from (seed, position), never
// transmitted, so server and every client must generate byte-identical voxels
// from the same seed. If this drifts, players silently see different worlds and
// collide with geometry that isn't there. These tests pin that invariant.

#include "Harness.h"

#include "engine/voxel/Block.h"
#include "engine/voxel/Chunk.h"
#include "game/WorldGen.h"

#include <cstdio>

namespace {

using meattest::check;

// Generate one chunk exactly as a fresh peer would: its own registry + generator
// for `seed`, so this also proves block-id registration is order-stable.
void genChunk(std::uint32_t seed, meat::GameRules::Terrain terrain, meat::ChunkPos pos,
              meat::Chunk& out) {
    meat::BlockRegistry reg;
    const meat::BlockPalette pal = meat::registerDefaultBlocks(reg);
    auto gen = meat::makeTerrainGenerator(seed, pal, terrain);
    gen(out, pos);
}

bool chunksEqual(const meat::Chunk& a, const meat::Chunk& b) {
    for (int y = 0; y < meat::kChunkSize; ++y)
        for (int z = 0; z < meat::kChunkSize; ++z)
            for (int x = 0; x < meat::kChunkSize; ++x)
                if (a.at(x, y, z) != b.at(x, y, z)) return false;
    return true;
}

int solidCount(const meat::Chunk& c) {
    int n = 0;
    for (int y = 0; y < meat::kChunkSize; ++y)
        for (int z = 0; z < meat::kChunkSize; ++z)
            for (int x = 0; x < meat::kChunkSize; ++x)
                if (c.at(x, y, z) != meat::BlockId{0}) ++n;
    return n;
}

void testSameSeedIsIdentical() {
    std::printf("same seed + position generates byte-identical chunks\n");
    const meat::ChunkPos positions[] = {{0, 0, 0}, {1, 0, -1}, {3, 0, 2}, {-2, 0, 5}};
    bool allMatch = true, anyContent = false;
    for (const meat::ChunkPos& p : positions) {
        meat::Chunk a, b;
        genChunk(1337u, meat::GameRules::Terrain::Normal, p, a);
        genChunk(1337u, meat::GameRules::Terrain::Normal, p, b);
        if (!chunksEqual(a, b)) allMatch = false;
        if (solidCount(a) > 0) anyContent = true;
    }
    check(allMatch, "every position is reproduced exactly from the same seed");
    check(anyContent, "the generator actually produced terrain (test isn't vacuous)");
}

void testDifferentSeedDiffers() {
    std::printf("a different seed yields a different world\n");
    // Across a handful of surface chunks at least one must differ, or the seed
    // isn't feeding the noise — this guards against a constant/ignored-seed regression.
    bool anyDifferent = false;
    const meat::ChunkPos surface[] = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {2, 0, 2}};
    for (const meat::ChunkPos& p : surface) {
        meat::Chunk a, b;
        genChunk(1u, meat::GameRules::Terrain::Normal, p, a);
        genChunk(999999u, meat::GameRules::Terrain::Normal, p, b);
        if (!chunksEqual(a, b)) { anyDifferent = true; break; }
    }
    check(anyDifferent, "the seed actually drives generation");
}

void testSuperflatIsDeterministic() {
    std::printf("superflat generates deterministically\n");
    // Superflat flattens the surface but still carves a seeded dungeon, so it is
    // reproducible for a given seed (the MP-parity invariant) but not seed-free.
    meat::Chunk a, b;
    genChunk(42u, meat::GameRules::Terrain::Superflat, {0, 0, 0}, a);
    genChunk(42u, meat::GameRules::Terrain::Superflat, {0, 0, 0}, b);
    check(chunksEqual(a, b), "same seed reproduces superflat exactly");
    check(solidCount(a) > 0, "superflat generated ground");
}

void testVoidIsSparseAndDeterministic() {
    std::printf("void is a near-empty canvas (spawn pad only) and deterministic\n");
    // Void is a blank canvas with just a small grass pad under spawn so the player
    // doesn't fall forever — not fully empty, but far sparser than a terrain chunk.
    meat::Chunk a, b;
    genChunk(1337u, meat::GameRules::Terrain::Void, {0, 0, 0}, a);
    genChunk(1337u, meat::GameRules::Terrain::Void, {0, 0, 0}, b);
    check(chunksEqual(a, b), "same seed reproduces void exactly");
    meat::Chunk normal;
    genChunk(1337u, meat::GameRules::Terrain::Normal, {0, 0, 0}, normal);
    check(solidCount(a) > 0 && solidCount(a) < solidCount(normal),
          "void has only the spawn pad — far sparser than a terrain chunk");
}

} // namespace

namespace meattest {

void runWorldgen() {
    testSameSeedIsIdentical();
    testDifferentSeedDiffers();
    testSuperflatIsDeterministic();
    testVoidIsSparseAndDeterministic();
}

} // namespace meattest
