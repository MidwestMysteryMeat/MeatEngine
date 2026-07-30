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
    // AP/HP ammo-loadout variants of the pistol: same 9mm frame, different round,
    // so both ammo-multiplier paths are reachable straight from the loadout.
    // AP (armor-piercing): drills more materials (penBudget x1.6) for a softer hit
    // (damage x0.85). HP (hollow-point): hits hard (damage x1.4) but stops in the
    // first material it strikes (penetration x0 -> zero budget).
    d.apPistol = items.add({.name = "AP pistol",
                            .type = ItemType::Weapon,
                            .damage = 25.0f,
                            .fireInterval = 0.15f,
                            .penBudget = 20.0f,
                            .ammoItem = d.ammo9mm,
                            .damageMult = 0.85f,
                            .penetrationMult = 1.6f});
    d.hpPistol = items.add({.name = "HP pistol",
                            .type = ItemType::Weapon,
                            .damage = 25.0f,
                            .fireInterval = 0.15f,
                            .penBudget = 20.0f,
                            .ammoItem = d.ammo9mm,
                            .damageMult = 1.4f,
                            .penetrationMult = 0.0f});
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
    // Projectile blasts are now composed: the on-impact EffectList holds an
    // AreaDamage that runEffects routes back through applyBlast, so behaviour is
    // identical to the old inline blast (same radius/damage/falloff) but the path
    // is data — a projectile could carry [AreaDamage, Ignite, ApplyModifier].
    d.rpg = items.add({.name = "rpg",
                       .type = ItemType::Weapon,
                       .fireInterval = 1.4f,
                       .ammoItem = d.rockets,
                       .delivery = DeliveryKind::Projectile,
                       .projectileSpeed = 34.0f,
                       .blastRadius = 4.5f,
                       .blastDamage = 120.0f,
                       .effects = {areaDamageEffect(120.0f, 4.5f)}});
    d.grenade = items.add({.name = "grenade",
                           .type = ItemType::Weapon,
                           .maxStack = 6, // thrown consumable-weapon
                           .fireInterval = 0.9f,
                           .delivery = DeliveryKind::Projectile,
                           .projectileSpeed = 16.0f,
                           .projectileGravity = 14.0f,
                           .blastRadius = 3.5f,
                           .blastDamage = 90.0f,
                           .effects = {areaDamageEffect(90.0f, 3.5f)}});
    d.claymore = items.add({.name = "claymore",
                            .type = ItemType::Weapon,
                            .maxStack = 4,
                            .fireInterval = 0.6f,
                            .delivery = DeliveryKind::Deployable,
                            .blastRadius = 3.0f,
                            .blastDamage = 110.0f});

    d.turret = items.add({.name = "turret",
                          .type = ItemType::Weapon,
                          .maxStack = 3,
                          .fireInterval = 0.8f,
                          .delivery = DeliveryKind::Deployable,
                          .deploysTurret = true});
    d.companionBeacon = items.add({.name = "companion beacon",
                                   .type = ItemType::Weapon,
                                   .maxStack = 2,
                                   .fireInterval = 1.0f,
                                   .delivery = DeliveryKind::Deployable,
                                   .deploysCompanion = true});
    // Consumables carry their effect on use. Medkit = a plain Heal 50 (was an
    // inline health bump); the stim STACKS effects to prove composition — it
    // Heals 25 AND applies a timed buff (x1.5 outgoing damage, x1.3 speed for 8s).
    // The damage half is enforced server-side now; the speed half is stored and
    // is a follow-up (CharacterController tuning is engine-owned — see Effects.h).
    d.medkit = items.add({.name = "medkit",
                          .type = ItemType::Consumable,
                          .maxStack = 4,
                          .effects = {healEffect(50.0f)}});
    d.stim = items.add({.name = "stim",
                        .type = ItemType::Consumable,
                        .maxStack = 4,
                        .effects = {healEffect(25.0f), modifierEffect(1.5f, 1.3f, 8.0f)}});
    d.stoneBlock = items.add(
        {.name = "stone", .type = ItemType::Block, .maxStack = 250, .blockId = stone});
    return d;
}

} // namespace meat
