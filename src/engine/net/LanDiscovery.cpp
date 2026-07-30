#include "engine/net/LanDiscovery.h"

#include "engine/core/Log.h"
#include "engine/net/ByteStream.h"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace meat {

// Ref-counted RAII wrapper for WSAStartup/WSACleanup, same pattern as
// EnetRuntime in EnetTransport.cpp. Plain statics are fine: discovery objects
// live on the main thread. No-op on POSIX.
struct WsaRuntime {
#ifdef _WIN32
    WsaRuntime() {
        if (s_refs++ == 0) {
            WSADATA data{};
            s_initialized = WSAStartup(MAKEWORD(2, 2), &data) == 0;
            if (!s_initialized) {
                log::warn("discovery: WSAStartup failed");
            }
        }
    }
    ~WsaRuntime() {
        if (--s_refs == 0 && s_initialized) {
            WSACleanup();
            s_initialized = false;
        }
    }
    WsaRuntime(const WsaRuntime&) = delete;
    WsaRuntime& operator=(const WsaRuntime&) = delete;

    bool ok() const { return s_initialized; }

private:
    static inline int s_refs = 0;
    static inline bool s_initialized = false;
#else
    bool ok() const { return true; }
#endif
};

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kNativeInvalid = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kNativeInvalid = -1;
#endif

constexpr std::uint8_t kBeaconVersion = 1;
constexpr std::array<std::uint8_t, 4> kBeaconMagic = {'M', 'E', 'A', 'T'};
constexpr std::chrono::seconds kBeaconInterval{1};
constexpr std::chrono::seconds kEntryTtl{5};

NativeSocket toNative(std::uintptr_t handle) {
    return static_cast<NativeSocket>(handle);
}

std::uintptr_t fromNative(NativeSocket s) {
    return static_cast<std::uintptr_t>(s);
}

void closeNative(NativeSocket s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

bool setNonBlocking(NativeSocket s) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool lastErrorWouldBlock() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

// Windows quirk: a UDP recvfrom can fail with WSAECONNRESET when an earlier
// send on the same socket drew an ICMP port-unreachable. Transient — skip the
// packet, keep draining.
bool lastErrorTransientRecv() {
#ifdef _WIN32
    const int err = WSAGetLastError();
    return err == WSAECONNRESET || err == WSAEMSGSIZE;
#else
    return errno == ECONNRESET || errno == EINTR;
#endif
}

std::uint8_t clampU8(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

} // namespace

// ---------------------------------------------------------------- beacon ----

LanBeacon::LanBeacon() = default;

LanBeacon::~LanBeacon() {
    stop();
}

bool LanBeacon::start(std::uint16_t gamePort, const std::string& name) {
    stop(); // restart-safe
    m_wsa = std::make_unique<WsaRuntime>();
    if (!m_wsa->ok()) {
        m_wsa.reset();
        return false; // WsaRuntime already warned
    }
    const NativeSocket sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kNativeInvalid) {
        log::warn("lan beacon: socket creation failed; LAN announce disabled");
        m_wsa.reset();
        return false;
    }
    const int broadcastOn = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcastOn),
                   sizeof(broadcastOn)) != 0 ||
        !setNonBlocking(sock)) {
        log::warn("lan beacon: socket setup failed; LAN announce disabled");
        closeNative(sock);
        m_wsa.reset();
        return false;
    }
    m_socket = fromNative(sock);
    m_gamePort = gamePort;
    m_name = name;
    m_enabled = true;
    // Back-date so the first update() broadcasts immediately.
    m_lastSend = std::chrono::steady_clock::now() - 2 * kBeaconInterval;
    log::info("lan beacon: announcing '{}' (game port {}) on UDP {}", name, gamePort,
              kLanBeaconPort);
    return true;
}

void LanBeacon::update(int players, int maxPlayers) {
    if (!m_enabled) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastSend < kBeaconInterval) {
        return;
    }
    m_lastSend = now;

    ByteWriter writer;
    for (const std::uint8_t byte : kBeaconMagic) {
        writer.write(byte);
    }
    writer.write(kBeaconVersion);
    writer.write(m_gamePort);
    writer.write(clampU8(players));
    writer.write(clampU8(maxPlayers));
    writer.write(std::string_view{m_name});

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(kLanBeaconPort);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const auto sent =
        sendto(toNative(m_socket), reinterpret_cast<const char*>(writer.data().data()),
               static_cast<int>(writer.size()), 0, reinterpret_cast<const sockaddr*>(&dst),
               sizeof(dst));
    if (sent < 0 && !lastErrorWouldBlock()) { // would-block: just try again next second
        log::warn("lan beacon: broadcast send failed; LAN announce disabled");
        stop();
    }
}

void LanBeacon::stop() {
    if (m_socket != kNoSocket) {
        closeNative(toNative(m_socket));
        m_socket = kNoSocket;
    }
    m_wsa.reset();
    m_enabled = false;
}

// ------------------------------------------------------------- discovery ----

LanDiscovery::LanDiscovery() = default;

LanDiscovery::~LanDiscovery() {
    stop();
}

