#pragma once
#include "engine/asset/ModelLoader.h"
#include "engine/core/Log.h"
#include "game/ShipControl.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace meat {

// H4 hull catalog — CC-BY 4.0 Fab free packs staged under assets/models/ships/
// (or resolved from G:\VaultCache when staged copies are missing).
// Authors (Fab seller names) — keep in sync with assets/ATTRIBUTION.md:
//   cyber   → JamyzGenius  (Floating Cyber Ship JFG)
//   star    → JazOone3D    (SpaceShip)
//   lowpoly → ABJVNK       (Lowpoly Spaceship)
// Decor: Sebastian Sosnowski (Junk Yard part2), Gerardo Justel (Spacestation 7).
inline constexpr int kShipHullCount = 3;

struct ShipHullDef {
    const char* id;          // stable name
    const char* author;      // CC-BY credit (Fab user_seller_name)
    const char* listing;     // Fab listing title
    const char* stagedPath;  // project-relative preferred
    const char* vaultPath;   // absolute fallback (local dev vault)
    float targetLength;      // metres along longest axis after import
    // Optional texture override when the FBX embeds no usable path (common for
    // extracted Fab packs whose textures sit next to the mesh).
    const char* stagedAlbedo;
};

inline const std::array<ShipHullDef, kShipHullCount>& shipHullDefs() {
    static const std::array<ShipHullDef, kShipHullCount> kDefs = {{
        {"cyber", "JamyzGenius", "Floating Cyber Ship JFG - Roblox Showcase Prop",
         "assets/models/ships/cyber_ship/ship.fbx",
         "G:/VaultCache/FabLibrary/Floating_Cyber_Ship_JFG_-_Roblox_Showcase_Prop-64a45a1d/"
         "fbx/floating-cyber-ship-jfg-_extracted/source/model.fbx",
         6.5f, "assets/models/ships/cyber_ship/albedo.jpeg"},
        {"star", "JazOone3D", "SpaceShip",
         "assets/models/ships/star_ship/ship.fbx",
         "G:/VaultCache/FabLibrary/SpaceShip-14265e80/fbx/spaceship_extracted/source/"
         "SpaceShip_extracted/SpaceShip.fbx",
         8.0f, "assets/models/ships/star_ship/albedo.jpg"},
        {"lowpoly", "ABJVNK", "Lowpoly Spaceship",
         "assets/models/ships/lowpoly/scene.gltf",
         "G:/VaultCache/FabLibrary/Lowpoly_Spaceship-69cc1137/gltf/converted/"
         "lowpoly_spaceship_gltf_extracted/scene.gltf",
         7.0f, "assets/models/ships/lowpoly/textures/freeble_baseColor.png"},
    }};
    return kDefs;
}

inline std::filesystem::path resolveShipHullPath(int variant) {
    const auto& defs = shipHullDefs();
    const int i = variant < 0 ? 0 : (variant >= kShipHullCount ? 0 : variant);
    const ShipHullDef& d = defs[static_cast<std::size_t>(i)];
    if (std::filesystem::exists(d.stagedPath)) return d.stagedPath;
    if (std::filesystem::exists(d.vaultPath)) return d.vaultPath;
    return d.stagedPath; // may fail at load; caller logs
}

