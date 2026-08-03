#include "game/Engine.h"
#include "engine/core/Log.h"
#include "engine/level/MeshLevel.h"
#include "editor/RoomEditor.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

// Apply genre template presets (camera + default terrain/env). Explicit
// terrain/environment/perspective fields applied after this still win.
void applyTemplatePreset(meat::EngineConfig& config, const std::string& t) {
    using Perspective = meat::GameRules::Perspective;
    using Template = meat::GameRules::Template;
    using Terrain = meat::GameRules::Terrain;
    using Environment = meat::GameRules::Environment;
    if (t == "tps" || t == "third") {
        config.rules.gameTemplate = Template::Tps;
        config.rules.perspective = Perspective::Third;
    } else if (t == "space" || t == "spaceship") {
        config.rules.gameTemplate = Template::Space;
        config.rules.perspective = Perspective::First;
        config.rules.terrain = Terrain::Void;
        config.rules.environment = Environment::Space;
        config.rules.hemisphereAmbient = false;
    } else if (t == "racer" || t == "race") {
        config.rules.gameTemplate = Template::Racer;
        config.rules.perspective = Perspective::Third;
        config.rules.terrain = Terrain::Superflat;
        config.rules.environment = Environment::Surface;
    } else {
        config.rules.gameTemplate = Template::Fps;
        config.rules.perspective = Perspective::First;
    }
}

void applyTerrainString(meat::EngineConfig& config, const std::string& t) {
    using Terrain = meat::GameRules::Terrain;
    config.rules.terrain = t == "superflat" ? Terrain::Superflat
                           : t == "void"     ? Terrain::Void
                                             : Terrain::Normal;
}

void applyEnvironmentString(meat::EngineConfig& config, const std::string& e) {
    using Environment = meat::GameRules::Environment;
    config.rules.environment = e == "underwater" ? Environment::Underwater
                               : e == "space"     ? Environment::Space
                                                  : Environment::Surface;
}

void applyPerspectiveString(meat::EngineConfig& config, const std::string& p) {
    using Perspective = meat::GameRules::Perspective;
    config.rules.perspective = (p == "third" || p == "tps") ? Perspective::Third
                                                            : Perspective::First;
}