bool LanDiscovery::start() {
    stop();
    m_wsa = std::make_unique<WsaRuntime>();
    if (!m_wsa->ok()) {
        m_wsa.reset();
        return false;
    }
    const NativeSocket sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kNativeInvalid) {
        log::warn("lan discovery: socket creation failed; LAN browsing disabled");
        m_wsa.reset();
        return false;
    }
    // SO_REUSEADDR so several browsers (e.g. two instances on one dev box) can
    // share the beacon port; broadcast datagrams are delivered to every bound
    // socket on Windows and Linux alike.
    const int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kLanBeaconPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        !setNonBlocking(sock)) {
        log::warn("lan discovery: bind on UDP {} failed; LAN browsing disabled", kLanBeaconPort);
        closeNative(sock);
        m_wsa.reset();
        return false;
    }
    m_socket = fromNative(sock);
    m_enabled = true;
    log::info("lan discovery: listening on UDP {}", kLanBeaconPort);
    return true;
}

std::vector<ServerAd> LanDiscovery::servers() const {
    const auto now = std::chrono::steady_clock::now();
    if (m_enabled) {
        // Drain every pending beacon. Buffer comfortably exceeds any beacon a
        // conforming host sends (fixed fields + u16-prefixed name).
        char buf[1500];
        for (;;) {
            sockaddr_in from{};
#ifdef _WIN32
            int fromLen = sizeof(from);
#else
            socklen_t fromLen = sizeof(from);
#endif
            const auto received =
                recvfrom(toNative(m_socket), buf, sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (received < 0) {
                if (lastErrorWouldBlock()) {
                    break; // queue empty
                }
                if (lastErrorTransientRecv()) {
                    continue;
                }
                if (!m_recvWarned) {
                    m_recvWarned = true;
                    log::warn("lan discovery: recvfrom failed; results may be stale");
                }
                break;
            }

            ByteReader reader(
                std::as_bytes(std::span<const char>{buf, static_cast<std::size_t>(received)}));
            bool magicOk = true;
            for (const std::uint8_t expected : kBeaconMagic) {
                magicOk = magicOk && reader.read<std::uint8_t>().value_or(0) == expected;
            }
            const std::uint8_t version = reader.read<std::uint8_t>().value_or(0);
            std::uint16_t gamePort = 0;
            std::uint8_t players = 0;
            std::uint8_t maxPlayers = 0;
            std::string name;
            reader.read(gamePort);
            reader.read(players);
            reader.read(maxPlayers);
            reader.read(name);
            if (!magicOk || version != kBeaconVersion || !reader.ok() || gamePort == 0) {
                continue; // malformed or foreign — drop silently per contract
            }

            char ip[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip)) == nullptr) {
                continue;
            }

            ServerAd ad{name, ip, gamePort, players, maxPlayers};
            const auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) {
                return e.ad.address == ad.address && e.ad.port == ad.port;
            });
            if (it != m_entries.end()) {
                it->ad = std::move(ad);
                it->lastSeen = now;
            } else {
                m_entries.push_back({std::move(ad), now});
            }
        }
    }

    std::erase_if(m_entries, [&](const Entry& e) { return now - e.lastSeen > kEntryTtl; });

    std::vector<ServerAd> out;
    out.reserve(m_entries.size());
    for (const Entry& entry : m_entries) {
        out.push_back(entry.ad);
    }
    return out;
}

void LanDiscovery::stop() {
    if (m_socket != kNoSocket) {
        closeNative(toNative(m_socket));
        m_socket = kNoSocket;
    }
    m_wsa.reset();
    m_enabled = false;
    m_entries.clear();
    m_recvWarned = false;
}

// ---------------------------------------------------------- master parse ----

std::vector<ServerAd> parseServerList(const std::string& json) {
    std::vector<ServerAd> out;
    // Non-throwing parse: a garbled master response yields an empty list, never
    // an exception on the menu path.
    const nlohmann::json doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded() || !doc.is_array()) {
        return out;
    }
    for (const auto& entry : doc) {
        if (!entry.is_object()) {
            continue;
        }
        const auto name = entry.find("name");
        const auto address = entry.find("address");
        const auto port = entry.find("port");
        if (name == entry.end() || !name->is_string() ||         //
            address == entry.end() || !address->is_string() ||   //
            port == entry.end() || !port->is_number_integer()) {
            continue;
        }
        const auto portValue = port->get<std::int64_t>();
        if (portValue <= 0 || portValue > 0xFFFF) {
            continue;
        }
        ServerAd ad;
        ad.name = name->get<std::string>();
        ad.address = address->get<std::string>();
        ad.port = static_cast<std::uint16_t>(portValue);
        const auto players = entry.find("players");
        const auto maxPlayers = entry.find("maxPlayers");
        if (players != entry.end() && players->is_number_integer()) {
            ad.players = players->get<int>();
        }
        if (maxPlayers != entry.end() && maxPlayers->is_number_integer()) {
            ad.maxPlayers = maxPlayers->get<int>();
        }
        out.push_back(std::move(ad));
    }
    return out;
}

} // namespace meat
