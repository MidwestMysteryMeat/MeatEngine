#include "engine/net/EncryptedTransport.h"

#include <monocypher.h>

#include <cstring>
#include <random>

namespace meat {

EncryptedTransport::EncryptedTransport(Transport& inner, const std::string& password)
    : m_inner(inner) {
    // key = BLAKE2b(password). A shared server password, not a per-user secret;
    // a hardened deployment would layer a slow salted KDF (Argon2) + X25519 key
    // exchange on top, which is the connect-token follow-up.
    crypto_blake2b(m_key.data(), m_key.size(),
                   reinterpret_cast<const std::uint8_t*>(password.data()), password.size());
    // Random 16-byte nonce prefix; the low 8 bytes are a per-packet counter. The
    // nonce need not be secret, only unique per (key, message) — the prefix keeps
    // this transport's stream disjoint from every other transport sharing the key.
    std::random_device rd;
    for (int i = 0; i < 16; ++i) m_nonce[i] = static_cast<std::uint8_t>(rd());
}

void EncryptedTransport::send(PeerId peer, std::span<const std::byte> bytes, bool reliable) {
    // Advance the 64-bit counter (bytes 16..23) so every packet gets a fresh nonce.
    for (int i = 16; i < 24; ++i)
        if (++m_nonce[i] != 0) break; // carry only on wrap

    const std::size_t n = bytes.size();
    std::vector<std::byte> wire(24 + 16 + n);
    std::memcpy(wire.data(), m_nonce.data(), 24);
    crypto_aead_lock(reinterpret_cast<std::uint8_t*>(wire.data()) + 24 + 16, // cipher out
                     reinterpret_cast<std::uint8_t*>(wire.data()) + 24,      // mac out
                     m_key.data(), m_nonce.data(), nullptr, 0,
                     reinterpret_cast<const std::uint8_t*>(bytes.data()), n);
    m_inner.send(peer, wire, reliable);
}

void EncryptedTransport::poll(std::vector<NetEvent>& out) {
    std::vector<NetEvent> raw;
    m_inner.poll(raw);
    for (NetEvent& e : raw) {
        if (e.type != NetEvent::Type::Packet) {
            out.push_back(std::move(e)); // Connected/Disconnected pass through
            continue;
        }
        if (e.data.size() < kOverhead) continue; // too short to hold nonce+mac — drop
        const std::size_t n = e.data.size() - kOverhead;
        std::vector<std::byte> plain(n);
        const int ok = crypto_aead_unlock(
            reinterpret_cast<std::uint8_t*>(plain.data()),
            reinterpret_cast<const std::uint8_t*>(e.data.data()) + 24, // mac
            m_key.data(),
            reinterpret_cast<const std::uint8_t*>(e.data.data()),      // nonce
            nullptr, 0,
            reinterpret_cast<const std::uint8_t*>(e.data.data()) + 24 + 16, n); // cipher
        if (ok != 0) continue; // wrong key / tampered / forged — silently drop
        NetEvent d;
        d.type = NetEvent::Type::Packet;
        d.peer = e.peer;
        d.data = std::move(plain);
        out.push_back(std::move(d));
    }
}

void EncryptedTransport::disconnect(PeerId peer) { m_inner.disconnect(peer); }

} // namespace meat
