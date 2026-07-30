#pragma once
#include "engine/voxel/Block.h"

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
    ItemId ammoItem = 0;    // consumed per shot when GameRules::finiteAmmo
    FireMode fireMode = FireMode::SemiAuto;
    DeliveryKind delivery = DeliveryKind::Hitscan;
    std::uint8_t pellets = 1;    // hitscan: rays per shot (shotgun > 1)
    float spreadDeg = 0.0f;      // hitscan cone half-angle
    int burstCount = 3;          // burst mode: rounds per pull
    // Projectile fields
    float projectileSpeed = 0.0f;
    float projectileGravity = 0.0f;
    float blastRadius = 0.0f;    // projectile/deployable AoE radius (m)
    float blastDamage = 0.0f;    // center damage, falls off to the edge
    bool deploysTurret = false;  // Deployable delivery: spawn an auto-turret, not a mine
    // Block field
    BlockId blockId = 0; // what placing this item builds
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
    ItemId smg = 0, shotgun = 0, sniper = 0, rpg = 0, grenade = 0, claymore = 0, turret = 0;
    ItemId shells = 0, rockets = 0, rifleAmmo = 0;
};
DefaultItems registerDefaultItems(ItemRegistry& items, BlockId stone);

} // namespace meat
