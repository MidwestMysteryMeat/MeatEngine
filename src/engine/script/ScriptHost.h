#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace meat {

// The capability surface a script may touch. The host (ServerSim) fills these in
// before loading scripts, so Lua can ONLY do what the server explicitly grants —
// no filesystem, no sockets, no engine internals. Server-authoritative by
// construction: every callback runs on the server tick.
struct ScriptApi {
    std::function<void(const std::string&)> log;
    std::function<void(int, int, int, int)> setBlock;     // x,y,z,blockId (0=air)
    std::function<int(int, int, int)> getBlock;           // → blockId
    std::function<void(float, float, float, int, int)> spawnPickup; // x,y,z,itemId,count
    std::function<int()> playerCount;
    std::function<int(int, int)> randi;                   // seeded deterministic [lo,hi]
    std::function<std::uint64_t()> tick;
    // Named item ids the default game exposes, so scripts don't hardcode wire ids.
    std::function<int(const std::string&)> itemId;        // "ammo9mm" → id, 0 if unknown
};

// Embeds Lua (sol2) with a sandboxed standard library. Loads scripts from a
// directory and dispatches engine hooks to optional global Lua functions. All
// Lua execution is wrapped: a script error is logged and swallowed, never
// crashing the server.
class ScriptHost {
public:
    ScriptHost();
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    void bind(ScriptApi api);          // install the capability table; call before load
    bool loadDir(const std::string& dir); // runs every *.lua; false if none loaded
    bool loaded() const;

    // Dispatch to Lua globals if the script defined them (all optional):
    void onInit(std::uint32_t seed);        // on_init(seed)
    void onPlayerJoin(std::uint32_t peer);  // on_player_join(peer)
    void onPlayerDeath(std::uint32_t peer);  // on_player_death(peer)
    void onTick(std::uint64_t tick);        // on_tick(tick) — server may throttle

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl; // pimpl: sol2 headers stay out of the engine
};

} // namespace meat