// Load hull geometry sized to targetLength, centered (base on floor, XZ mid).
// halfExtents is the post-import local AABB half-size for the kinematic collider.
inline std::optional<StaticModel> loadShipHull(int variant, glm::vec3& outHalfExtents) {
    const auto& defs = shipHullDefs();
    const int i = variant < 0 ? 0 : (variant >= kShipHullCount ? 0 : variant);
    const ShipHullDef& d = defs[static_cast<std::size_t>(i)];
    const auto path = resolveShipHullPath(i);
    if (!std::filesystem::exists(path)) {
        log::warn("ship hull '{}': missing at '{}' (and vault fallback)", d.id, path.string());
        outHalfExtents = kShipHalfExtents;
        return std::nullopt;
    }
    // Probe raw size, then re-import at the scale that hits targetLength.
    auto probe = loadStaticModel(path, {.scale = 1.0f, .center = false});
    if (!probe) {
        outHalfExtents = kShipHalfExtents;
        return std::nullopt;
    }
    const glm::vec3 raw = probe->boundsMax - probe->boundsMin;
    const float longest = std::max({raw.x, raw.y, raw.z, 0.01f});
    const float scale = d.targetLength / longest;
    auto model = loadStaticModel(path, {.scale = scale, .center = true});
    if (!model) {
        outHalfExtents = kShipHalfExtents;
        return std::nullopt;
    }
    // Prefer staged albedo when Assimp didn't resolve the extracted pack's textures.
    if (model->albedo.empty() && d.stagedAlbedo && std::filesystem::exists(d.stagedAlbedo))
        model->albedo = d.stagedAlbedo;
    const glm::vec3 size = model->boundsMax - model->boundsMin;
    outHalfExtents = glm::max(size * 0.5f, glm::vec3(0.2f));
    log::info("ship hull '{}' by {}: scale {:.4f}, half {:.2f}x{:.2f}x{:.2f}", d.id, d.author,
              scale, outHalfExtents.x, outHalfExtents.y, outHalfExtents.z);
    return model;
}

// EntityState.anim packing for ships:
//   bit7 = occupied (player pilot), bit6 = AI traffic, bits0-5 = hull variant.
inline std::uint8_t packShipAnim(bool occupied, bool ai, int variant) {
    const auto v = static_cast<std::uint8_t>(variant < 0 ? 0 : (variant & 0x3f));
    return static_cast<std::uint8_t>((occupied ? 0x80u : 0u) | (ai ? 0x40u : 0u) | v);
}
inline int shipVariantFromAnim(std::uint8_t anim) { return static_cast<int>(anim & 0x3f); }
inline bool shipOccupiedFromAnim(std::uint8_t anim) { return (anim & 0x80u) != 0; }
inline bool shipAiFromAnim(std::uint8_t anim) { return (anim & 0x40u) != 0; }

// Decor props (station / junkyard) — vault or staged; optional.
// junkyard → Sebastian Sosnowski (SpaceShips Junk Yard ASSET part2)
// station  → Gerardo Justel (Spacestation 7 - Procedural)
inline constexpr const char* kJunkyardAuthor = "Sebastian Sosnowski";
inline constexpr const char* kJunkyardStaged = "assets/models/ships/junkyard/set.fbx";
inline constexpr const char* kJunkyardVault =
    "G:/VaultCache/FabLibrary/SpaceShips_Junk_Yard_ASSET__part2_-81e5d377/fbx/"
    "spaceships-junk-yard-ass_extracted/source/JunkYard2_SetTwo.fbx";
inline constexpr const char* kStationAuthor = "Gerardo Justel";
inline constexpr const char* kStationStaged = "assets/models/ships/station/scene.gltf";
inline constexpr const char* kStationVault =
    "G:/VaultCache/FabLibrary/Spacestation_7_-_Procedural-bf84b4bd/gltf/converted/"
    "spacestation_7_procedura_extracted/scene.gltf";

// Resolve first existing path among candidates.
inline std::string resolveDecorPath(const char* staged, const char* vault) {
    if (staged && std::filesystem::exists(staged)) return staged;
    if (vault && std::filesystem::exists(vault)) return vault;
    return {};
}

// World TRS that sizes a raw mesh to `targetLongest` metres and parks it at `pos`.
// Uses a probe import so vault packs of unknown unit scale still look right.
inline std::optional<glm::mat4> decorTransform(const std::string& path, glm::vec3 pos,
                                               float targetLongest, float yaw = 0.0f) {
    if (path.empty() || !std::filesystem::exists(path)) return std::nullopt;
    const auto probe = loadStaticModel(path, {.scale = 1.0f, .center = false});
    if (!probe) return std::nullopt;
    const glm::vec3 raw = probe->boundsMax - probe->boundsMin;
    const float longest = std::max({raw.x, raw.y, raw.z, 0.01f});
    const float s = targetLongest / longest;
    // center=true mesh sits base on y=0 after load in addProp; lift by half height * s.
    const float lift = (raw.y * 0.5f) * s;
    return glm::translate(glm::mat4(1.0f), pos + glm::vec3(0.0f, lift, 0.0f)) *
           glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0)) *
           glm::scale(glm::mat4(1.0f), glm::vec3(s));
}

} // namespace meat
