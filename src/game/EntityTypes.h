#pragma once
#include "game/Items.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace meat {

// Wire archetype ids (EntityState::archetype). Append-only: values are protocol.
enum class EntityArchetype : std::uint8_t {
    None = 0,
    ItemPickup = 1,
    Projectile = 2,
    Deployable = 3,
    // Turret, NpcChaser, ... land with their phases.
};

// Server-side world entity. Deliberately a plain struct while the roster is
// small (pickups); migrates to EntityRegistry components when AI/projectiles
// need per-kind systems.
struct WorldEntity {
    std::uint32_t id = 0;
    EntityArchetype type = EntityArchetype::None;
    glm::vec3 pos{0};
    float yaw = 0;
    ItemId item = 0;         // ItemPickup: what it grants
    std::uint16_t count = 0; // ItemPickup: how many
};

} // namespace meat
