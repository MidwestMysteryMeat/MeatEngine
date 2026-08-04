#pragma once

// Small helpers shared across the ServerSim translation units (ServerSim.cpp,
// ServerSimEffects.cpp, ServerSimCombat.cpp, …). Kept here — not in ServerSim.h —
// so they stay implementation detail rather than public class surface.

#include "engine/voxel/Chunk.h" // kVoxelSize (runtime block scale)

#include <glm/glm.hpp> // vec3 + dot/length/clamp for raySegmentDistance

namespace meat {

// The authoritative server timestep: 60 Hz fixed. Shared so every ServerSim
// translation unit ticks on the same constant.
constexpr float kFixedDtServer = 1.0f / 60.0f;

// Combat tuning shared between processCombat (ServerSim.cpp) and the hitscan /
// projectile resolution (ServerSimCombat.cpp).
constexpr float kHitscanRange = 60.0f;
constexpr float kReloadSeconds = 1.6f;       // time to swap a magazine (H3)
constexpr float kBurstIntraInterval = 0.06f; // fast cadence between burst rounds (H2)
constexpr float kCapsuleRadius = 0.35f;      // keep in sync with CharacterTuning

// Distance between a ray segment [ro, ro + rd*range] and segment [a, b];
// tRayOut = distance along the ray at the closest approach. Standard clamped
// closest-point-of-two-segments; rd must be unit length. `inline` so multiple
// ServerSim translation units can include it without an ODR clash.
inline float raySegmentDistance(glm::vec3 ro, glm::vec3 rd, float range, glm::vec3 a,
                                glm::vec3 b, float& tRayOut) {
    const glm::vec3 u = rd * range, v = b - a, w0 = ro - a;
    const float A = glm::dot(u, u), B = glm::dot(u, v), C = glm::dot(v, v);
    const float D = glm::dot(u, w0), E = glm::dot(v, w0);
    const float denom = A * C - B * B;
    float s = denom > 1e-6f ? glm::clamp((B * E - C * D) / denom, 0.0f, 1.0f) : 0.0f;
    float t = C > 1e-6f ? glm::clamp((B * s + E) / C, 0.0f, 1.0f) : 0.0f;
    s = A > 1e-6f ? glm::clamp((B * t - D) / A, 0.0f, 1.0f) : 0.0f;
    tRayOut = s * range;
    return glm::length((ro + u * s) - (a + v * t));
}

// Default player spawn in world metres — voxel cell (16,16,16), scaled by the
// runtime voxel size so the pad stays above the surface at any block scale.
inline glm::vec3 defaultSpawnPos() {
    return glm::vec3(16.0f, 16.0f, 16.0f) * kVoxelSize;
}

} // namespace meat
