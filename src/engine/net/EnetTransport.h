#pragma once
#include "engine/net/Transport.h"

#include <cstdint>
#include <memory>
#include <string>

// ENet UDP transports. <enet/enet.h> is confined to the .cpp via pimpl so the
// C API (and its winsock includes) never leaks into engine headers. Channel
// scheme for both classes: 0 = reliable-ordered, 1 = unreliable-sequenced.
// enet_initialize/enet_deinitialize are managed by a ref-counted RAII helper
// inside the .cpp — constructing any transport keeps the library alive.
// Main-thread use only, same as the rest of the net layer.
namespace meat {

class EnetServerTransport final : public Transport {
public:
    EnetServerTransport();
    ~EnetServerTransport() override;
    EnetServerTransport(const EnetServerTransport&) = delete;
    EnetServerTransport& operator=(const EnetServerTransport&) = delete;

    // Binds and listens on all interfaces; max 8 peers. False on failure (logged).
    bool listen(std::uint16_t port);

    void poll(std::vector<NetEvent>& out) override;
    void send(PeerId peer, std::span<const std::byte> bytes, bool reliable) override;
    void disconnect(PeerId peer) override; // graceful; Disconnected arrives via poll()

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class EnetClientTransport final : public Transport {
public:
    EnetClientTransport();
    ~EnetClientTransport() override;
    EnetClientTransport(const EnetClientTransport&) = delete;
    EnetClientTransport& operator=(const EnetClientTransport&) = delete;

    // Starts the handshake and returns immediately (DNS resolution may block
    // briefly). The Connected event surfaces via poll(); until then connected()
    // is false and send() drops. The server is always PeerId 1.
    bool connect(const std::string& host, std::uint16_t port);
    bool connected() const;

    void poll(std::vector<NetEvent>& out) override;
    void send(PeerId peer, std::span<const std::byte> bytes, bool reliable) override;
    void disconnect(PeerId peer) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace meat
