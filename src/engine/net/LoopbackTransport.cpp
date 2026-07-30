#include "engine/net/LoopbackTransport.h"

#include "engine/core/Log.h"

#include <utility>

namespace meat {

namespace {
// Each end sees exactly one remote peer, and it is always id 1.
constexpr PeerId kRemotePeer = 1;
} // namespace

LoopbackPair::LoopbackPair() {
    m_server.m_inbox = &m_toServer;
    m_server.m_peerInbox = &m_toClient;
    m_server.m_open = &m_open;
    m_server.m_label = "server";

    m_client.m_inbox = &m_toClient;
    m_client.m_peerInbox = &m_toServer;
    m_client.m_open = &m_open;
    m_client.m_label = "client";

    // Born connected: the first poll() on each end reports the other side.
    m_toServer.push_back({NetEvent::Type::Connected, kRemotePeer, {}});
    m_toClient.push_back({NetEvent::Type::Connected, kRemotePeer, {}});
}

void LoopbackPair::End::poll(std::vector<NetEvent>& out) {
    for (NetEvent& event : *m_inbox) {
        out.push_back(std::move(event));
    }
    m_inbox->clear();
}

void LoopbackPair::End::send(PeerId peer, std::span<const std::byte> bytes, bool /*reliable*/) {
    // reliable is ignored: loopback never drops, duplicates, or reorders.
    if (!*m_open) {
        return; // link torn down — drop silently, like a socket after hangup
    }
    if (peer != kRemotePeer) {
        log::warn("loopback {}: send to unknown peer {}", m_label, peer);
        return;
    }
    NetEvent event;
    event.type = NetEvent::Type::Packet;
    event.peer = kRemotePeer;
    event.data.assign(bytes.begin(), bytes.end());
    m_peerInbox->push_back(std::move(event));
}

void LoopbackPair::End::disconnect(PeerId peer) {
    if (peer != kRemotePeer || !*m_open) {
        return;
    }
    *m_open = false;
    // Both ends observe the hangup on their next poll(); packets already queued
    // ahead of it still deliver, matching graceful-disconnect ordering.
    m_inbox->push_back({NetEvent::Type::Disconnected, kRemotePeer, {}});
    m_peerInbox->push_back({NetEvent::Type::Disconnected, kRemotePeer, {}});
}

} // namespace meat
