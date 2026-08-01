#include "engine/net/Messages.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>

namespace meat {

namespace {

// 16 floats, column-major (glm's storage order) — the whole world TRS on the wire.
void writeMat4(ByteWriter& w, const glm::mat4& m) {
    const float* p = glm::value_ptr(m);
    for (int i = 0; i < 16; ++i) w.write(p[i]);
}
bool readMat4(ByteReader& r, glm::mat4& m) {
    float v[16];
    for (float& f : v)
        if (!r.read(f)) return false;
    m = glm::make_mat4(v);
    return true;
}

// PlayerCommand button bitmask layout — order is part of the wire format.
enum CommandButton : std::uint8_t {
    kBtnJump = 1u << 0,
    kBtnCrouch = 1u << 1,
    kBtnSprint = 1u << 2,
    kBtnFire = 1u << 3,
    kBtnUse = 1u << 4,
    kBtnReload = 1u << 5,
    kBtnPlace = 1u << 6,
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
    if (cmd.place) buttons |= kBtnPlace;
    w.write(buttons);
    w.write(cmd.selectedSlot);
}

bool decode(PlayerCommand& cmd, ByteReader& r) {
    std::uint8_t buttons = 0;
    if (!r.read(cmd.tick) || !r.read(cmd.move) || !r.read(cmd.yaw) || !r.read(cmd.pitch) ||
        !r.read(buttons) || !r.read(cmd.selectedSlot)) {
        return false;
    }
    cmd.jump = (buttons & kBtnJump) != 0;
    cmd.crouch = (buttons & kBtnCrouch) != 0;
    cmd.sprint = (buttons & kBtnSprint) != 0;
    cmd.fire = (buttons & kBtnFire) != 0;
    cmd.use = (buttons & kBtnUse) != 0;
    cmd.reload = (buttons & kBtnReload) != 0;
    cmd.place = (buttons & kBtnPlace) != 0;
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
    w.write(msg.rulesModel);
    w.write(msg.rulesFlags);
    w.write(msg.voxelSize);
    w.write(msg.environment);
}

bool decode(WelcomeMsg& msg, ByteReader& r) {
    return r.read(msg.playerId) && r.read(msg.worldSeed) && r.read(msg.serverTick) &&
           r.read(msg.rulesModel) && r.read(msg.rulesFlags) && r.read(msg.voxelSize) &&
           r.read(msg.environment);
}

void encode(const CommandMsg& msg, ByteWriter& w) {
    encode(msg.cmd, w);
    w.write(msg.ackSnapshotTick);
}

bool decode(CommandMsg& msg, ByteReader& r) {
    return decode(msg.cmd, r) && r.read(msg.ackSnapshotTick);
}

void encode(const PlayerState& state, ByteWriter& w) {
    w.write(state.playerId);
    w.write(state.pos);
    w.write(state.vel);
    w.write(state.yaw);
    w.write(state.pitch);
    w.write(state.onGround);
    w.write(state.crouched);
    w.write(state.health);
    w.write(state.vehicleId);
    w.write(state.vehicleRole);
}

bool decode(PlayerState& state, ByteReader& r) {
    return r.read(state.playerId) && r.read(state.pos) && r.read(state.vel) &&
           r.read(state.yaw) && r.read(state.pitch) && r.read(state.onGround) &&
           r.read(state.crouched) && r.read(state.health) && r.read(state.vehicleId) &&
           r.read(state.vehicleRole);
}

void encode(const EntityState& e, ByteWriter& w) {
    w.write(e.id);
    w.write(e.archetype);
    w.write(e.pos);
    w.write(e.yaw);
    w.write(e.anim);
    w.write(e.health);
    w.write(e.data);
}

bool decode(EntityState& e, ByteReader& r) {
    return r.read(e.id) && r.read(e.archetype) && r.read(e.pos) && r.read(e.yaw) &&
           r.read(e.anim) && r.read(e.health) && r.read(e.data);
}

void encode(const SnapshotMsg& msg, ByteWriter& w) {
    w.write(msg.tick);
    w.write(msg.lastCmdTick);
    const std::size_t count = std::min(msg.players.size(), kMaxSnapshotPlayers);
    w.write(static_cast<std::uint16_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        encode(msg.players[i], w);
    }
    const std::size_t entityCount = std::min(msg.entities.size(), kMaxSnapshotEntities);
    w.write(static_cast<std::uint16_t>(entityCount));
    for (std::size_t i = 0; i < entityCount; ++i) {
        encode(msg.entities[i], w);
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
    std::uint16_t entityCount = 0;
    if (!r.read(entityCount) || entityCount > kMaxSnapshotEntities) {
        return false;
    }
    msg.entities.clear();
    msg.entities.reserve(entityCount);
    for (std::uint16_t i = 0; i < entityCount; ++i) {
        EntityState e;
        if (!decode(e, r)) {
            return false;
        }
        msg.entities.push_back(e);
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

void encode(const PlacePropMsg& msg, ByteWriter& w) {
    w.write(std::string_view{msg.asset});
    writeMat4(w, msg.transform);
}

bool decode(PlacePropMsg& msg, ByteReader& r) {
    return r.read(msg.asset) && readMat4(r, msg.transform);
}

void encode(const PropAddedMsg& msg, ByteWriter& w) {
    w.write(msg.id);
    w.write(std::string_view{msg.asset});
    writeMat4(w, msg.transform);
}

bool decode(PropAddedMsg& msg, ByteReader& r) {
    return r.read(msg.id) && r.read(msg.asset) && readMat4(r, msg.transform);
}

void encode(const RemovePropMsg& msg, ByteWriter& w) {
    w.write(msg.id);
}

bool decode(RemovePropMsg& msg, ByteReader& r) {
    return r.read(msg.id);
}

void encode(const MovePropMsg& msg, ByteWriter& w) {
    w.write(msg.id);
    writeMat4(w, msg.transform);
}

bool decode(MovePropMsg& msg, ByteReader& r) {
    return r.read(msg.id) && readMat4(r, msg.transform);
}

void encode(const ScriptFxMsg& msg, ByteWriter& w) {
    w.write(msg.kind);
    w.write(msg.id);
    w.write(msg.duration);
    w.write(msg.r);
    w.write(msg.g);
    w.write(msg.b);
    // Trailing string: empty for highlight; announce text for kind 1.
    w.write(std::string_view{msg.text});
}

bool decode(ScriptFxMsg& msg, ByteReader& r) {
    if (!r.read(msg.kind) || !r.read(msg.id) || !r.read(msg.duration) || !r.read(msg.r) ||
        !r.read(msg.g) || !r.read(msg.b))
        return false;
    // Older packets without text still decode as highlight if remaining==0 —
    // empty string is fine for kind 0.
    if (r.remaining() == 0) {
        msg.text.clear();
        return true;
    }
    return r.read(msg.text);
}

void encode(const GravityVolumesMsg& msg, ByteWriter& w) {
    const std::size_t n = std::min(msg.volumes.size(), kMaxGravityVolumes);
    w.write(static_cast<std::uint16_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const GravityVolumeEntry& v = msg.volumes[i];
        w.write(v.min);
        w.write(v.max);
        w.write(v.gravity);
        w.write(v.priority);
    }
}

bool decode(GravityVolumesMsg& msg, ByteReader& r) {
    std::uint16_t count = 0;
    if (!r.read(count) || count > kMaxGravityVolumes) return false;
    msg.volumes.clear();
    msg.volumes.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        GravityVolumeEntry v;
        if (!r.read(v.min) || !r.read(v.max) || !r.read(v.gravity) || !r.read(v.priority))
            return false;
        msg.volumes.push_back(v);
    }
    return true;
}

std::optional<MsgType> peekType(std::span<const std::byte> packet) {
    if (packet.empty()) {
        return std::nullopt;
    }
    const auto raw = static_cast<std::uint8_t>(packet.front());
    // GravityVolumes is the last enumerator — new message types must extend this bound
    // or unpack()/peekType reject them as unknown.
    if (raw < static_cast<std::uint8_t>(MsgType::Hello) ||
        raw > static_cast<std::uint8_t>(MsgType::GravityVolumes)) {
        return std::nullopt;
    }
    return static_cast<MsgType>(raw);
}

} // namespace meat