// B5: nested "world" object and/or top-level map keys. Nested world wins when
// both are present so projects can group map defaults cleanly.
void applyWorldFields(meat::EngineConfig& config, const nlohmann::json& j) {
    // Template first (presets), then explicit overrides.
    if (j.contains("template") && j["template"].is_string())
        applyTemplatePreset(config, j["template"].get<std::string>());
    if (j.contains("terrain") && j["terrain"].is_string())
        applyTerrainString(config, j["terrain"].get<std::string>());
    if (j.contains("environment") && j["environment"].is_string())
        applyEnvironmentString(config, j["environment"].get<std::string>());
    if (j.contains("perspective") && j["perspective"].is_string())
        applyPerspectiveString(config, j["perspective"].get<std::string>());
    if (j.contains("mode") && j["mode"].is_string()) {
        const std::string m = j["mode"].get<std::string>();
        config.rules.gameMode = (m == "deathmatch" || m == "dm")
                                    ? meat::GameRules::GameMode::Deathmatch
                                    : meat::GameRules::GameMode::Sandbox;
    }
    if (j.contains("fragLimit") && j["fragLimit"].is_number_integer())
        config.rules.fragLimit = j["fragLimit"].get<int>();
    if (j.contains("seed") && j["seed"].is_number_unsigned())
        config.seed = j["seed"].get<std::uint32_t>();
    else if (j.contains("seed") && j["seed"].is_number_integer())
        config.seed = static_cast<std::uint32_t>(j["seed"].get<std::int64_t>());
    if (j.contains("hemisphereAmbient") && j["hemisphereAmbient"].is_boolean())
        config.rules.hemisphereAmbient = j["hemisphereAmbient"].get<bool>();
    // B2: static mesh level (triangle colliders + render).
    auto parseMeshInstance = [](const nlohmann::json& e,
                                float defaultScale) -> meat::MeshLevelInstance {
        meat::MeshLevelInstance inst;
        if (e.is_string()) {
            inst.assetPath = e.get<std::string>();
            inst.scale = defaultScale;
            return inst;
        }
        if (!e.is_object()) return inst;
        if (e.contains("asset") && e["asset"].is_string())
            inst.assetPath = e["asset"].get<std::string>();
        else if (e.contains("path") && e["path"].is_string())
            inst.assetPath = e["path"].get<std::string>();
        else if (e.contains("mesh") && e["mesh"].is_string())
            inst.assetPath = e["mesh"].get<std::string>();
        inst.scale = (e.contains("scale") && e["scale"].is_number()) ? e["scale"].get<float>()
                                                                     : defaultScale;
        glm::vec3 pos(0.0f);
        float yaw = 0.0f;
        if (e.contains("pos") && e["pos"].is_array() && e["pos"].size() >= 3) {
            pos = {e["pos"][0].get<float>(), e["pos"][1].get<float>(), e["pos"][2].get<float>()};
        }
        if (e.contains("position") && e["position"].is_array() && e["position"].size() >= 3) {
            pos = {e["position"][0].get<float>(), e["position"][1].get<float>(),
                   e["position"][2].get<float>()};
        }
        if (e.contains("yaw") && e["yaw"].is_number()) yaw = e["yaw"].get<float>();
        if (e.contains("yawDeg") && e["yawDeg"].is_number())
            yaw = e["yawDeg"].get<float>() * 0.01745329252f; // deg → rad
        inst.transform = meat::meshLevelTransform(pos, yaw);
        return inst;
    };

    if (j.contains("meshLevel") && j["meshLevel"].is_string())
        config.meshLevelAsset = j["meshLevel"].get<std::string>();
    if (j.contains("levelMesh") && j["levelMesh"].is_string()) // alias
        config.meshLevelAsset = j["levelMesh"].get<std::string>();
    if (j.contains("meshLevelScale") && j["meshLevelScale"].is_number())
        config.meshLevelScale = j["meshLevelScale"].get<float>();

    // Multi-mesh: "meshLevels": [ "a.obj", { "asset":"b.obj", "pos":[x,y,z], "yawDeg":90 } ]
    if (j.contains("meshLevels") && j["meshLevels"].is_array()) {
        for (const auto& e : j["meshLevels"]) {
            meat::MeshLevelInstance inst = parseMeshInstance(e, config.meshLevelScale);
            if (inst.assetPath.empty()) continue;
            config.meshLevelDesc.instances.push_back(std::move(inst));
        }
    }
    // Single object form: "meshLevel": { "asset":"...", "pos":[...] }
    if (j.contains("meshLevel") && j["meshLevel"].is_object()) {
        meat::MeshLevelInstance inst = parseMeshInstance(j["meshLevel"], config.meshLevelScale);
        if (!inst.assetPath.empty())
            config.meshLevelDesc.instances.push_back(std::move(inst));
    }
    // Promote single string shortcut into desc when desc empty.
    if (config.meshLevelDesc.instances.empty() && !config.meshLevelAsset.empty())
        config.meshLevelDesc =
            meat::makeMeshLevelDesc(config.meshLevelAsset, config.meshLevelScale);

    if (j.contains("levelType") && j["levelType"].is_string()) {
        const std::string lt = j["levelType"].get<std::string>();
        if (lt == "mesh" || lt == "MeshLevel") {
            if (!j.contains("terrain")) applyTerrainString(config, "void");
        }
    }
    if (!config.meshLevelDesc.instances.empty() && !j.contains("terrain") &&
        !j.contains("levelType"))
        applyTerrainString(config, "void");
}

// A game project is a folder with game.json (name/seed/rules) + scripts/ +
// optional assets/. Loading one lets a dev ship a game without touching C++.
void loadProject(meat::EngineConfig& config, const std::string& dir) {
    config.projectDir = dir;
    std::ifstream in(dir + "/game.json");
    if (!in) {
        meat::log::warn("project '{}': no game.json — using defaults", dir);
        return;
    }
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded()) {
        meat::log::error("project '{}': game.json is invalid JSON", dir);
        return;
    }
    if (j.contains("name")) config.serverName = j["name"].get<std::string>();
    if (j.contains("seed")) config.seed = j["seed"].get<std::uint32_t>();
    using Model = meat::GameRules::InventoryModel;
    if (j.contains("inventoryModel")) {
        const std::string m = j["inventoryModel"].get<std::string>();
        config.rules.inventoryModel = m == "grid"      ? Model::GridOnly
                                      : m == "weapons" ? Model::WeaponSlots
                                                       : Model::HotbarBackpack;
    }
    // Top-level map keys (legacy / still supported), then nested world overrides.
    applyWorldFields(config, j);
    if (j.contains("world") && j["world"].is_object()) applyWorldFields(config, j["world"]);

    config.rules.finiteAmmo = j.value("finiteAmmo", config.rules.finiteAmmo);
    config.rules.minedBlockDrops = j.value("minedBlockDrops", config.rules.minedBlockDrops);
    config.rules.penetration = j.value("penetration", config.rules.penetration);
    config.rules.blockDamage = j.value("blockDamage", config.rules.blockDamage);
    config.rules.voxelSize = j.value("voxelSize", config.rules.voxelSize);

    static const char* kTpl[] = {"fps", "tps", "space", "racer"};
    static const char* kTer[] = {"normal", "superflat", "void"};
    static const char* kEnv[] = {"surface", "underwater", "space"};
    const int ti = static_cast<int>(config.rules.gameTemplate);
    const int te = static_cast<int>(config.rules.terrain);
    const int en = static_cast<int>(config.rules.environment);
    meat::log::info("loaded project '{}' seed={} template={} terrain={} env={}",
                    config.serverName, config.seed, kTpl[ti < 0 || ti > 3 ? 0 : ti],
                    kTer[te < 0 || te > 2 ? 0 : te], kEnv[en < 0 || en > 2 ? 0 : en]);
}

