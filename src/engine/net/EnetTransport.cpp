#include "engine/net/EnetTransport.h"

#include "engine/core/Log.h"

#include <enet/enet.h>

#include <cstdint>
#include <unordered_map>
#include <utility>

namespace meat {

namespace {

constexpr std::size_t kMaxPeers = 8;
constexpr std::size_t kChannelCount = 2;
constexpr std::uint8_t kChannelReliable = 0;   // reliable-ordered
constexpr std::uint8_t kChannelUnreliable = 1; // unreliable-sequenced
constexpr PeerId kServerPeer = 1;              // the client's one remote

// Ref-counted RAII wrapper for enet_initialize/enet_deinitialize, shared by
// server and client transports (each Impl owns one). Plain statics are fine:
// all transports live on the main thread, so no atomics needed.
class EnetRuntime {
public:
    EnetRuntime() {
        if (s_refs++ == 0) {
            s_initialized = enet_initialize() == 0;
            if (!s_initialized) {
                log::error("enet: enet_initialize failed");
            }
        }
    }
    ~EnetRuntime() {
        if (--s_refs == 0 && s_initialized) {
            enet_deinitialize();
            s_initialized = false;
        }
    }
    EnetRuntime(const EnetRuntime&) = delete;
    EnetRuntime& operator=(const EnetRuntime&) = delete;

    bool ok() const { return s_initialized; }

private:
    static inline int s_refs = 0;
    static inline bool s_initialized = false;
};

std::vector<std::byte> copyPacket(const ENetPacket& packet) {
    const auto* first = reinterpret_cast<const std::byte*>(packet.data);
    return std::vector<std::byte>(first, first + packet.dataLength);
}

// flags 0 on the unreliable channel = ENet's unreliable-sequenced delivery:
// stale packets are dropped instead of arriving out of order, which is what
// tick-stamped commands and snapshots want.
ENetPacket* makePacket(std::span<const std::byte> bytes, bool reliable) {
    const enet_uint32 flags = reliable ? static_cast<enet_uint32>(ENET_PACKET_FLAG_RELIABLE) : 0u;
    return enet_packet_create(bytes.data(), bytes.size(), flags);
}

PeerId peerIdOf(const ENetPeer& peer) {
    return static_cast<PeerId>(reinterpret_cast<std::uintptr_t>(peer.data));
}

void tagPeer(ENetPeer& peer, PeerId id) {
    peer.data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
}

} // namespace

// ---------------------------------------------------------------- server ----

struct EnetServerTransport::Impl {
    EnetRuntime runtime;
    ENetHost* host = nullptr;
    std::unordered_map<PeerId, ENetPeer*> peers;
    PeerId nextId = 1; // sequential, never reused — matches EntityId philosophy

    ~Impl() {
        if (host != nullptr) {
            for (auto& entry : peers) {
                enet_peer_disconnect_now(entry.second, 0);
            }
            enet_host_destroy(host);
        }
    }
};

EnetServerTransport::EnetServerTransport() : m_impl(std::make_unique<Impl>()) {}
EnetServerTransport::~EnetServerTransport() = default;

bool EnetServerTransport::listen(std::uint16_t port) {
    Impl& impl = *m_impl;
    if (!impl.runtime.ok()) {
        return false;
    }
    if (impl.host != nullptr) {
        log::warn("enet server: listen called while already listening");
        return false;
    }
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    impl.host = enet_host_create(&address, kMaxPeers, kChannelCount, 0, 0);
    if (impl.host == nullptr) {
        log::error("enet server: enet_host_create failed on port {}", port);
        return false;
    }
    log::info("enet server: listening on port {} ({} peers max)", port, kMaxPeers);
    return true;
}

void EnetServerTransport::poll(std::vector<NetEvent>& out) {
    Impl& impl = *m_impl;
    if (impl.host == nullptr) {
        return;
    }
    ENetEvent event;
    int rc = 0;
    while ((rc = enet_host_service(impl.host, &event, 0)) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            const PeerId id = impl.nextId++;
            tagPeer(*event.peer, id);
            impl.peers.emplace(id, event.peer);
            out.push_back({NetEvent::Type::Connected, id, {}});
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: { // covers graceful hangup and timeout
            const PeerId id = peerIdOf(*event.peer);
            event.peer->data = nullptr;
            if (id == 0) {
                break; // never completed connect bookkeeping — nothing to report
            }
            impl.peers.erase(id);
            out.push_back({NetEvent::Type::Disconnected, id, {}});
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            NetEvent netEvent;
            netEvent.type = NetEvent::Type::Packet;
            netEvent.peer = peerIdOf(*event.peer);
            netEvent.data = copyPacket(*event.packet);
            enet_packet_destroy(event.packet);
            out.push_back(std::move(netEvent));
            break;
        }
        case ENET_EVENT_TYPE_NONE:
            break;
        }
    }
    if (rc < 0) {
        log::error("enet server: enet_host_service failed");
    }
}

