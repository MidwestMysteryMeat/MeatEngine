#pragma once
#include "engine/net/ByteStream.h"
#include "engine/net/Transport.h"
#include "engine/platform/Input.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Wire messages. Every packet is [u8 MsgType][payload]. encode() writes the
// payload only; pack() adds the type byte. decode() expects a reader positioned
// at the payload (i.e. past the type byte — see unpack()). decode() returning
// false means the packet was malformed or truncated; the out-param may be
// partially written and must be discarded.
namespace meat {

enum class MsgType : std::uint8_t {
    Hello = 1, Welcome, Command, Snapshot, VoxelOp, Inventory, BatchVoxelOp, DeltaSnapshot,
    PlaceProp, PropAdded, RemoveProp, MoveProp
};

struct HelloMsg {
    std::string name;
};

struct WelcomeMsg {
    PeerId playerId = 0;
    std::uint32_t worldSeed = 0;
    std::uint64_t serverTick = 0;
    // Game rules as opaque bytes — engine/net stays game-agnostic; the game
    // layer (GameRules) owns the meaning. rulesFlags packs inventory economy
    // bits + terrain (see GameRules::flagsByte). voxelSize + environment ride
    // as their own fields so a join client can rebuild the host's world scale
    // and gravity/fog without free flag bits.
    std::uint8_t rulesModel = 0;
    std::uint8_t rulesFlags = 0;
    float voxelSize = 0.5f;
    std::uint8_t environment = 0; // GameRules::Environment as u8
};

struct CommandMsg {
    PlayerCommand cmd;
    // Piggybacked snapshot ack: the newest snapshot tick the client holds and can
    // use as a delta baseline. 0 = none yet (server sends a keyframe). Rides every
    // command (60 Hz) so the server's per-client baseline stays fresh for free.
    std::uint64_t ackSnapshotTick = 0;
};

struct PlayerState {
    PeerId playerId = 0;
    glm::vec3 pos{0};
    glm::vec3 vel{0};
    float yaw = 0;
    float pitch = 0;
    bool onGround = false;
    bool crouched = false;
    float health = 100.0f;
    // H4: entity id of the ship this player is piloting (0 = on foot). Client
    // uses this to attach the camera and skip capsule prediction.
    std::uint32_t vehicleId = 0;
};

// Non-player world entities (pickups, projectiles, NPCs, turrets). archetype and
// data are opaque to the net layer — the game layer owns their meaning.
struct EntityState {
    std::uint32_t id = 0;
    std::uint8_t archetype = 0;
    glm::vec3 pos{0};
    float yaw = 0;
    std::uint8_t anim = 0;
    float health = 0;
    std::uint16_t data = 0;
};

struct SnapshotMsg {
    std::uint64_t tick = 0;
    std::uint64_t lastCmdTick = 0;
    std::vector<PlayerState> players;
    std::vector<EntityState> entities;
};

struct VoxelOpMsg {
    glm::ivec3 voxel{0};
    std::uint16_t block = 0;
};

// Editor intent: place a mesh prop as a server-authoritative world object.
// Client→server. The server assigns an id, sizes a static box collider, persists
// it in the world save, and echoes PropAddedMsg to everyone (including the sender).
struct PlacePropMsg {
    std::string asset;         // project-relative model path (e.g. "assets/models/prop_crate.obj")
    glm::mat4 transform{1.0f}; // world TRS
};

// Server→client: a world prop now exists — from a live place, the join replay
// (sendOverlayTo), or a save reload. The client renders it and builds a matching
// client-mirror box collider so prediction collides like the server.
struct PropAddedMsg {
    std::uint32_t id = 0;
    std::string asset;
    glm::mat4 transform{1.0f};
};

// Bidirectional prop removal: client→server is editor intent (outliner Delete);
// server→client is the authoritative echo (and join-side teardown). Same payload.
struct RemovePropMsg {
    std::uint32_t id = 0;
};

// Client→server: editor gizmo moved/rotated/scaled a synced prop. Server updates
// the collider + save state and rebroadcasts PropAdded with the new transform so
// every peer (including the editor) converges. Id must already exist.
struct MovePropMsg {
    std::uint32_t id = 0;
    glm::mat4 transform{1.0f};
};

// Snapshots larger than this are rejected on decode (and clamped on encode) so
// a hostile packet can never make us allocate an absurd player list.
inline constexpr std::size_t kMaxSnapshotPlayers = 64;
inline constexpr std::size_t kMaxSnapshotEntities = 256;

// PlayerCommand travels inside CommandMsg but is exposed for tests/tools.
// Wire layout: tick u64, move vec2, yaw f32, pitch f32, 6 buttons in one u8.
void encode(const PlayerCommand& cmd, ByteWriter& w);
bool decode(PlayerCommand& cmd, ByteReader& r);

void encode(const HelloMsg& msg, ByteWriter& w);
bool decode(HelloMsg& msg, ByteReader& r);
void encode(const WelcomeMsg& msg, ByteWriter& w);
bool decode(WelcomeMsg& msg, ByteReader& r);
void encode(const CommandMsg& msg, ByteWriter& w);
bool decode(CommandMsg& msg, ByteReader& r);
void encode(const PlayerState& state, ByteWriter& w);
bool decode(PlayerState& state, ByteReader& r);
void encode(const EntityState& e, ByteWriter& w);
bool decode(EntityState& e, ByteReader& r);
void encode(const SnapshotMsg& msg, ByteWriter& w);
bool decode(SnapshotMsg& msg, ByteReader& r);
void encode(const VoxelOpMsg& msg, ByteWriter& w);
bool decode(VoxelOpMsg& msg, ByteReader& r);
void encode(const PlacePropMsg& msg, ByteWriter& w);
bool decode(PlacePropMsg& msg, ByteReader& r);
void encode(const PropAddedMsg& msg, ByteWriter& w);
bool decode(PropAddedMsg& msg, ByteReader& r);
void encode(const RemovePropMsg& msg, ByteWriter& w);
bool decode(RemovePropMsg& msg, ByteReader& r);
void encode(const MovePropMsg& msg, ByteWriter& w);
bool decode(MovePropMsg& msg, ByteReader& r);

constexpr MsgType msgTypeOf(const HelloMsg&) { return MsgType::Hello; }
constexpr MsgType msgTypeOf(const WelcomeMsg&) { return MsgType::Welcome; }
constexpr MsgType msgTypeOf(const CommandMsg&) { return MsgType::Command; }
constexpr MsgType msgTypeOf(const SnapshotMsg&) { return MsgType::Snapshot; }
constexpr MsgType msgTypeOf(const VoxelOpMsg&) { return MsgType::VoxelOp; }
constexpr MsgType msgTypeOf(const PlacePropMsg&) { return MsgType::PlaceProp; }
constexpr MsgType msgTypeOf(const PropAddedMsg&) { return MsgType::PropAdded; }
constexpr MsgType msgTypeOf(const RemovePropMsg&) { return MsgType::RemoveProp; }
constexpr MsgType msgTypeOf(const MovePropMsg&) { return MsgType::MoveProp; }

// nullopt if the packet is empty or the type byte is not a known MsgType.
std::optional<MsgType> peekType(std::span<const std::byte> packet);

// [u8 type][payload] ready for Transport::send.
template <typename Msg>
std::vector<std::byte> pack(const Msg& msg) {
    ByteWriter w;
    w.write(static_cast<std::uint8_t>(msgTypeOf(msg)));
    encode(msg, w);
    return std::move(w).take();
}

// Full-packet decode: verifies the type byte matches Msg, then decodes the
// payload. False on type mismatch or malformed payload.
template <typename Msg>
bool unpack(Msg& out, std::span<const std::byte> packet) {
    const std::optional<MsgType> type = peekType(packet);
    if (!type || *type != msgTypeOf(out)) {
        return false;
    }
    ByteReader r(packet.subspan(1));
    return decode(out, r);
}

} // namespace meat
