#pragma once

// Small helpers shared across the ServerSim translation units (ServerSim.cpp,
// ServerSimEffects.cpp, ServerSimCombat.cpp, …). Kept here — not in ServerSim.h —
// so they stay implementation detail rather than public class surface.

#include "engine/voxel/Chunk.h" // kVoxelSize (runtime block scale)

#include <glm/vec3.hpp>

namespace meat {

// Default player spawn in world metres — voxel cell (16,16,16), scaled by the
// runtime voxel size so the pad stays above the surface at any block scale.
inline glm::vec3 defaultSpawnPos() {
    return glm::vec3(16.0f, 16.0f, 16.0f) * kVoxelSize;
}

} // namespace meat
