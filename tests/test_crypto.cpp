// EncryptedTransport: authenticated encryption over the wire. Verifies the
// payload is actually encrypted, round-trips under the right password, and that
// a wrong key or a tampered packet is rejected (dropped) rather than delivered.

#include "Harness.h"

#include "engine/net/EncryptedTransport.h"
#include "engine/net/Transport.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using meattest::check;

// A stand-in inner transport: records what was sent, replays what we inject.
struct MockInner final : meat::Transport {
    std::vector<std::vector<std::byte>> sent;
    std::vector<meat::NetEvent> incoming;
    void poll(std::vector<meat::NetEvent>& out) override {
        out.insert(out.end(), incoming.begin(), incoming.end());
        incoming.clear();
    }
    void send(meat::PeerId, std::span<const std::byte> b, bool) override {
        sent.emplace_back(b.begin(), b.end());
    }
    void disconnect(meat::PeerId) override {}
};

std::vector<std::byte> bytesOf(const std::string& s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}

void testRoundTrip() {
    std::printf("a payload encrypts and round-trips under the right password\n");
    MockInner senderInner, receiverInner;
    meat::EncryptedTransport sender(senderInner, "hunter2");
    meat::EncryptedTransport receiver(receiverInner, "hunter2");

    const std::vector<std::byte> plain = bytesOf("secret voxel op payload");
    sender.send(1, plain, true);
    if (senderInner.sent.size() != 1) { check(false, "one packet was sent"); return; }
    const std::vector<std::byte>& wire = senderInner.sent[0];
    check(wire.size() == plain.size() + meat::EncryptedTransport::kOverhead,
          "the wire adds exactly the nonce+mac overhead");
    check(std::memcmp(wire.data() + 40, plain.data(), plain.size()) != 0,
          "the payload on the wire is ciphertext, not plaintext");

    receiverInner.incoming.push_back({meat::NetEvent::Type::Packet, 1, wire});
    std::vector<meat::NetEvent> got;
    receiver.poll(got);
    check(got.size() == 1 && got[0].data == plain,
          "the same password decrypts back to the exact plaintext");
}

void testWrongPasswordDrops() {
    std::printf("a wrong password cannot decrypt and the packet is dropped\n");
    MockInner si, ri;
    meat::EncryptedTransport sender(si, "correct-horse");
    meat::EncryptedTransport wrong(ri, "incorrect-horse");
    sender.send(1, bytesOf("hello"), true);
    ri.incoming.push_back({meat::NetEvent::Type::Packet, 1, si.sent[0]});
    std::vector<meat::NetEvent> got;
    wrong.poll(got);
    check(got.empty(), "the packet is dropped, not delivered as garbage");
}

void testTamperDrops() {
    std::printf("a tampered packet fails the MAC and is dropped\n");
    MockInner si, ri;
    meat::EncryptedTransport sender(si, "pw");
    meat::EncryptedTransport receiver(ri, "pw");
    sender.send(1, bytesOf("integrity"), true);
    std::vector<std::byte> wire = si.sent[0];
    wire.back() ^= std::byte{0xFF}; // flip a ciphertext bit
    ri.incoming.push_back({meat::NetEvent::Type::Packet, 1, wire});
    std::vector<meat::NetEvent> got;
    receiver.poll(got);
    check(got.empty(), "tampering is detected and the packet dropped");
}

void testControlEventsPassThrough() {
    std::printf("connect/disconnect events pass through untouched\n");
    MockInner ri;
    meat::EncryptedTransport t(ri, "pw");
    ri.incoming.push_back({meat::NetEvent::Type::Connected, 3, {}});
    ri.incoming.push_back({meat::NetEvent::Type::Disconnected, 3, {}});
    std::vector<meat::NetEvent> got;
    t.poll(got);
    check(got.size() == 2 && got[0].type == meat::NetEvent::Type::Connected &&
              got[1].type == meat::NetEvent::Type::Disconnected,
          "control events are delivered as-is");
}

} // namespace

namespace meattest {

void runCrypto() {
    testRoundTrip();
    testWrongPasswordDrops();
    testTamperDrops();
    testControlEventsPassThrough();
}

} // namespace meattest
