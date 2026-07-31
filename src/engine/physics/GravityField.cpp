#include "engine/physics/GravityField.h"
#include "engine/voxel/Chunk.h" // kVoxelSize — habitat scales with world metres

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace meat {

void GravityField::clear() {
    m_base = glm::vec3(0.0f, -18.0f, 0.0f);
    m_boxes.clear();
    m_bodies.clear();
}

void GravityField::setBase(glm::vec3 gravity) { m_base = gravity; }

void GravityField::addBox(GravityBoxVolume box) {
    // Normalize bounds so min <= max on every axis (tolerant of inverted editor input).
    for (int i = 0; i < 3; ++i) {
        if (box.min[i] > box.max[i]) std::swap(box.min[i], box.max[i]);
    }
    m_boxes.push_back(box);
}

void GravityField::addBody(GravityBody body) {
    body.surfaceRadius = std::max(body.surfaceRadius, 0.5f);
    body.influenceRadius = std::max(body.influenceRadius, body.surfaceRadius);
    body.surfaceGravity = std::max(body.surfaceGravity, 0.0f);
    m_bodies.push_back(body);
}

glm::vec3 GravityField::sample(glm::vec3 worldPos) const {
    glm::vec3 g = m_base;

    // Strongest orbital body in SOI replaces ambient base (not summed — keeps
    // "which planet am I falling toward" unambiguous for the FPS controller).
    float bestMag = 0.0f;
    glm::vec3 bodyG{0.0f};
    for (const GravityBody& body : m_bodies) {
        const glm::vec3 delta = body.center - worldPos; // toward center
        const float dist = glm::length(delta);
        if (dist < 1e-3f || dist >= body.influenceRadius) continue;
        const float r = body.surfaceRadius;
        // Outside: inverse-square. Inside the "surface": linear falloff toward 0 at center
        // so we never infinite-spike and EVA through a hollow station is gentle.
        float mag = 0.0f;
        if (dist >= r) {
            const float ratio = r / dist;
            mag = body.surfaceGravity * ratio * ratio;
        } else {
            mag = body.surfaceGravity * (dist / r);
        }
        // Soft edge near SOI so you don't hard-cut at the boundary.
        const float edge = 1.0f - glm::clamp((dist - body.influenceRadius * 0.85f) /
                                                 (body.influenceRadius * 0.15f + 1e-3f),
                                             0.0f, 1.0f);
        mag *= edge;
        if (mag > bestMag) {
            bestMag = mag;
            bodyG = (delta / dist) * mag;
        }
    }
    if (bestMag > 0.0f) g = bodyG;

    // Local volumes (ship decks, habitats) override everything when containing the point.
    int bestPri = -1;
    for (const GravityBoxVolume& box : m_boxes) {
        if (worldPos.x < box.min.x || worldPos.x > box.max.x || worldPos.y < box.min.y ||
            worldPos.y > box.max.y || worldPos.z < box.min.z || worldPos.z > box.max.z)
            continue;
        if (box.priority >= bestPri) {
            bestPri = box.priority;
            g = box.gravity;
        }
    }
    return g;
}

void configureDefaultGravityField(GravityField& field, float envGravityY, bool spaceTheme) {
    field.clear();
    field.setBase(glm::vec3(0.0f, envGravityY, 0.0f));
    if (!spaceTheme) return;

    // Space: ambient is the env near-zero pull. A habitat box around the spawn
    // pad (voxel cell 16,16,16 × kVoxelSize) restores walkable "down" so the
    // player isn't immediately in freefall. Priority high so it wins over any
    // nearby orbital body. Bounds scale with kVoxelSize so large blocks still fit.
    const float s = kVoxelSize;
    const glm::vec3 pad(16.0f * s, 16.0f * s, 16.0f * s);
    GravityBoxVolume habitat;
    habitat.min = pad + glm::vec3(-16.0f * s, -8.0f * s, -16.0f * s);
    habitat.max = pad + glm::vec3(16.0f * s, 12.0f * s, 16.0f * s);
    habitat.gravity = glm::vec3(0.0f, -12.0f, 0.0f);
    habitat.priority = 10;
    field.addBox(habitat);

    // A planetoid below the pad for fall-into-SOI tests and H4 orbital prep.
    // Standing on the habitat box still wins (priority); leave the box and you
    // feel the radial pull toward the body center.
    GravityBody planet;
    planet.center = pad + glm::vec3(0.0f, -50.0f * s, 0.0f);
    planet.surfaceRadius = 20.0f * s;
    planet.surfaceGravity = 14.0f;
    planet.influenceRadius = 100.0f * s;
    field.addBody(planet);
}

} // namespace meat
