#pragma once
#include <cstdint>
#include <string>

// What a connected peer is allowed to do to the authoritative world.
//
// This is deliberately separate from player identity. A peer is a network
// participant; its rights are a property of how it authenticated, not of which
// character it drives. Before this existed, any peer that could construct a
// VoxelOp or PlaceProp packet could mutate the world, because the server's edit
// handlers had no notion of who was allowed to send one.
//
// The default is Player, and it must stay the default: a peer that arrives
// without proving anything gets the fewest rights, not the most.
namespace meat {

enum class PeerRole : std::uint8_t {
    Player,  // ordinary client: moves, shoots, picks things up. No world authoring.
    Editor,  // may author the world, but is not the session owner
    Host,    // the session owner's own client
    Admin    // reserved: Host rights plus future moderation
};

struct PeerPermissions {
    PeerRole role = PeerRole::Player;

    bool canEditVoxels() const {
        return role == PeerRole::Editor || role == PeerRole::Host ||
               role == PeerRole::Admin;
    }
    bool canEditProps() const { return canEditVoxels(); }
    // Save/load, script execution and other world-authoring messages land here
    // as they gain network paths, so the answer is in one place.
    bool canAuthorWorld() const { return canEditVoxels(); }
};

inline const char* toString(PeerRole role) {
    switch (role) {
    case PeerRole::Player: return "player";
    case PeerRole::Editor: return "editor";
    case PeerRole::Host:   return "host";
    case PeerRole::Admin:  return "admin";
    }
    return "player";
}

// Server-side networking policy. Both fields are refusals by default: a server
// that is misconfigured, or built from a stale config struct, denies editing
// rather than granting it.
struct NetPolicy {
    // Generated per boot and handed to the owner's own client in-process. A peer
    // that presents it in Hello is the session owner. Empty disables the grant
    // entirely, which is what a packaged game should ship with.
    std::string editorToken;

    // Whether an editor token is honoured at all. The owner's own client sets
    // this for its session; a dedicated server leaves it false so that even a
    // leaked token cannot author the world.
    bool allowRemoteEditing = false;

    // Connection auth: if non-empty, a joining peer must present this exact
    // password in Hello or it is disconnected. Empty = open server (anyone may
    // join), the historical default. This gates ACCESS; it is not wire secrecy —
    // an encrypted transport (GNS) is the follow-up for confidentiality.
    std::string serverPassword;

    // Peers may not author the world faster than a human plausibly could. See
    // RateLimiter in ServerSim.h for the buckets these feed.
    float voxelEditsPerSecond = 30.0f;
    float propEditsPerSecond = 10.0f;
};

} // namespace meat
