#pragma once
#include "engine/voxel/Block.h"

#include <cstdint>
#include <string>
#include <vector>

namespace meat {

using ItemId = std::uint16_t; // 0 = empty
enum class ItemType : std::uint8_t { Weapon, Ammo, Consumable, KeyItem, Block };

struct ItemDef {
    std::string name;
    ItemType type = ItemType::KeyItem;
    std::uint16_t maxStack = 1;
    // Weapon fields
    float damage = 0.0f;
    float fireInterval = 0.0f;
    float penBudget = 0.0f; // penetration budget spent on materials crossed
    ItemId ammoItem = 0;    // consumed per shot when GameRules::finiteAmmo
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
};
DefaultItems registerDefaultItems(ItemRegistry& items, BlockId stone);

} // namespace meat
