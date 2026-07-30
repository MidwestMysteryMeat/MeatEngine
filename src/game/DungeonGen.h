#pragma once
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace meat {

// Seeded rooms-and-corridors dungeon carved out of the solid underground.
// Layout is a pure function of (params, seed): every peer computes the same
// boxes locally and the chunk generator carves air where a box says so — no
// dungeon data ever travels over the network. Carved space is ordinary voxels:
// fully destructible and buildable like everything else.
struct DungeonParams {
    int roomAttempts = 40;            // placement tries; survivors become rooms
    glm::ivec2 roomSizeXZ{6, 16};     // voxels (0.5 m each): 3-8 m rooms
    glm::ivec2 roomHeightRange{5, 9}; // 2.5-4.5 m ceilings
    int corridorWidth = 2;            // 1 m
    int corridorHeight = 5;           // 2.5 m
    float loopChance = 0.2f;          // extra connections beyond the spanning chain
    int levels = 2;                   // stacked dungeon floors
    int levelGap = 14;                // voxels between floor levels
    glm::ivec3 areaMin{-90, -44, -90};
    glm::ivec3 areaMax{90, -6, 90};
};

class DungeonLayout {
public:
    struct Box {
        glm::ivec3 min{0}, max{0}; // inclusive voxel bounds
        bool contains(glm::ivec3 p) const {
            return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y &&
                   p.z >= min.z && p.z <= max.z;
        }
        bool intersects(glm::ivec3 lo, glm::ivec3 hi) const {
            return min.x <= hi.x && max.x >= lo.x && min.y <= hi.y && max.y >= lo.y &&
                   min.z <= hi.z && max.z >= lo.z;
        }
    };

    static DungeonLayout generate(std::uint32_t seed, const DungeonParams& params);

    // Chunk generators prefilter once per chunk, then test per voxel.
    std::vector<const Box*> boxesIntersecting(glm::ivec3 lo, glm::ivec3 hi) const;
    bool isAir(glm::ivec3 voxel, const std::vector<const Box*>& candidates) const;

    const std::vector<Box>& rooms() const { return m_rooms; }
    glm::ivec3 entranceTop() const { return m_entranceTop; } // surface-level shaft mouth

private:
    std::vector<Box> m_rooms;
    std::vector<Box> m_carves; // rooms + corridors + shafts, the actual air
    glm::ivec3 m_entranceTop{0};
};

} // namespace meat