void EnetServerTransport::send(PeerId peer, std::span<const std::byte> bytes, bool reliable) {
    Impl& impl = *m_impl;
    const auto it = impl.peers.find(peer);
    if (it == impl.peers.end()) {
        log::warn("enet server: send to unknown peer {}", peer);
        return;
    }
    ENetPacket* packet = makePacket(bytes, reliable);
    if (packet == nullptr) {
        log::error("enet server: enet_packet_create failed ({} bytes)", bytes.size());
        return;
    }
    const auto channel = static_cast<std::uint8_t>(reliable ? kChannelReliable : kChannelUnreliable);
    if (enet_peer_send(it->second, channel, packet) < 0) {
        log::error("enet server: enet_peer_send to peer {} failed", peer);
        enet_packet_destroy(packet); // still ours on failure
    }
}

void EnetServerTransport::disconnect(PeerId peer) {
    Impl& impl = *m_impl;
    const auto it = impl.peers.find(peer);
    if (it == impl.peers.end()) {
        return;
    }
    // Graceful: the Disconnected event arrives via poll() once ENet finishes
    // the handshake (or times out); the peer stays mapped until then.
    enet_peer_disconnect(it->second, 0);
}

// ---------------------------------------------------------------- client ----

struct EnetClientTransport::Impl {
    EnetRuntime runtime;
    ENetHost* host = nullptr;
    ENetPeer* server = nullptr;
    bool connected = false;

    void destroyHost() {
        if (host != nullptr) {
            enet_host_destroy(host);
            host = nullptr;
        }
        server = nullptr;
        connected = false;
    }

    ~Impl() {
        if (server != nullptr && connected) {
            enet_peer_disconnect_now(server, 0);
        }
        destroyHost();
    }
};

EnetClientTransport::EnetClientTransport() : m_impl(std::make_unique<Impl>()) {}
EnetClientTransport::~EnetClientTransport() = default;

bool EnetClientTransport::connect(const std::string& host, std::uint16_t port) {
    Impl& impl = *m_impl;
    if (!impl.runtime.ok()) {
        return false;
    }
    if (impl.host != nullptr) {
        log::warn("enet client: connect called while a connection is active");
        return false;
    }
    impl.host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    if (impl.host == nullptr) {
        log::error("enet client: enet_host_create failed");
        return false;
    }
    ENetAddress address{};
    if (enet_address_set_host(&address, host.c_str()) < 0) { // may block briefly on DNS
        log::error("enet client: cannot resolve host '{}'", host);
        impl.destroyHost();
        return false;
    }
    address.port = port;
    impl.server = enet_host_connect(impl.host, &address, kChannelCount, 0);
    if (impl.server == nullptr) {
        log::error("enet client: enet_host_connect to {}:{} failed", host, port);
        impl.destroyHost();
        return false;
    }
    log::info("enet client: connecting to {}:{}", host, port);
    return true;
}

bool EnetClientTransport::connected() const {
    return m_impl->connected;
}

void EnetClientTransport::poll(std::vector<NetEvent>& out) {
    Impl& impl = *m_impl;
    if (impl.host == nullptr) {
        return;
    }
    bool lost = false;
    ENetEvent event;
    int rc = 0;
    while ((rc = enet_host_service(impl.host, &event, 0)) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            impl.connected = true;
            out.push_back({NetEvent::Type::Connected, kServerPeer, {}});
            break;
        case ENET_EVENT_TYPE_DISCONNECT: // hangup, refusal, or timeout
            lost = true;
            out.push_back({NetEvent::Type::Disconnected, kServerPeer, {}});
            break;
        case ENET_EVENT_TYPE_RECEIVE: {
            NetEvent netEvent;
            netEvent.type = NetEvent::Type::Packet;
            netEvent.peer = kServerPeer;
            netEvent.data = copyPacket(*event.packet);
            enet_packet_destroy(event.packet);
            out.push_back(std::move(netEvent));
            break;
        }
        case ENET_EVENT_TYPE_NONE:
            break;
        }
    }
    if (rc < 0) {
        log::error("enet client: enet_host_service failed");
    }
    if (lost) {
        impl.destroyHost(); // frees the slot so connect() can be called again
    }
}

void EnetClientTransport::send(PeerId peer, std::span<const std::byte> bytes, bool reliable) {
    Impl& impl = *m_impl;
    if (!impl.connected || impl.server == nullptr) {
        log::warn("enet client: send while not connected");
        return;
    }
    if (peer != kServerPeer) {
        log::warn("enet client: send to unknown peer {}", peer);
        return;
    }
    ENetPacket* packet = makePacket(bytes, reliable);
    if (packet == nullptr) {
        log::error("enet client: enet_packet_create failed ({} bytes)", bytes.size());
        return;
    }
    const auto channel = static_cast<std::uint8_t>(reliable ? kChannelReliable : kChannelUnreliable);
    if (enet_peer_send(impl.server, channel, packet) < 0) {
        log::error("enet client: enet_peer_send failed");
        enet_packet_destroy(packet);
    }
}

void EnetClientTransport::disconnect(PeerId peer) {
    Impl& impl = *m_impl;
    if (peer != kServerPeer || impl.server == nullptr) {
        return;
    }
    enet_peer_disconnect(impl.server, 0); // Disconnected surfaces via poll()
}

} // namespace meat
