#pragma once

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace meat {

// Steering weights + limits for the boids crowd. Defaults give a cohesive but
// non-overlapping flock that drifts toward its goal.
struct CrowdConfig {
    float separationRadius = 1.5f; // push apart closer than this (m)
    float neighborRadius = 4.0f;   // align/cohere with neighbours within this (m)
    float separationWeight = 1.6f;
    float alignmentWeight = 1.0f;
    float cohesionWeight = 0.8f;
    float goalWeight = 0.6f;
    float maxSpeed = 3.5f; // m/s
    float maxForce = 8.0f; // m/s^2 steering clamp (keeps turns smooth + stable)
    // Neighbour query: a uniform spatial hash grid (O(n)) vs brute force (O(n^2)).
    // The grid gathers candidates from the 3x3x3 cell block (cell = neighborRadius)
    // and sorts them by index, so it is *bit-identical* to brute force — just the
    // scalable path for large crowds. Off falls back to brute force (fine for tiny
    // crowds / as a reference).
    bool spatialGrid = true;
};

// A deterministic boids crowd: a flat array of agents steered by the classic
// separation / alignment / cohesion rules plus an optional shared goal. Built to
// the Phase-7 authoritative-tick contract — fixed-step, seeded, order-stable —
// so the same seed and step count reproduce the same crowd. (Float math is
// reproducible within a platform; the cross-platform hardening to fixed-point is
// the tracked follow-up.) Independent of the renderer and the netcode: the server
// owns one and replicates the agents; tools can run one headless.
class CrowdSim {
public:
    struct Agent {
        glm::vec3 pos{0.0f};
        glm::vec3 vel{0.0f};
    };

    // Seeded spawn of `count` agents scattered in a disc of `radius` around
    // `center` (on the center.y plane) with small seeded starting velocities.
    // Replaces any existing agents. Deterministic in seed + count.
    void spawn(std::uint32_t seed, int count, glm::vec3 center, float radius);

    void setGoal(glm::vec3 goal);
    void clearGoal();
    bool hasGoal() const { return m_hasGoal; }

    // Advance one fixed step. Steering reads the pre-step state of every agent, so
    // agent order never changes the result.
    void step(float dt);

    const std::vector<Agent>& agents() const { return m_agents; }
    std::size_t size() const { return m_agents.size(); }

    CrowdConfig& config() { return m_cfg; }
    const CrowdConfig& config() const { return m_cfg; }

    // The average agent position (crowd centre); {0,0,0} when empty.
    glm::vec3 centroid() const;

private:
    // New velocity for agent i, given the indices of candidate neighbours (which it
    // filters by radius). Candidates must be in a fixed order (ascending) so the
    // float accumulation is reproducible across the brute and grid paths.
    glm::vec3 steer(std::size_t i, const std::vector<std::size_t>& candidates) const;
    void stepBrute(float dt);
    void stepGrid(float dt);

    std::vector<Agent> m_agents;
    CrowdConfig m_cfg;
    glm::vec3 m_goal{0.0f};
    bool m_hasGoal = false;
};

} // namespace meat
