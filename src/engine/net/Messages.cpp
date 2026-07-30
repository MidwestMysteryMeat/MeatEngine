#include "engine/net/Messages.h"

#include <algorithm>

namespace meat {

namespace {

// PlayerCommand button bitmask layout — order is part of the wire format.
enum CommandButton : std::uint8_t {
    kBtnJump = 1u << 0,
    kBtnCrouch = 1u << 1,
    kBtnSprint = 1u << 2,
    kBtnFire = 1u << 3,
    kBtnUse = 1u << 4,
    kBtnReload = 1u << 5,
};

} // namespace

void encode(const PlayerCommand& cmd, ByteWriter& w) {
    w.write(cmd.tick);
    w.write(cmd.move);
    w.write(cmd.yaw);
    w.write(cmd.pitch);
    std::uint8_t buttons = 0;
    if (cmd.jump) buttons |= kBtnJump;
    if (cmd.crouch) buttons |= kBtnCrouch;
    if (cmd.sprint) buttons |= kBtnSprint;
    if (cmd.fire) buttons |= kBtnFire;
    if (cmd.use) buttons |= kBtnUse;
    if (cmd.reload) buttons |= kBtnReload;
    w.write(buttons);
}

bool decode(PlayerCommand& cmd, ByteReader& r) {
    std::uint8_t buttons = 0;
    if (!r.read(cmd.tick) || !r.read(cmd.move) || !r.read(cmd.yaw) || !r.read(cmd.pitch) ||
        !r.read(buttons)) {
        return false;
    }
    cmd.jump = (buttons & kBtnJump) != 0;
    cmd.crouch = (buttons & kBtnCrouch) != 0;
    cmd.sprint = (buttons & kBtnSprint) != 0;
    cmd.fire = (buttons & kBtnFire) != 0;
    cmd.use = (buttons & kBtnUse) != 0;
    cmd.reload = (buttons & kBtnReload) != 0;
    return true;
}

void encode(const HelloMsg& msg, ByteWriter& w) {
    w.write(std::string_view{msg.name});
}

bool decode(HelloMsg& msg, ByteReader& r) {
    return r.read(msg.name);
}

void encode(const WelcomeMsg& msg, ByteWriter& w) {
    w.write(msg.playerId);
    w.write(msg.worldSeed);
    w.write(msg.serverTick);
}

bool decode(WelcomeMsg& msg, ByteReader& r) {
    return r.read(msg.playerId) && r.read(msg.worldSeed) && r.read(msg.serverTick);
}

void encode(const CommandMsg& msg, ByteWriter& w) {
    encode(msg.cmd, w);
}

bool decode(CommandMsg& msg, ByteReader& r) {
    return decode(msg.cmd, r);
}

void encode(const PlayerState& state, ByteWriter& w) {
    w.write(state.playerId);
    w.write(state.pos);
    w.write(state.vel);
    w.write(state.yaw);
    w.write(state.pitch);
    w.write(state.onGround);
    w.write(state.crouched);
}

bool decode(PlayerState& state, ByteReader& r) {
    return r.read(state.playerId) && r.read(state.pos) && r.read(state.vel) &&
           r.read(state.yaw) && r.read(state.pitch) && r.read(state.onGround) &&
           r.read(state.crouched);
}

void encode(const SnapshotMsg& msg, ByteWriter& w) {
    w.write(msg.tick);
    w.write(msg.lastCmdTick);
    const std::size_t count = std::min(msg.players.size(), kMaxSnapshotPlayers);
    w.write(static_cast<std::uint16_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        encode(msg.players[i], w);
    }
}

bool decode(SnapshotMsg& msg, ByteReader& r) {
    std::uint16_t count = 0;
    if (!r.read(msg.tick) || !r.read(msg.lastCmdTick) || !r.read(count)) {
        return false;
    }
    if (count > kMaxSnapshotPlayers) {
        return false; // hostile or corrupt — reject rather than allocate
    }
    msg.players.clear();
    msg.players.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        PlayerState state;
        if (!decode(state, r)) {
            return false;
        }
        msg.players.push_back(state);
    }
    return true;
}

void encode(const VoxelOpMsg& msg, ByteWriter& w) {
    w.write(msg.voxel);
    w.write(msg.block);
}

bool decode(VoxelOpMsg& msg, ByteReader& r) {
    return r.read(msg.voxel) && r.read(msg.block);
}

std::optional<MsgType> peekType(std::span<const std::byte> packet) {
    if (packet.empty()) {
        return std::nullopt;
    }
    const auto raw = static_cast<std::uint8_t>(packet.front());
    if (raw < static_cast<std::uint8_t>(MsgType::Hello) ||
        raw > static_cast<std::uint8_t>(MsgType::VoxelOp)) {
        return std::nullopt;
    }
    return static_cast<MsgType>(raw);
}

} // namespace meat
