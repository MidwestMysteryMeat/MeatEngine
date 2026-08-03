#pragma once
#include "engine/net/Transport.h"

#include <array>
#include <cstdint>
#include <string>

namespace meat {

// Authenticated-encryption decorator over any Transport. Each payload is sealed
// with XChaCha20-Poly1305 (Monocypher) under a key derived from a shared
// password, so the wire carries only nonce || mac || ciphertext: positions and
// commands are confidential, and tampering or a wrong key is detected — a bad
// MAC drops the packet. A peer without the password can neither read nor forge
// traffic. This is the encryption layer above the plaintext-password access gate.
//
// Keying is pre-shared: key = BLAKE2b(password). There is no forward secrecy or
// key exchange yet — the X25519 connect-token handshake is the follow-up.
// Connected/Disconnected events pass through untouched.
class EncryptedTransport final : public Transport {
public:
    EncryptedTransport(Transport& inner, const std::string& password);

    void poll(std::vector<NetEvent>& out) override;
    void send(PeerId peer, std::span<const std::byte> bytes, bool reliable) override;
    void disconnect(PeerId peer) override;

    // Wire overhead added to every payload (24-byte nonce + 16-byte MAC).
    static constexpr std::size_t kOverhead = 24 + 16;

private:
    Transport& m_inner;
    std::array<std::uint8_t, 32> m_key{};
    // 24-byte nonce = 16-byte random per-instance prefix + 8-byte packet counter,
    // so two transports sharing the key (server + each client) never collide.
    std::array<std::uint8_t, 24> m_nonce{};
};

} // namespace meat
