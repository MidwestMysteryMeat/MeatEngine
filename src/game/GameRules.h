#pragma once
#include <cstdint>

namespace meat {

// Engine users pick these; nothing downstream hardcodes a choice. Rules live on
// the server and travel to clients in Welcome (inventory model, flags/terrain,
// voxelSize, environment). Perspective is a local presentation choice and is
// NOT packed into Welcome — each client may pick first/third independently.
struct GameRules {
    enum class InventoryModel : std::uint8_t {
        HotbarBackpack = 0, // 1-9 hotbar + Tab grid (default)
        GridOnly = 1,       // Tab grid, click to equip
        WeaponSlots = 2,    // guns on 1-4, blocks/consumables as counters
    };
    InventoryModel inventoryModel = InventoryModel::HotbarBackpack;
    // World generation mode — a dev-facing option (the engine isn't voxel-only in spirit).
    // Normal = FastNoiseLite terrain + dungeon; Superflat = flat ground for building; Void = empty
    // canvas (place everything yourself). Travels to clients packed in the flags byte (bits 5-6).
    enum class Terrain : std::uint8_t { Normal = 0, Superflat = 1, Void = 2 };
    Terrain terrain = Terrain::Normal;
    // World ENVIRONMENT preset — composes with terrain to define the *feel* of a world by
    // driving gravity + fog + ambient light together (see game/Environment.h for the values).
    // Surface = the default earthy world; Underwater = buoyant + thick blue fog; Space = floaty +
    // black void, no fog. Packed into Welcome (not flagsByte — no free bits) so a networked
    // joiner gets the host's gravity/fog/ambient instead of defaulting to Surface.
    enum class Environment : std::uint8_t { Surface = 0, Underwater = 1, Space = 2 };
    Environment environment = Environment::Surface;
    // Camera presentation (H1 first slice: FPS↔TPS). First = eye-height FPS cam + crosshair;
    // Third = over-shoulder cam with collision pullback, no crosshair. Set via game.json
    // "perspective" or "template" (fps→First, tps→Third) / --perspective / --template.
    // While piloting a ship (H4), V toggles this at runtime without changing the project default.
    enum class Perspective : std::uint8_t { First = 0, Third = 1 };
    Perspective perspective = Perspective::First;
    // Project genre template (H1/H4). Space presets Void+Space env+ships; Racer spawns a car.
    enum class Template : std::uint8_t { Fps = 0, Tps = 1, Space = 2, Racer = 3 };
    Template gameTemplate = Template::Fps;
    // Game mode: the rules of victory layered over a template. Sandbox = no win
    // condition (the historical free-play). Deathmatch = first to fragLimit frags
    // wins. Selectable via --mode / game.json "mode". More modes (Horde/Breach/
    // teams) build on the same frag/score + match-over hooks.
    enum class GameMode : std::uint8_t { Sandbox = 0, Deathmatch = 1 };
    GameMode gameMode = GameMode::Sandbox;
    int fragLimit = 10; // Deathmatch target score
    // When true, the environment's hemiStrength is applied (A3). When false, ambient is
    // classic isotropic only — the dark PSX-night look. Toggled via game.json / F7 / New Map.
    bool hemisphereAmbient = true;
    bool finiteAmmo = true;      // guns consume ammo items
    bool minedBlockDrops = true; // broken blocks enter the breaker's inventory
    bool penetration = true;     // bullets spend budget passing through materials
    bool blockDamage = true;     // blocks chip (hp) instead of breaking on first hit
    bool dropOnDeath = true;     // a killed player scatters part of their bag as world pickups
    // Metres per voxel — world-defining, applied to meat::kVoxelSize at startup (and again
    // from Welcome on a join client so host scale wins). Devs pick anything from fine
    // (< 0.5) to chunkier-than-Minecraft (> 1). Must match across a session.
    float voxelSize = 0.5f; // == meat::kDefaultVoxelSize (avoid the heavy Chunk.h include here)
    // F1 interest management: only replicate non-player entities within this many
    // metres of each client's own player. 0 = disabled (every client gets every
    // entity — the historical behaviour). Players are always replicated regardless
    // (you must see who can shoot you). Cuts snapshot bandwidth on large maps.
    float interestRadius = 0.0f;

    std::uint8_t flagsByte() const {
        return static_cast<std::uint8_t>((finiteAmmo ? 1 : 0) | (minedBlockDrops ? 2 : 0) |
                                         (penetration ? 4 : 0) | (blockDamage ? 8 : 0) |
                                         (dropOnDeath ? 16 : 0) |
                                         ((static_cast<std::uint8_t>(terrain) & 0x3) << 5));
    }
    void setFlagsByte(std::uint8_t f) {
        finiteAmmo = (f & 1) != 0;
        minedBlockDrops = (f & 2) != 0;
        penetration = (f & 4) != 0;
        blockDamage = (f & 8) != 0;
        dropOnDeath = (f & 16) != 0;
        terrain = static_cast<Terrain>((f >> 5) & 0x3);
    }
};

} // namespace meat
