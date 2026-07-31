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
    NpcChaser = 4,  // melee rusher
    NpcShooter = 5, // ranged, holds distance
    Turret = 6,     // placed auto-defense
    Companion = 7,  // mobile ally: follows its owner, engages hostile NPCs
    NpcZombie = 8,  // slow melee shambler; tanky, hits hard
    // Scheduled NPCs land with their phases.
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
