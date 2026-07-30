#pragma once
#include "engine/voxel/VoxelWorld.h"

#include <vector>

namespace meat {

// Voxel-native A*: paths directly over the live voxel grid, so any change to the
// world (mined tunnel, blasted wall, player-built bridge) is instantly pathable —
// no navmesh rebuild. Cells are world-voxel coords; a cell is standable when it
// and the cell above are air and the cell below is solid.
//
// Moves: 4 horizontal, step-up 1 voxel, drop up to 4 voxels. Returns standable
// waypoints start→goal (inclusive), or empty when unreachable within maxNodes —
// callers treat empty as "wander/hold", never as an error.
bool isStandable(const VoxelWorld& world, glm::ivec3 cell);
std::vector<glm::ivec3> findPath(const VoxelWorld& world, glm::ivec3 start, glm::ivec3 goal,
                                 int maxNodes = 2048);

// Nearest standable cell to a world position (snaps flying/embedded targets to
// the floor); searches a small vertical window. false if none.
bool snapToStandable(const VoxelWorld& world, glm::vec3 worldPos, glm::ivec3& outCell);

} // namespace meat
