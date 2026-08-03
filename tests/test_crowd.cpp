// Deterministic boids crowd (Phase 7 first slice). The crowd runs on the
// authoritative-tick contract, so the tests assert what that promises: the same
// seed reproduces the same crowd bit-for-bit, and the steering rules produce the
// expected emergent shape — the flock advances toward a goal, spreads apart when
// overlapping, and stays cohesive instead of flying off to infinity.

#include "Harness.h"

#include "engine/ai/CrowdSim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

using meattest::check;

constexpr float kDt = 1.0f / 60.0f;

float dist(glm::vec3 a, glm::vec3 b) {
    const glm::vec3 d = a - b;
    return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

float minPairwise(const meat::CrowdSim& c) {
    const auto& a = c.agents();
    float m = 1e9f;
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = i + 1; j < a.size(); ++j)
            m = std::min(m, dist(a[i].pos, a[j].pos));
    return m;
}

float spread(const meat::CrowdSim& c) { // max distance of any agent from the centre
    const glm::vec3 ctr = c.centroid();
    float m = 0.0f;
    for (const auto& ag : c.agents()) m = std::max(m, dist(ag.pos, ctr));
    return m;
}

void testCrowdIsDeterministic() {
    std::printf("a seeded crowd reproduces identically\n");
    meat::CrowdSim a, b;
    a.spawn(42, 60, glm::vec3(0.0f), 8.0f);
    b.spawn(42, 60, glm::vec3(0.0f), 8.0f);
    a.setGoal(glm::vec3(40.0f, 0.0f, 0.0f));
    b.setGoal(glm::vec3(40.0f, 0.0f, 0.0f));
    for (int i = 0; i < 150; ++i) { a.step(kDt); b.step(kDt); }

    bool same = a.size() == b.size() && a.size() == 60;
    for (std::size_t i = 0; i < a.size() && same; ++i)
        if (dist(a.agents()[i].pos, b.agents()[i].pos) != 0.0f) same = false;
    check(same, "same seed + step count reproduce identical agent positions");
}

void testCrowdSeeksGoal() {
    std::printf("a crowd with a goal advances toward it\n");
    meat::CrowdSim c;
    c.spawn(7, 50, glm::vec3(0.0f), 6.0f);
    const glm::vec3 goal(60.0f, 0.0f, 0.0f);
    c.setGoal(goal);
    const float before = dist(c.centroid(), goal);
    for (int i = 0; i < 400; ++i) c.step(kDt);
    const float after = dist(c.centroid(), goal);
    check(after < before - 10.0f, "the crowd's centre moves measurably toward the goal");
}

void testCrowdSeparatesButStaysCohesive() {
    std::printf("a crowd spreads out yet holds together\n");
    meat::CrowdSim c;
    c.spawn(3, 40, glm::vec3(0.0f), 1.0f); // tightly overlapping → separation must act
    const float startSpread = spread(c);
    for (int i = 0; i < 400; ++i) c.step(kDt); // pure flocking, no goal
    const float endSpread = spread(c);
    check(endSpread > startSpread + 0.5f, "separation pushes the packed crowd apart");
    check(minPairwise(c) > 0.1f, "no two agents collapse onto the same point");
    check(endSpread < 25.0f, "cohesion keeps the crowd from flying apart");
}

} // namespace

namespace meattest {

void runCrowd() {
    testCrowdIsDeterministic();
    testCrowdSeeksGoal();
    testCrowdSeparatesButStaysCohesive();
}

} // namespace meattest
