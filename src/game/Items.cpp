#include "game/Items.h"

namespace meat {

DefaultItems registerDefaultItems(ItemRegistry& items, BlockId stone) {
    DefaultItems d;
    // Ammo
    d.ammo9mm = items.add({.name = "9mm", .type = ItemType::Ammo, .maxStack = 120});
    d.shells = items.add({.name = "shells", .type = ItemType::Ammo, .maxStack = 60});
    d.rifleAmmo = items.add({.name = "rifle rounds", .type = ItemType::Ammo, .maxStack = 90});
    d.rockets = items.add({.name = "rockets", .type = ItemType::Ammo, .maxStack = 8});

    // Reference arsenal — one weapon per archetype, so every code path has a
    // proving item and devs have templates to copy.
    d.pistol = items.add({.name = "pistol",
                          .type = ItemType::Weapon,
                          .damage = 25.0f,
                          .fireInterval = 0.15f,
                          .penBudget = 20.0f, // punches dirt/grass, stopped by stone
                          .ammoItem = d.ammo9mm});
    d.smg = items.add({.name = "smg",
                       .type = ItemType::Weapon,
                       .damage = 14.0f,
                       .fireInterval = 0.075f,
                       .penBudget = 15.0f,
                       .ammoItem = d.ammo9mm,
                       .fireMode = FireMode::Auto,
                       .spreadDeg = 2.2f});
    d.shotgun = items.add({.name = "shotgun",
                           .type = ItemType::Weapon,
                           .damage = 11.0f, // per pellet; 8 = brutal up close, fades far
                           .fireInterval = 0.8f,
                           .penBudget = 8.0f,
                           .ammoItem = d.shells,
                           .pellets = 8,
                           .spreadDeg = 6.0f});
    d.sniper = items.add({.name = "sniper",
                          .type = ItemType::Weapon,
                          .damage = 95.0f,
                          .fireInterval = 1.1f,
                          .penBudget = 90.0f, // drills through several stone blocks
                          .ammoItem = d.rifleAmmo});
    d.rpg = items.add({.name = "rpg",
                       .type = ItemType::Weapon,
                       .fireInterval = 1.4f,
                       .ammoItem = d.rockets,
                       .delivery = DeliveryKind::Projectile,
                       .projectileSpeed = 34.0f,
                       .blastRadius = 4.5f,
                       .blastDamage = 120.0f});
    d.grenade = items.add({.name = "grenade",
                           .type = ItemType::Weapon,
                           .maxStack = 6, // thrown consumable-weapon
                           .fireInterval = 0.9f,
                           .delivery = DeliveryKind::Projectile,
                           .projectileSpeed = 16.0f,
                           .projectileGravity = 14.0f,
                           .blastRadius = 3.5f,
                           .blastDamage = 90.0f});
    d.claymore = items.add({.name = "claymore",
                            .type = ItemType::Weapon,
                            .maxStack = 4,
                            .fireInterval = 0.6f,
                            .delivery = DeliveryKind::Deployable,
                            .blastRadius = 3.0f,
                            .blastDamage = 110.0f});

    d.medkit = items.add({.name = "medkit", .type = ItemType::Consumable, .maxStack = 4});
    d.stoneBlock = items.add(
        {.name = "stone", .type = ItemType::Block, .maxStack = 250, .blockId = stone});
    return d;
}

} // namespace meat
