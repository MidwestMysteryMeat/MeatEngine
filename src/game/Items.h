#pragma once
#include "engine/voxel/Block.h"
#include "game/Effects.h"

#include <cstdint>
#include <string>
#include <vector>

namespace meat {

using ItemId = std::uint16_t; // 0 = empty
enum class ItemType : std::uint8_t { Weapon, Ammo, Consumable, KeyItem, Block };

// How the trigger behaves; the client uses this to decide auto-repeat feel.
enum class FireMode : std::uint8_t { SemiAuto, Auto, Burst };

// What a shot delivers. Hitscan covers pistol/AR/SMG/shotgun/sniper (pellets +
// spread + budget parameterize them); Projectile covers RPG/grenade as server
// entities; Deployable places a trap/turret entity on the aimed surface.
enum class DeliveryKind : std::uint8_t { Hitscan, Projectile, Deployable };

struct ItemDef {
    std::string name;
    ItemType type = ItemType::KeyItem;
    std::uint16_t maxStack = 1;
    // Weapon fields
    float damage = 0.0f;
    float fireInterval = 0.0f;
    float penBudget = 0.0f; // penetration budget spent on materials crossed
    ItemId ammoItem = 0;    // reserve ammo: consumed per shot (magless) or per reload (magazine)
    // AP/HP ammo tuning, applied per shot in the hitscan march (fireHitscan/
    // marchBullet): damageMult scales the damage delivered to flesh/blocks, and
    // penetrationMult scales the penetration budget. AP drills more but hits
    // softer (damage < 1, pen > 1); HP hits hard but stops in the first material
    // (pen 0). 1.0/1.0 = a plain full-metal-jacket round (every existing weapon).
    float damageMult = 1.0f;
    float penetrationMult = 1.0f;
    FireMode fireMode = FireMode::SemiAuto;
    DeliveryKind delivery = DeliveryKind::Hitscan;
    std::uint8_t pellets = 1;    // hitscan: rays per shot (shotgun > 1)
    float spreadDeg = 0.0f;      // hitscan cone half-angle
    std::uint16_t magSize = 0;   // rounds per magazine; 0 = no magazine (melee/thrown/infinite)
    int burstCount = 3;          // burst mode: rounds per pull
    // Projectile fields
    float projectileSpeed = 0.0f;
    float projectileGravity = 0.0f;
    float blastRadius = 0.0f;    // projectile/deployable AoE radius (m)
    float blastDamage = 0.0f;    // center damage, falls off to the edge
    bool deploysTurret = false;  // Deployable delivery: spawn an auto-turret, not a mine
    bool deploysCompanion = false; // Deployable delivery: spawn a mobile ally, not a mine
    // Block field
    BlockId blockId = 0; // what placing this item builds
    // Effect-composition (GAS-lite). What this item *does* when it lands, run
    // through ServerSim::runEffects. A Consumable runs `effects` on the user
    // (medkit = [Heal], stim = [Heal, ApplyModifier]); a Projectile carries them
    // as its on-impact list (rpg/grenade = [AreaDamage]). Empty = the item still
    // uses its legacy bespoke path (or, for blasts, a derived AreaDamage).
    EffectList effects;
};

class ItemRegistry {
public:
    ItemId add(ItemDef def) {
        m_defs.push_back(std::move(def));
        return static_cast<ItemId>(m_defs.size()); // ids start at 1
    }
    const ItemDef& get(ItemId id) const {
        static const ItemDef kEmpty{"empty"};
        return id == 0 || id > m_defs.size() ? kEmpty : m_defs[id - 1];
    }

private:
    std::vector<ItemDef> m_defs;
};

// The slice's default item set; games redefine their own (Lua later). Must be
// registered identically on server and client — ids are wire data.
struct DefaultItems {
    ItemId pistol = 0, ammo9mm = 0, medkit = 0, stoneBlock = 0;
    ItemId apPistol = 0, hpPistol = 0; // AP/HP ammo-loadout variants of the pistol
    ItemId smg = 0, shotgun = 0, sniper = 0, rpg = 0, grenade = 0, claymore = 0, turret = 0;
    ItemId companionBeacon = 0;
    ItemId shells = 0, rockets = 0, rifleAmmo = 0;
    ItemId stim = 0; // composed consumable: Heal + a timed damage/speed buff
};
DefaultItems registerDefaultItems(ItemRegistry& items, BlockId stone);

} // namespace meat
