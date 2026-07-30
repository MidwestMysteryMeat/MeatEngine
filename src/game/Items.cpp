#include "game/Items.h"

namespace meat {

DefaultItems registerDefaultItems(ItemRegistry& items, BlockId stone) {
    DefaultItems d;
    d.ammo9mm = items.add({.name = "9mm", .type = ItemType::Ammo, .maxStack = 120});
    d.pistol = items.add({.name = "pistol",
                          .type = ItemType::Weapon,
                          .maxStack = 1,
                          .damage = 25.0f,
                          .fireInterval = 0.15f,
                          .ammoItem = d.ammo9mm});
    d.medkit = items.add({.name = "medkit", .type = ItemType::Consumable, .maxStack = 4});
    d.stoneBlock = items.add(
        {.name = "stone", .type = ItemType::Block, .maxStack = 250, .blockId = stone});
    return d;
}

} // namespace meat
