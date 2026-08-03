// Lua-defined effects: a script composes custom effects from the same
// server-authoritative primitives the built-in effect kinds use. This drives
// ScriptHost directly (no GPU, no ServerSim) with recording capabilities and a
// temp script, proving the Lua -> sol2 -> ScriptApi bridge passes every argument
// through intact for ignite / chain / spawn. The primitives themselves are
// covered by the ServerSim effect tests; here the contract is the binding.

#include "Harness.h"

#include "engine/script/ScriptHost.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

using meattest::check;

void testLuaComposesEffectPrimitives() {
    std::printf("Lua drives the effect primitives (Lua-defined effects)\n");
    namespace fs = std::filesystem;

    struct Rec {
        bool ignited = false;
        int ip = 0, isrc = 0;
        float dps = 0, sec = 0;
        bool chained = false;
        float cx = 0, cd = 0, crange = 0;
        int cmax = 0, csrc = 0;
        bool turret = false, companion = false;
        int tsrc = 0;
        float tx = 0, tz = 0;
    } rec;

    meat::ScriptApi api;
    api.log = [](const std::string&) {};
    api.ignite = [&](int p, float d, float s, int src) {
        rec.ignited = true; rec.ip = p; rec.dps = d; rec.sec = s; rec.isrc = src;
    };
    api.chainDamage = [&](float x, float, float, float dmg, int mx, float r, int src) {
        rec.chained = true; rec.cx = x; rec.cd = dmg; rec.cmax = mx; rec.crange = r; rec.csrc = src;
    };
    api.spawnTurret = [&](int src, float x, float, float z) -> int {
        rec.turret = true; rec.tsrc = src; rec.tx = x; rec.tz = z; return 42;
    };
    api.spawnCompanion = [&](int, float, float, float) -> int {
        rec.companion = true; return 43;
    };

    meat::ScriptHost host;
    host.bind(std::move(api));

    const fs::path dir = fs::temp_directory_path() / "meat_lua_effect_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    {
        std::ofstream f(dir / "effect.lua");
        f << "function on_init(seed)\n"
             "  game.ignite(7, 20.0, 1.5, 3)\n"
             "  game.chain_damage(1.0, 2.0, 3.0, 25.0, 2, 8.0, 9)\n"
             "  game.spawn_turret(4, 10.0, 0.0, -5.0)\n"
             "  game.spawn_companion(4, 11.0, 0.0, -5.0)\n"
             "end\n";
    }

    check(host.loadDir(dir.string()), "the effect script loaded");
    host.onInit(1);
    fs::remove_all(dir, ec);

    check(rec.ignited && rec.ip == 7 && rec.dps == 20.0f && rec.sec == 1.5f && rec.isrc == 3,
          "game.ignite bridged to applyDamageOverTime with its args intact");
    check(rec.chained && rec.cx == 1.0f && rec.cd == 25.0f && rec.cmax == 2 &&
              rec.crange == 8.0f && rec.csrc == 9,
          "game.chain_damage bridged to applyChainDamage with its args intact");
    check(rec.turret && rec.tsrc == 4 && rec.tx == 10.0f && rec.tz == -5.0f,
          "game.spawn_turret bridged to spawnTurret with its args intact");
    check(rec.companion, "game.spawn_companion bridged to spawnCompanion");
}

} // namespace

namespace meattest {

void runScripting() {
    testLuaComposesEffectPrimitives();
}

} // namespace meattest
