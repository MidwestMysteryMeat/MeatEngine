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

enum class MsgType : std::uint8_t { Hello = 1, Welcome, Command, Snapshot, VoxelOp };

struct HelloMsg {
    std::string name;
};

struct WelcomeMsg {
    PeerId playerId = 0;
    std::uint32_t worldSeed = 0;
    std::uint64_t serverTick = 0;
};

struct CommandMsg {
    PlayerCommand cmd;
};

struct PlayerState {
    PeerId playerId = 0;
    glm::vec3 pos{0};
    glm::vec3 vel{0};
    float yaw = 0;
    float pitch = 0;
    bool onGround = false;
    bool crouched = false;
};

struct SnapshotMsg {
    std::uint64_t tick = 0;
    std::uint64_t lastCmdTick = 0;
    std::vector<PlayerState> players;
};

struct VoxelOpMsg {
    glm::ivec3 voxel{0};
    std::uint16_t block = 0;
};

// Snapshots larger than this are rejected on decode (and clamped on encode) so
// a hostile packet can never make us allocate an absurd player list.
inline constexpr std::size_t kMaxSnapshotPlayers = 64;

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
void encode(const SnapshotMsg& msg, ByteWriter& w);
bool decode(SnapshotMsg& msg, ByteReader& r);
void encode(const VoxelOpMsg& msg, ByteWriter& w);
bool decode(VoxelOpMsg& msg, ByteReader& r);

constexpr MsgType msgTypeOf(const HelloMsg&) { return MsgType::Hello; }
constexpr MsgType msgTypeOf(const WelcomeMsg&) { return MsgType::Welcome; }
constexpr MsgType msgTypeOf(const CommandMsg&) { return MsgType::Command; }
constexpr MsgType msgTypeOf(const SnapshotMsg&) { return MsgType::Snapshot; }
constexpr MsgType msgTypeOf(const VoxelOpMsg&) { return MsgType::VoxelOp; }

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
