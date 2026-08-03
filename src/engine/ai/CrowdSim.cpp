#include "engine/ai/CrowdSim.h"

#include <glm/geometric.hpp> // length, normalize, dot

#include <cmath>

namespace meat {

namespace {

// A small deterministic RNG (SplitMix64) so seeded spawns reproduce identically
// on every platform — std::uniform_*_distribution does not guarantee that.
struct SplitMix64 {
    std::uint64_t s;
    explicit SplitMix64(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {
        s += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // Uniform float in [0,1).
    float unit() { return static_cast<float>(next() >> 40) / static_cast<float>(1u << 24); }
    // Uniform float in [-1,1).
    float signedUnit() { return unit() * 2.0f - 1.0f; }
};

// Clamp a vector's magnitude to `maxLen` (no-op below it). Zero stays zero.
glm::vec3 clampLen(glm::vec3 v, float maxLen) {
    const float len2 = glm::dot(v, v);
    if (len2 <= maxLen * maxLen || len2 < 1e-12f) return v;
    return v * (maxLen / std::sqrt(len2));
}

} // namespace

void CrowdSim::spawn(std::uint32_t seed, int count, glm::vec3 center, float radius) {
    m_agents.clear();
    if (count <= 0) return;
    m_agents.reserve(static_cast<std::size_t>(count));
    SplitMix64 rng(0x1234567800000000ull ^ seed);
    for (int i = 0; i < count; ++i) {
        // Rejection-free disc sample: sqrt(u) radius keeps it uniform, not centre-heavy.
        const float ang = rng.unit() * 6.28318530718f;
        const float r = std::sqrt(rng.unit()) * radius;
        Agent a;
        a.pos = center + glm::vec3(std::cos(ang) * r, 0.0f, std::sin(ang) * r);
        a.vel = glm::vec3(rng.signedUnit(), 0.0f, rng.signedUnit()) * (m_cfg.maxSpeed * 0.25f);
        m_agents.push_back(a);
    }
}

void CrowdSim::setGoal(glm::vec3 goal) {
    m_goal = goal;
    m_hasGoal = true;
}

void CrowdSim::clearGoal() { m_hasGoal = false; }

void CrowdSim::step(float dt) {
    const std::size_t n = m_agents.size();
    if (n == 0) return;
    const float sepR2 = m_cfg.separationRadius * m_cfg.separationRadius;
    const float nbrR2 = m_cfg.neighborRadius * m_cfg.neighborRadius;

    // Compute every agent's new velocity from the PRE-step state, then apply — so
    // iteration order cannot change the result (determinism).
    std::vector<glm::vec3> newVel(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Agent& self = m_agents[i];
        glm::vec3 sep{0.0f}, aliSum{0.0f}, cohSum{0.0f};
        int aliN = 0, cohN = 0;
        for (std::size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            const glm::vec3 d = self.pos - m_agents[j].pos;
            const float d2 = glm::dot(d, d);
            if (d2 < sepR2 && d2 > 1e-8f) sep += d / d2; // stronger the closer they are
            if (d2 < nbrR2) {
                aliSum += m_agents[j].vel;
                cohSum += m_agents[j].pos;
                ++aliN;
                ++cohN;
            }
        }
        glm::vec3 steer{0.0f};
        steer += sep * m_cfg.separationWeight;
        if (aliN > 0) {
            const glm::vec3 avgVel = aliSum / static_cast<float>(aliN);
            steer += (avgVel - self.vel) * m_cfg.alignmentWeight;
        }
        if (cohN > 0) {
            const glm::vec3 center = cohSum / static_cast<float>(cohN);
            steer += (center - self.pos) * m_cfg.cohesionWeight;
        }
        if (m_hasGoal) {
            const glm::vec3 toGoal = m_goal - self.pos;
            const float len2 = glm::dot(toGoal, toGoal);
            if (len2 > 1e-8f)
                steer += (toGoal / std::sqrt(len2)) * (m_cfg.maxSpeed * m_cfg.goalWeight);
        }
        steer = clampLen(steer, m_cfg.maxForce);
        newVel[i] = clampLen(self.vel + steer * dt, m_cfg.maxSpeed);
    }

    for (std::size_t i = 0; i < n; ++i) {
        m_agents[i].vel = newVel[i];
        m_agents[i].pos += m_agents[i].vel * dt;
    }
}

glm::vec3 CrowdSim::centroid() const {
    if (m_agents.empty()) return glm::vec3(0.0f);
    glm::vec3 sum{0.0f};
    for (const Agent& a : m_agents) sum += a.pos;
    return sum / static_cast<float>(m_agents.size());
}

} // namespace meat
