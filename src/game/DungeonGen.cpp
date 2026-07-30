#include "game/DungeonGen.h"

#include <algorithm>

namespace meat {
namespace {

// Hand-rolled SplitMix64: std::uniform_int_distribution is not guaranteed to
// produce identical sequences across standard libraries, and clients on other
// platforms must derive bit-identical layouts from the same seed.
struct SplitMix {
    std::uint64_t state;
    std::uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int range(int lo, int hi) { // inclusive
        return lo + static_cast<int>(next() % static_cast<std::uint64_t>(hi - lo + 1));
    }
    float unit() { return static_cast<float>(next() >> 40) / 16777216.0f; }
};

glm::ivec3 center(const DungeonLayout::Box& b) {
    return {(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, (b.min.z + b.max.z) / 2};
}

} // namespace

DungeonLayout DungeonLayout::generate(std::uint32_t seed, const DungeonParams& p) {
    DungeonLayout layout;
    SplitMix rng{0xD1CE0000ull ^ seed};

    // 1. Rooms: rejection-place axis-aligned boxes on discrete levels.
    for (int attempt = 0; attempt < p.roomAttempts; ++attempt) {
        const int level = rng.range(0, p.levels - 1);
        const int floorY = p.areaMin.y + level * p.levelGap;
        const glm::ivec3 size{rng.range(p.roomSizeXZ.x, p.roomSizeXZ.y),
                              rng.range(p.roomHeightRange.x, p.roomHeightRange.y),
                              rng.range(p.roomSizeXZ.x, p.roomSizeXZ.y)};
        const glm::ivec3 min{rng.range(p.areaMin.x, p.areaMax.x - size.x), floorY,
                             rng.range(p.areaMin.z, p.areaMax.z - size.z)};
        const Box room{min, min + size - glm::ivec3(1)};

        const glm::ivec3 pad{2}; // keep at least 1 m of rock between rooms
        const bool overlaps = std::any_of(
            layout.m_rooms.begin(), layout.m_rooms.end(),
            [&](const Box& other) { return other.intersects(room.min - pad, room.max + pad); });
        if (!overlaps) layout.m_rooms.push_back(room);
    }
    if (layout.m_rooms.empty()) return layout;

    // 2. Connections: chain each room to its nearest already-connected room
    //    (guarantees a connected dungeon), then extra edges for loops.
    const auto addCorridor = [&](const Box& a, const Box& b) {
        const glm::ivec3 ca = center(a), cb = center(b);
        const int w = p.corridorWidth, h = p.corridorHeight;
        const int floorA = a.min.y, floorB = b.min.y;
        // L-shape on A's floor level: X leg then Z leg; a vertical shaft at the
        // corner joins differing levels (stairs come later — it's a drop/climb).
        const glm::ivec3 corner{cb.x, floorA, ca.z};
        const auto leg = [&](glm::ivec3 from, glm::ivec3 to, int floorY) {
            const glm::ivec3 lo{std::min(from.x, to.x), floorY, std::min(from.z, to.z)};
            const glm::ivec3 hi{std::max(from.x, to.x) + w - 1, floorY + h - 1,
                                std::max(from.z, to.z) + w - 1};
            layout.m_carves.push_back({lo, hi});
        };
        leg({ca.x, 0, ca.z}, {corner.x, 0, corner.z}, floorA);
        leg({corner.x, 0, corner.z}, {cb.x, 0, cb.z}, floorB);
        if (floorA != floorB) {
            const int lo = std::min(floorA, floorB), hi = std::max(floorA, floorB) + h - 1;
            layout.m_carves.push_back(
                {{corner.x, lo, corner.z}, {corner.x + w - 1, hi, corner.z + w - 1}});
        }
    };

    for (std::size_t i = 1; i < layout.m_rooms.size(); ++i) {
        std::size_t nearest = 0;
        std::int64_t bestD = INT64_MAX;
        const glm::ivec3 ci = center(layout.m_rooms[i]);
        for (std::size_t j = 0; j < i; ++j) {
            const glm::ivec3 d = center(layout.m_rooms[j]) - ci;
            const std::int64_t dist = std::int64_t(d.x) * d.x + std::int64_t(d.y) * d.y +
                                      std::int64_t(d.z) * d.z;
            if (dist < bestD) {
                bestD = dist;
                nearest = j;
            }
        }
        addCorridor(layout.m_rooms[i], layout.m_rooms[nearest]);
        if (rng.unit() < p.loopChance && i >= 2)
            addCorridor(layout.m_rooms[i],
                        layout.m_rooms[rng.range(0, static_cast<int>(i) - 1)]);
    }

    // 3. Entrance: shaft from the room nearest the world spawn up past the
    //    tallest terrain (surface max is ~12 voxels; +16 clears it with margin).
    std::size_t entrance = 0;
    std::int64_t bestD = INT64_MAX;
    for (std::size_t i = 0; i < layout.m_rooms.size(); ++i) {
        const glm::ivec3 c = center(layout.m_rooms[i]);
        const std::int64_t dist = std::int64_t(c.x) * c.x + std::int64_t(c.z) * c.z;
        if (dist < bestD) {
            bestD = dist;
            entrance = i;
        }
    }
    const glm::ivec3 ec = center(layout.m_rooms[entrance]);
    const int shaftTop = 16;
    layout.m_carves.push_back(
        {{ec.x, layout.m_rooms[entrance].min.y, ec.z}, {ec.x + 1, shaftTop, ec.z + 1}});
    layout.m_entranceTop = {ec.x, shaftTop, ec.z};

    // Rooms are carved space too.
    layout.m_carves.insert(layout.m_carves.end(), layout.m_rooms.begin(),
                           layout.m_rooms.end());
    return layout;
}

std::vector<const DungeonLayout::Box*> DungeonLayout::boxesIntersecting(glm::ivec3 lo,
                                                                        glm::ivec3 hi) const {
    std::vector<const Box*> out;
    for (const Box& b : m_carves)
        if (b.intersects(lo, hi)) out.push_back(&b);
    return out;
}

bool DungeonLayout::isAir(glm::ivec3 voxel, const std::vector<const Box*>& candidates) const {
    for (const Box* b : candidates)
        if (b->contains(voxel)) return true;
    return false;
}

} // namespace meat
