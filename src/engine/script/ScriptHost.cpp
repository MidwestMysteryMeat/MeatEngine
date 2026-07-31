#include "engine/script/ScriptHost.h"
#include "engine/core/Log.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4127 4324 4459) // sol2/Lua third-party, not held to /W4
#endif
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <filesystem>

namespace meat {

namespace {
// Instruction-budget guard: a script cannot hang the 60 Hz server thread. The
// count hook fires every N VM instructions and raises a Lua error, which the
// protected_function around every dispatch catches. ~5M instructions is a
// generous per-call budget (normal hooks use a few thousand).
constexpr int kInstructionBudget = 5'000'000;
void budgetHook(lua_State* L, lua_Debug*) {
    luaL_error(L, "script exceeded instruction budget (infinite loop?)");
}
} // namespace

struct ScriptHost::Impl {
    sol::state lua;
    ScriptApi api;
    bool anyLoaded = false;
    std::string lastDir;

    // Run a protected call to an optional global function under the instruction
    // budget; log+swallow errors (including a budget overrun).
    template <typename... Args> void dispatch(const char* name, Args&&... args) {
        if (!anyLoaded) return;
        sol::protected_function fn = lua[name];
        if (!fn.valid()) return;
        lua_State* L = lua.lua_state();
        lua_sethook(L, budgetHook, LUA_MASKCOUNT, kInstructionBudget);
        sol::protected_function_result r = fn(std::forward<Args>(args)...);
        lua_sethook(L, nullptr, 0, 0);
        if (!r.valid()) {
            const sol::error err = r;
            log::error("script: {} failed: {}", name, err.what());
        }
    }
};

ScriptHost::ScriptHost() : m_impl(std::make_unique<Impl>()) {
    // Sandboxed stdlib: math/table/string are pure; base gives print/pairs/etc.
    // No io, os, package, or debug — no filesystem, process, or native loading.
    m_impl->lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::table,
                               sol::lib::string);
    // base STILL exposes load/loadfile/dofile (arbitrary file read + bytecode
    // execution, a classic sandbox escape) and collectgarbage — nil them out.
    for (const char* fn : {"load", "loadfile", "dofile", "loadstring", "collectgarbage"})
        m_impl->lua[fn] = sol::nil;
}

ScriptHost::~ScriptHost() = default;

void ScriptHost::bind(ScriptApi api) {
    m_impl->api = std::move(api);
    const ScriptApi& a = m_impl->api;

    // The single capability table exposed to scripts. Missing callbacks are
    // simply absent from the table rather than crashing.
    sol::table game = m_impl->lua.create_named_table("game");
    if (a.log)
        game.set_function("log", [&a](const std::string& s) { a.log(s); });
    if (a.setBlock)
        game.set_function("set_block", [&a](int x, int y, int z, int b) { a.setBlock(x, y, z, b); });
    if (a.getBlock)
        game.set_function("get_block", [&a](int x, int y, int z) { return a.getBlock(x, y, z); });
    if (a.spawnPickup)
        game.set_function("spawn_pickup", [&a](float x, float y, float z, int item, int count) {
            a.spawnPickup(x, y, z, item, count);
        });
    if (a.playerCount)
        game.set_function("player_count", [&a] { return a.playerCount(); });
    if (a.randi)
        game.set_function("randi", [&a](int lo, int hi) { return a.randi(lo, hi); });
    if (a.tick)
        game.set_function("tick", [&a] { return static_cast<double>(a.tick()); });
    if (a.itemId)
        game.set_function("item_id", [&a](const std::string& n) { return a.itemId(n); });
    if (a.highlightProp)
        game.set_function("highlight_prop",
                          [&a](int id, double seconds) {
                              a.highlightProp(id, static_cast<float>(seconds));
                          });
    if (a.propPos)
        game.set_function("prop_pos", [&a](int id, sol::this_state ts) -> sol::object {
            float x = 0, y = 0, z = 0;
            if (!a.propPos(id, x, y, z)) return sol::make_object(ts, sol::lua_nil);
            sol::state_view lua(ts);
            sol::table t = lua.create_table(3, 0);
            t[1] = x;
            t[2] = y;
            t[3] = z;
            return sol::make_object(ts, t);
        });
    if (a.propCount)
        game.set_function("prop_count", [&a] { return a.propCount(); });
}

bool ScriptHost::loadDir(const std::string& dir) {
    m_impl->lastDir = dir;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.path().extension() == ".lua") files.push_back(entry.path());
    std::sort(files.begin(), files.end()); // deterministic load order across peers

    for (const auto& path : files) {
        lua_State* L = m_impl->lua.lua_state();
        lua_sethook(L, budgetHook, LUA_MASKCOUNT, kInstructionBudget); // guard top-level code
        sol::protected_function_result r =
            m_impl->lua.safe_script_file(path.string(), sol::script_pass_on_error);
        lua_sethook(L, nullptr, 0, 0);
        if (!r.valid()) {
            const sol::error err = r;
            log::error("script '{}' failed to load: {}", path.string(), err.what());
            continue;
        }
        m_impl->anyLoaded = true;
        log::info("script loaded: {}", path.filename().string());
    }
    return m_impl->anyLoaded;
}

bool ScriptHost::reload() {
    // Re-run the scripts: top-level code re-executes and hook functions redefine
    // over the persistent `game` table and lua state. on_init won't re-fire (the
    // world is already up), but edited on_tick/join logic takes effect live.
    if (m_impl->lastDir.empty()) return false;
    log::info("script: reloading '{}'", m_impl->lastDir);
    return loadDir(m_impl->lastDir);
}

bool ScriptHost::loaded() const { return m_impl->anyLoaded; }

void ScriptHost::onInit(std::uint32_t seed) { m_impl->dispatch("on_init", seed); }
void ScriptHost::onPlayerJoin(std::uint32_t peer) { m_impl->dispatch("on_player_join", peer); }
void ScriptHost::onPlayerDeath(std::uint32_t peer) { m_impl->dispatch("on_player_death", peer); }
void ScriptHost::onTick(std::uint64_t tick) {
    m_impl->dispatch("on_tick", static_cast<double>(tick));
}

} // namespace meat
