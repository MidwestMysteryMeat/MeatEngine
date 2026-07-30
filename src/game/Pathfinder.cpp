#include "game/Pathfinder.h"

#include <algorithm>
#include <queue>
#include <unordered_map>

namespace meat {
namespace {

std::uint64_t key(glm::ivec3 v) {
    // 21 bits per axis, offset to positive — collision-free within ±1M voxels.
    const auto pack = [](int x) { return static_cast<std::uint64_t>(x + (1 << 20)) & 0x1FFFFF; };
    return pack(v.x) | (pack(v.y) << 21) | (pack(v.z) << 42);
}

int heuristic(glm::ivec3 a, glm::ivec3 b) {
    return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z);
}

bool solid(const VoxelWorld& w, glm::ivec3 v) {
    return w.blockRegistry().get(w.blockAt(v)).solid;
}

} // namespace

bool isStandable(const VoxelWorld& world, glm::ivec3 cell) {
    return !solid(world, cell) && !solid(world, cell + glm::ivec3(0, 1, 0)) &&
           solid(world, cell - glm::ivec3(0, 1, 0));
}

bool snapToStandable(const VoxelWorld& world, glm::vec3 worldPos, glm::ivec3& outCell) {
    const glm::ivec3 base = worldToVoxel(worldPos);
    for (int dy = 0; dy >= -6; --dy) { // look down first (falling/standing)
        const glm::ivec3 c = base + glm::ivec3(0, dy, 0);
        if (isStandable(world, c)) {
            outCell = c;
            return true;
        }
    }
    for (int dy = 1; dy <= 2; ++dy) { // then just above (embedded in a slope)
        const glm::ivec3 c = base + glm::ivec3(0, dy, 0);
        if (isStandable(world, c)) {
            outCell = c;
            return true;
        }
    }
    return false;
}

std::vector<glm::ivec3> findPath(const VoxelWorld& world, glm::ivec3 start, glm::ivec3 goal,
                                 int maxNodes) {
    if (!isStandable(world, start) || !isStandable(world, goal)) return {};

    struct Node {
        int f = 0;
        glm::ivec3 cell{0};
    };
    const auto cmp = [](const Node& a, const Node& b) { return a.f > b.f; };
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> open(cmp);
    std::unordered_map<std::uint64_t, int> gScore;
    std::unordered_map<std::uint64_t, glm::ivec3> cameFrom;

    open.push({heuristic(start, goal), start});
    gScore[key(start)] = 0;

    static constexpr glm::ivec3 kDirs[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    int expanded = 0;

    while (!open.empty() && expanded < maxNodes) {
        const Node current = open.top();
        open.pop();
        ++expanded;
        if (current.cell == goal) { // reconstruct
            std::vector<glm::ivec3> path{goal};
            glm::ivec3 c = goal;
            while (c != start) {
                c = cameFrom[key(c)];
                path.push_back(c);
            }
            std::reverse(path.begin(), path.end());
            return path;
        }
        const int g = gScore[key(current.cell)];

        for (const glm::ivec3& d : kDirs) {
            const glm::ivec3 flat = current.cell + d;
            // Candidate landings in preference order: level, step-up 1, drops 1-4.
            glm::ivec3 candidates[6] = {flat,
                                        flat + glm::ivec3(0, 1, 0),
                                        flat - glm::ivec3(0, 1, 0),
                                        flat - glm::ivec3(0, 2, 0),
                                        flat - glm::ivec3(0, 3, 0),
                                        flat - glm::ivec3(0, 4, 0)};
            for (const glm::ivec3& next : candidates) {
                if (!isStandable(world, next)) continue;
                // Step-up needs headroom above the current cell to move diagonally up.
                if (next.y > current.cell.y &&
                    solid(world, current.cell + glm::ivec3(0, 2, 0)))
                    break;
                const int stepCost = 10 + std::abs(next.y - current.cell.y) * 2;
                const int ng = g + stepCost;
                const std::uint64_t nk = key(next);
                if (const auto it = gScore.find(nk); it != gScore.end() && it->second <= ng)
                    continue; // THIS cell was reached cheaper; lower landings differ
                gScore[nk] = ng;
                cameFrom[nk] = current.cell;
                open.push({ng + heuristic(next, goal) * 10, next});
                break; // take the first standable landing in this column
            }
        }
    }
    return {}; // unreachable within budget
}

} // namespace meat
