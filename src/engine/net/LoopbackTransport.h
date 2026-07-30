#pragma once
#include "engine/net/Transport.h"

#include <vector>

// In-process transport pair for single-player (Game mode): ServerSim talks to
// serverEnd(), Client talks to clientEnd(), and packets cross via queues owned
// by the pair. Both ends are connected from construction — each end's first
// poll() yields Connected for peer 1 (the one remote it will ever see).
//
// Single-threaded use only (main thread), like the rest of the sim loop — the
// shared queues are deliberately unlocked. Do not touch either end from workers.
namespace meat {

class LoopbackPair {
public:
    LoopbackPair();
    // Ends hold pointers into the pair, so it must stay put.
    LoopbackPair(const LoopbackPair&) = delete;
    LoopbackPair& operator=(const LoopbackPair&) = delete;

    Transport& serverEnd() { return m_server; }
    Transport& clientEnd() { return m_client; }

private:
    class End final : public Transport {
    public:
        void poll(std::vector<NetEvent>& out) override;
        void send(PeerId peer, std::span<const std::byte> bytes, bool reliable) override;
        void disconnect(PeerId peer) override;

        // Wired by LoopbackPair's constructor.
        std::vector<NetEvent>* m_inbox = nullptr;     // drained by this end's poll()
        std::vector<NetEvent>* m_peerInbox = nullptr; // where this end's send() lands
        bool* m_open = nullptr;                       // shared link state
        const char* m_label = "";                     // "server"/"client" for log messages
    };

    std::vector<NetEvent> m_toServer;
    std::vector<NetEvent> m_toClient;
    bool m_open = true;
    End m_server;
    End m_client;
};

} // namespace meat
