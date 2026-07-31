#pragma once

#include <glm/vec3.hpp>

#include <vector>

namespace meat {

// AABB zone with a constant gravity acceleration (m/s²). Higher priority wins
// when volumes overlap — ship decks (priority 10) beat ambient void (base).
struct GravityBoxVolume {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    glm::vec3 gravity{0.0f, -18.0f, 0.0f};
    int priority = 0;
};

// Radial gravity source (planetoid / station). Pull is toward `center`.
// Magnitude at distance d (outside the surface) ≈ surfaceGravity * (R/d)²,
// clamped near the singularity; soft zero beyond influenceRadius (SOI).
struct GravityBody {
    glm::vec3 center{0.0f};
    float surfaceRadius = 20.0f;   // metres — body "skin"
    float surfaceGravity = 12.0f;  // m/s² magnitude at the surface
    float influenceRadius = 120.0f; // metres — beyond this, body contributes nothing
};

// B3b: gravity as a field, not one global scalar. Pure / deterministic so the
// server and every predicting client sample the same vector at a world point.
// CharacterController and (later) ships/EVA all call sample(feetPos).
class GravityField {
public:
    void clear();
    void setBase(glm::vec3 gravity);
    glm::vec3 base() const { return m_base; }

    void addBox(GravityBoxVolume box);
    void addBody(GravityBody body);
    void clearBoxes() { m_boxes.clear(); }
    void clearBodies() { m_bodies.clear(); }

    const std::vector<GravityBoxVolume>& boxes() const { return m_boxes; }
    const std::vector<GravityBody>& bodies() const { return m_bodies; }

    // Acceleration vector (m/s²) at world position. Order: base → strongest body
    // in SOI (replaces base) → highest-priority containing box (replaces all).
    glm::vec3 sample(glm::vec3 worldPos) const;

private:
    glm::vec3 m_base{0.0f, -18.0f, 0.0f};
    std::vector<GravityBoxVolume> m_boxes;
    std::vector<GravityBody> m_bodies;
};

// Build a default field for an environment preset. Space gets near-zero base + a
// habitat box around the spawn pad + an optional orbital body for fall tests.
// Surface/Underwater: base only (matches EnvSettings::gravity).
void configureDefaultGravityField(GravityField& field, float envGravityY, bool spaceTheme);

} // namespace meat
