#pragma once
#include <cstdint>

namespace meat {

// Engine users pick these; nothing downstream hardcodes a choice. Rules live on
// the server and travel to clients in Welcome.
struct GameRules {
    enum class InventoryModel : std::uint8_t {
        HotbarBackpack = 0, // 1-9 hotbar + Tab grid (default)
        GridOnly = 1,       // Tab grid, click to equip
        WeaponSlots = 2,    // guns on 1-4, blocks/consumables as counters
    };
    InventoryModel inventoryModel = InventoryModel::HotbarBackpack;
    bool finiteAmmo = true;      // guns consume ammo items
    bool minedBlockDrops = true; // broken blocks enter the breaker's inventory
    bool penetration = true;     // bullets spend budget passing through materials
    bool blockDamage = true;     // blocks chip (hp) instead of breaking on first hit
    // Metres per voxel — world-defining, applied to meat::kVoxelSize at startup. Devs pick
    // anything from fine (< 0.5) to chunkier-than-Minecraft (> 1). Must match across a
    // session (server + all clients see the same world), so it is set before world gen.
    float voxelSize = 0.5f; // == meat::kDefaultVoxelSize (avoid the heavy Chunk.h include here)

    std::uint8_t flagsByte() const {
        return static_cast<std::uint8_t>((finiteAmmo ? 1 : 0) | (minedBlockDrops ? 2 : 0) |
                                         (penetration ? 4 : 0) | (blockDamage ? 8 : 0));
    }
    void setFlagsByte(std::uint8_t f) {
        finiteAmmo = (f & 1) != 0;
        minedBlockDrops = (f & 2) != 0;
        penetration = (f & 4) != 0;
        blockDamage = (f & 8) != 0;
    }
};

} // namespace meat
