#pragma once
#include "engine/voxel/Block.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <functional>

namespace meat {

inline constexpr int kChunkSize = 32;
inline constexpr float kDefaultVoxelSize = 0.5f;
// Metres per voxel. DEV-CONFIGURABLE (GameRules::voxelSize, --voxelsize, game.json
// "voxelSize"): set ONCE at startup before any world/mesh/physics/job runs, then treated
// as read-only, so concurrent meshing jobs see a stable value. It only scales the
// world<->voxel conversions; the 32^3 chunk DIMENSION (kChunkSize) is unaffected. Smaller
// than 0.5 = finer detail (more chunks/mesh); larger (e.g. >1 m, chunkier than Minecraft)
// = cheaper, blockier. inline (not constexpr) precisely so it can be a runtime choice.
inline float kVoxelSize = kDefaultVoxelSize;
// Chunk edge length in metres — derived, so it tracks a dev-chosen voxel size everywhere.
inline float chunkWorldSize() { return static_cast<float>(kChunkSize) * kVoxelSize; }

struct ChunkPos {
    int x, y, z;
    auto operator<=>(const ChunkPos&) const = default;
};

class Chunk {
public:
    BlockId at(int x, int y, int z) const {
        assert(inBounds(x, y, z));
        return m_blocks[index(x, y, z)];
    }

    void set(int x, int y, int z, BlockId id) {
        assert(inBounds(x, y, z));
        m_blocks[index(x, y, z)] = id;
        m_dirty = true;
    }

    // Per-voxel block-light level, 0..15. Computed on the main/edit thread by
    // VoxelWorld's flood-fill; the mesher only READS it (via the by-value chunk
    // snapshot), keeping meshing pure and worker-safe.
    std::uint8_t lightAt(int x, int y, int z) const {
        assert(inBounds(x, y, z));
        return m_light[index(x, y, z)];
    }

    void setLight(int x, int y, int z, std::uint8_t level) {
        assert(inBounds(x, y, z));
        m_light[index(x, y, z)] = level;
    }

    bool dirty() const { return m_dirty; }
    void markDirty() { m_dirty = true; }
    void clearDirty() { m_dirty = false; }

    static std::size_t index(int x, int y, int z) {
        return static_cast<std::size_t>(x + z * kChunkSize + y * kChunkSize * kChunkSize);
    }

private:
    static bool inBounds(int x, int y, int z) {
        return x >= 0 && x < kChunkSize && y >= 0 && y < kChunkSize && z >= 0 && z < kChunkSize;
    }

    std::array<BlockId, static_cast<std::size_t>(kChunkSize) * kChunkSize * kChunkSize> m_blocks{};
    std::array<std::uint8_t, static_cast<std::size_t>(kChunkSize) * kChunkSize * kChunkSize>
        m_light{};
    bool m_dirty = false;
};

} // namespace meat

template <> struct std::hash<meat::ChunkPos> {
    std::size_t operator()(const meat::ChunkPos& p) const noexcept {
        // FNV-style mix; chunk coords are small so low-bit spread matters most.
        std::uint64_t h = 0xCBF29CE484222325ull;
        h = (h ^ static_cast<std::uint32_t>(p.x)) * 0x100000001B3ull;
        h = (h ^ static_cast<std::uint32_t>(p.y)) * 0x100000001B3ull;
        h = (h ^ static_cast<std::uint32_t>(p.z)) * 0x100000001B3ull;
        return static_cast<std::size_t>(h);
    }
};
