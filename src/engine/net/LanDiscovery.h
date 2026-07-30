#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// LAN session discovery: LanBeacon (host side) broadcasts a small UDP ad about
// once a second; LanDiscovery (menu side) listens on the beacon port and keeps
// a live, deduped, expiring list. Wire format (see ARCHITECTURE.md): "MEAT"
// magic, u8 protocolVersion, u16 gamePort, u8 players, u8 maxPlayers,
// string name. Malformed packets are dropped silently.
//
// Discovery is a convenience layer: every failure (winsock init, socket
// creation, bind) logs a warning once and leaves the object in a disabled
// no-op state — it must never take down a game. Winsock lives entirely in the
// .cpp; this header stays platform-clean. Main-thread use only, like the rest
// of the net layer.
namespace meat {

struct ServerAd {
    std::string name;
    std::string address; // dotted IPv4 of the announcing host
    std::uint16_t port = 0;
    int players = 0;
    int maxPlayers = 0;
};

inline constexpr std::uint16_t kLanBeaconPort = 26010;

struct WsaRuntime; // ref-counted winsock init, defined in the .cpp

class LanBeacon {
public:
    LanBeacon();
    ~LanBeacon(); // implies stop()
    LanBeacon(const LanBeacon&) = delete;
    LanBeacon& operator=(const LanBeacon&) = delete;

    // False on failure (logged); the beacon is then disabled and update() no-ops.
    bool start(std::uint16_t gamePort, const std::string& name);
    void update(int players, int maxPlayers); // call per frame; rate-limits to ~1 Hz
    void stop();

private:
    static constexpr std::uintptr_t kNoSocket = ~std::uintptr_t{0};

    std::unique_ptr<WsaRuntime> m_wsa;
    std::uintptr_t m_socket = kNoSocket; // SOCKET/fd kept opaque so winsock stays out of the header
    std::uint16_t m_gamePort = 0;
    std::string m_name;
    bool m_enabled = false;
    std::chrono::steady_clock::time_point m_lastSend{};
};

class LanDiscovery {
public:
    LanDiscovery();
    ~LanDiscovery(); // implies stop()
    LanDiscovery(const LanDiscovery&) = delete;
    LanDiscovery& operator=(const LanDiscovery&) = delete;

    // Binds 0.0.0.0:26010 (SO_REUSEADDR, so a hosting process can browse too).
    // False on failure (logged); servers() then always returns empty.
    bool start();
    std::vector<ServerAd> servers() const; // drains pending beacons, expires stale entries
    void stop();

private:
    static constexpr std::uintptr_t kNoSocket = ~std::uintptr_t{0};

    struct Entry {
        ServerAd ad;
        std::chrono::steady_clock::time_point lastSeen;
    };

    std::unique_ptr<WsaRuntime> m_wsa;
    std::uintptr_t m_socket = kNoSocket;
    bool m_enabled = false;
    // servers() is logically const (a query) but drains the socket into this
    // cache — hence mutable, matching the contract signature.
    mutable std::vector<Entry> m_entries;
    mutable bool m_recvWarned = false;
};

// Parses the master server's GET /servers JSON response. Tolerant: returns
// empty on malformed JSON, skips entries with missing/mistyped fields.
std::vector<ServerAd> parseServerList(const std::string& json);

} // namespace meat
