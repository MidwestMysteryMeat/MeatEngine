#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Transport interface — carries opaque byte packets between peers. No game
// knowledge; nothing above this layer knows which implementation is in use.
namespace meat {

using PeerId = std::uint32_t; // assigned by transport; 0 = invalid

struct NetEvent {
    enum class Type { Connected, Disconnected, Packet };
    Type type = Type::Connected;
    PeerId peer = 0;
    std::vector<std::byte> data; // only for Packet
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual void poll(std::vector<NetEvent>& out) = 0; // drain pending events
    virtual void send(PeerId peer, std::span<const std::byte> bytes, bool reliable) = 0;
    virtual void disconnect(PeerId peer) = 0;
};

} // namespace meat
