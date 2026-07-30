#include "engine/core/Engine.h"
#include "engine/core/Log.h"
#include "editor/RoomEditor.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

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
    config.rules.finiteAmmo = j.value("finiteAmmo", config.rules.finiteAmmo);
    config.rules.minedBlockDrops = j.value("minedBlockDrops", config.rules.minedBlockDrops);
    config.rules.penetration = j.value("penetration", config.rules.penetration);
    config.rules.blockDamage = j.value("blockDamage", config.rules.blockDamage);
    config.rules.voxelSize = j.value("voxelSize", config.rules.voxelSize);
    meat::log::info("loaded project '{}' (seed {})", config.serverName, config.seed);
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
