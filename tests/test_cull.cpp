// Culling math (always-on frustum + distance culling, and the opt-in PSX budget
// cull). Pure and GPU-free, so the tests pin the geometry: a sphere in front is
// visible while ones behind / off to the side / past the far plane are culled;
// distance culling respects the sphere radius; and the budget selector keeps the
// nearest draws first, stopping at the draw-count or triangle cap.

#include "Harness.h"

#include "engine/render/Cull.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>
#include <vector>

namespace {

using meattest::check;

meat::cull::Frustum makeFrustum() {
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    const glm::mat4 view =
        glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    return meat::cull::extractPlanes(proj * view);
}

void testFrustumCulling() {
    std::printf("frustum culling keeps what's in view and drops what isn't\n");
    const meat::cull::Frustum f = makeFrustum();
    check(meat::cull::sphereInFrustum(f, {0.0f, 0.0f, -5.0f}, 0.5f),
          "a sphere in front of the camera is visible");
    check(!meat::cull::sphereInFrustum(f, {0.0f, 0.0f, 5.0f}, 0.5f),
          "a sphere behind the camera is culled");
    check(!meat::cull::sphereInFrustum(f, {0.0f, 0.0f, -200.0f}, 0.5f),
          "a sphere past the far plane is culled");
    check(!meat::cull::sphereInFrustum(f, {100.0f, 0.0f, -5.0f}, 0.5f),
          "a sphere far off to the side is culled");
}

void testDistanceCulling() {
    std::printf("distance culling respects the sphere radius and the disable case\n");
    const glm::vec3 cam{0.0f};
    check(meat::cull::beyondDistance(cam, {0.0f, 0.0f, -50.0f}, 1.0f, 40.0f),
          "a sphere well beyond the draw distance is culled");
    check(!meat::cull::beyondDistance(cam, {0.0f, 0.0f, -50.0f}, 1.0f, 60.0f),
          "a sphere within the draw distance is kept");
    check(!meat::cull::beyondDistance(cam, {0.0f, 0.0f, -41.0f}, 2.0f, 40.0f),
          "a sphere whose radius reaches inside the range is kept");
    check(!meat::cull::beyondDistance(cam, {0.0f, 0.0f, -9999.0f}, 1.0f, 0.0f),
          "a non-positive max distance disables distance culling");
}

void testBudgetSelection() {
    std::printf("budget culling keeps the nearest draws within both caps\n");
    // Array order is intentionally scrambled: nearest-first is A,B,C,D but they are
    // stored [D, B, A, C] (indices 0..3), so the result proves distance sorting.
    const std::vector<meat::cull::BudgetItem> items = {
        {{0.0f, 0.0f, -40.0f}, 30}, // 0 = D (farthest)
        {{0.0f, 0.0f, -20.0f}, 30}, // 1 = B
        {{0.0f, 0.0f, -10.0f}, 30}, // 2 = A (nearest)
        {{0.0f, 0.0f, -30.0f}, 30}, // 3 = C
    };
    const glm::vec3 cam{0.0f};

    const auto twoDraws = meat::cull::selectWithinBudget(items, cam, 2, 0);
    check(twoDraws.size() == 2 && twoDraws[0] == 2 && twoDraws[1] == 1,
          "a 2-draw cap keeps the two nearest (A then B), in order");

    const auto triCap = meat::cull::selectWithinBudget(items, cam, 0, 70);
    check(triCap.size() == 2 && triCap[0] == 2 && triCap[1] == 1,
          "a 70-triangle cap fits two 30-tri draws, not a third");

    const auto unbounded = meat::cull::selectWithinBudget(items, cam, 0, 0);
    check(unbounded.size() == 4, "zero caps mean unbounded — everything is kept");

    check(meat::cull::selectWithinBudget(items, cam, 1, 0).size() == 1,
          "a 1-draw cap keeps exactly one");
}

} // namespace

namespace meattest {

void runCull() {
    testFrustumCulling();
    testDistanceCulling();
    testBudgetSelection();
}

} // namespace meattest
