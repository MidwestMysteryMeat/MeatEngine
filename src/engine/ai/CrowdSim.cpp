#include "engine/ai/CrowdSim.h"

#include <glm/geometric.hpp> // length, normalize, dot

#include <algorithm> // sort
#include <cmath>
#include <cstdint>
#include <unordered_map>

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

glm::vec3 CrowdSim::steer(std::size_t i, const std::vector<std::size_t>& candidates) const {
    const float sepR2 = m_cfg.separationRadius * m_cfg.separationRadius;
    const float nbrR2 = m_cfg.neighborRadius * m_cfg.neighborRadius;
    const Agent& self = m_agents[i];
    glm::vec3 sep{0.0f}, aliSum{0.0f}, cohSum{0.0f};
    int aliN = 0, cohN = 0;
    for (const std::size_t j : candidates) { // candidates are ascending → reproducible sum
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
    glm::vec3 s{0.0f};
    s += sep * m_cfg.separationWeight;
    if (aliN > 0) s += (aliSum / static_cast<float>(aliN) - self.vel) * m_cfg.alignmentWeight;
    if (cohN > 0) s += (cohSum / static_cast<float>(cohN) - self.pos) * m_cfg.cohesionWeight;
    if (m_hasGoal) {
        const glm::vec3 toGoal = m_goal - self.pos;
        const float len2 = glm::dot(toGoal, toGoal);
        if (len2 > 1e-8f)
            s += (toGoal / std::sqrt(len2)) * (m_cfg.maxSpeed * m_cfg.goalWeight);
    }
    return clampLen(s, m_cfg.maxForce); // steering force; caller integrates onto velocity
}

void CrowdSim::stepBrute(float dt) {
    const std::size_t n = m_agents.size();
    std::vector<std::size_t> all(n);
    for (std::size_t j = 0; j < n; ++j) all[j] = j; // ascending, so steer() sums in index order
    std::vector<glm::vec3> newVel(n);
    for (std::size_t i = 0; i < n; ++i)
        newVel[i] = clampLen(m_agents[i].vel + steer(i, all) * dt, m_cfg.maxSpeed);
    for (std::size_t i = 0; i < n; ++i) {
        m_agents[i].vel = newVel[i];
        m_agents[i].pos += m_agents[i].vel * dt;
    }
}

void CrowdSim::stepGrid(float dt) {
    const std::size_t n = m_agents.size();
    const float cell = m_cfg.neighborRadius > 1e-4f ? m_cfg.neighborRadius : 1.0f;
    const float inv = 1.0f / cell;
    // Pack a cell coord into a collision-free key (coords biased into [0, 2^21)).
    constexpr std::int64_t kBias = 1 << 20, kMask = (1 << 21) - 1;
    auto cellOf = [&](glm::vec3 p) {
        return glm::ivec3(static_cast<int>(std::floor(p.x * inv)),
                          static_cast<int>(std::floor(p.y * inv)),
                          static_cast<int>(std::floor(p.z * inv)));
    };
    auto key = [&](int x, int y, int z) -> std::int64_t {
        return ((static_cast<std::int64_t>(x) + kBias) & kMask) << 42 |
               ((static_cast<std::int64_t>(y) + kBias) & kMask) << 21 |
               ((static_cast<std::int64_t>(z) + kBias) & kMask);
    };
    // Build the grid by iterating agents in order, so each cell's list is ascending.
    std::unordered_map<std::int64_t, std::vector<std::size_t>> grid;
    grid.reserve(n);
    std::vector<glm::ivec3> cells(n);
    for (std::size_t j = 0; j < n; ++j) {
        cells[j] = cellOf(m_agents[j].pos);
        grid[key(cells[j].x, cells[j].y, cells[j].z)].push_back(j);
    }
    std::vector<glm::vec3> newVel(n);
    std::vector<std::size_t> cand;
    for (std::size_t i = 0; i < n; ++i) {
        cand.clear();
        const glm::ivec3 c = cells[i];
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto it = grid.find(key(c.x + dx, c.y + dy, c.z + dz));
                    if (it != grid.end()) cand.insert(cand.end(), it->second.begin(), it->second.end());
                }
        std::sort(cand.begin(), cand.end()); // ascending → identical sum order to brute force
        newVel[i] = clampLen(m_agents[i].vel + steer(i, cand) * dt, m_cfg.maxSpeed);
    }
    for (std::size_t i = 0; i < n; ++i) {
        m_agents[i].vel = newVel[i];
        m_agents[i].pos += m_agents[i].vel * dt;
    }
}

void CrowdSim::step(float dt) {
    if (m_agents.empty()) return;
    if (m_cfg.spatialGrid) stepGrid(dt);
    else stepBrute(dt);
}

glm::vec3 CrowdSim::centroid() const {
    if (m_agents.empty()) return glm::vec3(0.0f);
    glm::vec3 sum{0.0f};
    for (const Agent& a : m_agents) sum += a.pos;
    return sum / static_cast<float>(m_agents.size());
}

} // namespace meat