meat::EngineConfig parseArgs(int argc, char** argv) {
    meat::EngineConfig config;
    using Mode = meat::EngineConfig::Mode;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--host") {
            config.mode = Mode::Host;
        } else if (arg == "--play") {
            config.mode = Mode::Game; // straight to singleplayer, skip the menu
        } else if (arg == "--name") {
            if (const char* n = next()) config.serverName = n;
        } else if (arg == "--password") {
            if (const char* p = next()) config.serverPassword = p;
        } else if (arg == "--maxplayers") {
            if (const char* n = next()) {
                const int v = std::atoi(n);
                if (v > 0) config.maxPlayers = v;
            }
        } else if (arg == "--master") {
            if (const char* m = next()) config.master = m;
        } else if (arg == "--shot") {
            config.mode = Mode::Game;
            if (const char* s = next()) config.autoShot = s;
        } else if (arg == "--join") {
            config.mode = Mode::Join;
            if (const char* addr = next()) {
                const std::string s = addr;
                if (const auto colon = s.find(':'); colon != std::string::npos) {
                    config.address = s.substr(0, colon);
                    config.port = static_cast<std::uint16_t>(std::atoi(s.c_str() + colon + 1));
                } else {
                    config.address = s;
                }
            }
        } else if (arg == "--server") {
            config.mode = Mode::Dedicated;
        } else if (arg == "--port") {
            if (const char* p = next()) config.port = static_cast<std::uint16_t>(std::atoi(p));
        } else if (arg == "--load") {
            if (const char* p = next()) config.loadPath = p;
        } else if (arg == "--seed") {
            if (const char* s = next())
                config.seed = static_cast<std::uint32_t>(std::strtoul(s, nullptr, 10));
        } else if (arg == "--project") {
            if (const char* d = next()) loadProject(config, d);
        } else if (arg == "--editor") {
            config.startEditor = true;
        } else if (arg == "--animshot") {
            config.mode = Mode::Game;
            config.animBooth = true;
            if (const char* s = next()) config.autoShot = s;
        } else if (arg == "--animmodel") {
            if (const char* m = next()) config.animModel = m;
        } else if (arg == "--animclip") {
            if (const char* m = next()) config.animClip = m;
        } else if (arg == "--animretarget") {
            if (const char* m = next()) config.animRetarget = m;
        } else if (arg == "--voxelsize") {
            if (const char* v = next()) config.rules.voxelSize = std::strtof(v, nullptr);
        } else if (arg == "--terrain") {
            if (const char* t = next()) applyTerrainString(config, t);
        } else if (arg == "--env") {
            if (const char* e = next()) applyEnvironmentString(config, e);
        } else if (arg == "--template") {
            if (const char* t = next()) applyTemplatePreset(config, t);
        } else if (arg == "--perspective") {
            if (const char* p = next()) applyPerspectiveString(config, p);
        } else if (arg == "--mode") {
            if (const char* m = next()) {
                const std::string s = m;
                config.rules.gameMode = (s == "deathmatch" || s == "dm")
                                            ? meat::GameRules::GameMode::Deathmatch
                                            : meat::GameRules::GameMode::Sandbox;
            }
        } else {
            meat::log::warn("unknown argument '{}'", arg);
        }
    }
    return config;
}

} // namespace

int main(int argc, char** argv) {
    meat::Engine engine;
    engine.setEditor(std::make_unique<meat::RoomEditor>());
    return engine.run(parseArgs(argc, argv));
}
