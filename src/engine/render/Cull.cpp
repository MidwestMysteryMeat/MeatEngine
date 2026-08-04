#include "engine/render/Cull.h"

#include <glm/geometric.hpp> // dot, length

#include <algorithm>
#include <cmath>
#include <numeric>

namespace meat::cull {

Frustum extractPlanes(const glm::mat4& m) {
    // glm is column-major, so row i is (m[0][i], m[1][i], m[2][i], m[3][i]).
    const auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    Frustum f = {
        r3 + r0, // left
        r3 - r0, // right
        r3 + r1, // bottom
        r3 - r1, // top
        r3 + r2, // near (GL clip: z in [-1, 1])
        r3 - r2, // far
    };
    for (glm::vec4& p : f) {
        const float len = glm::length(glm::vec3(p));
        if (len > 1e-6f) p /= len; // normalize so plane.w is a true signed distance
    }
    return f;
}

bool sphereInFrustum(const Frustum& f, glm::vec3 center, float radius) {
    for (const glm::vec4& p : f) {
        const float dist = glm::dot(glm::vec3(p), center) + p.w;
        if (dist < -radius) return false; // fully outside this plane → outside frustum
    }
    return true;
}

bool beyondDistance(glm::vec3 camPos, glm::vec3 center, float radius, float maxDist) {
    if (maxDist <= 0.0f) return false; // disabled
    return glm::length(center - camPos) - radius > maxDist;
}

std::vector<std::uint32_t> selectWithinBudget(std::span<const BudgetItem> items,
                                              glm::vec3 camPos, std::uint32_t maxDraws,
                                              std::uint32_t maxTriangles) {
    // Sort indices nearest-first so the closest (most visually important) draws win
    // the budget. Stable order keeps the result deterministic for equal distances.
    std::vector<std::uint32_t> order(items.size());
    std::iota(order.begin(), order.end(), 0u);
    const auto d2 = [&](std::uint32_t i) {
        const glm::vec3 v = items[i].center - camPos;
        return glm::dot(v, v);
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](std::uint32_t a, std::uint32_t b) { return d2(a) < d2(b); });

    std::vector<std::uint32_t> kept;
    std::uint32_t draws = 0, tris = 0;
    for (const std::uint32_t i : order) {
        if (maxDraws != 0 && draws + 1 > maxDraws) break;
        const std::uint32_t t = items[i].triangles;
        if (maxTriangles != 0 && tris + t > maxTriangles) break;
        kept.push_back(i);
        ++draws;
        tris += t;
    }
    return kept;
}

} // namespace meat::cull
