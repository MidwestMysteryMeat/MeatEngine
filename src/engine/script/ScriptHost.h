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
    // C6 object helpers (world props / node-graph Get World Object).
    std::function<void(int propId, float seconds)> highlightProp; // networked cyan pulse
    // Returns true and writes x,y,z when prop id exists.
    std::function<bool(int propId, float& x, float& y, float& z)> propPos;
    std::function<int()> propCount;
    // C6-b: combat / messaging hooks for node graphs.
    // Damage a connected peer; amount is clamped. Lethal hits respawn at pad.
    std::function<void(int peerId, float amount)> damagePlayer;
    // HUD toast on all clients (ScriptFx kind=1) + server log.
    std::function<void(const std::string&)> announce;
    // Current health of a peer (0 if unknown / disconnected).
    std::function<float(int peerId)> playerHealth;
    // C6-c: named watch value for the editor Watches panel (+ optional log line).
    std::function<void(const std::string& name, const std::string& value)> watch;
    // Effect primitives: the same server-authoritative building blocks the built-in
    // effect kinds use, exposed so scripts can compose their own (Lua-defined)
    // effects. Every call runs on the server tick and is bounds-checked host-side.
    std::function<void(int peer, float dps, float seconds, int source)> ignite;
    std::function<void(float x, float y, float z, float damage, int maxTargets,
                       float range, int source)>
        chainDamage;
    std::function<int(int source, float x, float y, float z)> spawnTurret;    // → entity id
    std::function<int(int source, float x, float y, float z)> spawnCompanion; // → entity id
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
    bool reload();                     // re-run the last loaded dir (live editing)
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
