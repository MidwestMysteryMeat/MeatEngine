#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace meat::cull {

// Six frustum planes (left, right, bottom, top, near, far), each normalized with
// the normal pointing INTO the frustum: a point p is inside when
// dot(plane.xyz, p) + plane.w >= 0 for all six.
using Frustum = std::array<glm::vec4, 6>;

// Extract the frustum planes from a view-projection matrix (Gribb-Hartmann).
// Assumes GL clip space (z in [-1, 1]).
Frustum extractPlanes(const glm::mat4& viewProj);

// True if the bounding sphere is at least partially inside the frustum.
bool sphereInFrustum(const Frustum& f, glm::vec3 center, float radius);

// True if the sphere lies entirely beyond maxDist from camPos (distance cull).
// A non-positive maxDist disables the test (never culls).
bool beyondDistance(glm::vec3 camPos, glm::vec3 center, float radius, float maxDist);

// One drawable for budget selection: where it is and how many triangles it costs.
struct BudgetItem {
    glm::vec3 center{0.0f};
    std::uint32_t triangles = 0;
};

// PSX-profile budget cull: walk items nearest-first and keep each while it fits
// under BOTH caps (draw count and cumulative triangles); stop at the first that
// would overflow either budget. Returns the kept items' original indices, nearest
// first. maxDraws/maxTriangles == 0 means "unbounded" for that axis.
std::vector<std::uint32_t> selectWithinBudget(std::span<const BudgetItem> items,
                                              glm::vec3 camPos, std::uint32_t maxDraws,
                                              std::uint32_t maxTriangles);

} // namespace meat::cull
