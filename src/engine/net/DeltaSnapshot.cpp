#include "engine/net/DeltaSnapshot.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace meat {
namespace {

// Positions dominate the snapshot payload and don't need 32-bit precision on the wire — clients
// only interpolate them for rendering; the server stays authoritative. Quantize each component to
// 16 bits over the world bound (±2048 m → ~6 cm steps), halving a vec3 from 12 to 6 bytes. Absolute
// (not delta-of-delta), so there's no cumulative drift. (bitsery is vendored for a future full
// bit-packed codec; this surgical fixed-point pass avoids threading a bit-stream through the
// byte-aligned framing.)
constexpr float kPosMin = -2048.0f, kPosMax = 2048.0f;
std::uint16_t quantize(float v, float lo, float hi) {
    const float t = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
    return static_cast<std::uint16_t>(t * 65535.0f + 0.5f);
}
float dequantize(std::uint16_t q, float lo, float hi) {
    return lo + (static_cast<float>(q) / 65535.0f) * (hi - lo);
}
void writePos(ByteWriter& w, const glm::vec3& p) {
    w.write(quantize(p.x, kPosMin, kPosMax));
    w.write(quantize(p.y, kPosMin, kPosMax));
    w.write(quantize(p.z, kPosMin, kPosMax));
}
bool readPos(ByteReader& r, glm::vec3& p) {
    std::uint16_t x = 0, y = 0, z = 0;
    if (!r.read(x) || !r.read(y) || !r.read(z)) return false;
    p = {dequantize(x, kPosMin, kPosMax), dequantize(y, kPosMin, kPosMax),
         dequantize(z, kPosMin, kPosMax)};
    return true;
}

std::uint8_t playerFlags(const PlayerState& s) {
    return static_cast<std::uint8_t>((s.onGround ? 1u : 0u) | (s.crouched ? 2u : 0u));
}

std::uint8_t diffPlayer(const PlayerState& c, const PlayerState& b) {
    std::uint8_t m = 0;
    if (c.pos != b.pos)       m |= playerfield::Pos;
    if (c.vel != b.vel)       m |= playerfield::Vel;
    if (c.yaw != b.yaw)       m |= playerfield::Yaw;
    if (c.pitch != b.pitch)   m |= playerfield::Pitch;
    if (c.onGround != b.onGround || c.crouched != b.crouched) m |= playerfield::Flags;
    if (c.health != b.health) m |= playerfield::Health;
    if (c.vehicleId != b.vehicleId) m |= playerfield::Vehicle;
    if (c.vehicleRole != b.vehicleRole) m |= playerfield::VehicleRole;
    return m;
}

void writePlayerFields(ByteWriter& w, const PlayerState& s, std::uint8_t m) {
    if (m & playerfield::Pos)    writePos(w, s.pos);
    if (m & playerfield::Vel)    w.write(s.vel);
    if (m & playerfield::Yaw)    w.write(s.yaw);
    if (m & playerfield::Pitch)  w.write(s.pitch);
    if (m & playerfield::Flags)  w.write(playerFlags(s));
    if (m & playerfield::Health) w.write(s.health);
    if (m & playerfield::Vehicle) w.write(s.vehicleId);
    if (m & playerfield::VehicleRole) w.write(s.vehicleRole);
}

bool readPlayerFields(ByteReader& r, PlayerState& s, std::uint8_t m) {
    if ((m & playerfield::Pos)   && !readPos(r, s.pos)) return false;
    if ((m & playerfield::Vel)   && !r.read(s.vel))   return false;
    if ((m & playerfield::Yaw)   && !r.read(s.yaw))   return false;
    if ((m & playerfield::Pitch) && !r.read(s.pitch)) return false;
    if (m & playerfield::Flags) {
        std::uint8_t f = 0;
        if (!r.read(f)) return false;
        s.onGround = (f & 1u) != 0;
        s.crouched = (f & 2u) != 0;
    }
    if ((m & playerfield::Health) && !r.read(s.health)) return false;
    if ((m & playerfield::Vehicle) && !r.read(s.vehicleId)) return false;
    if ((m & playerfield::VehicleRole) && !r.read(s.vehicleRole)) return false;
    return true;
}

std::uint8_t diffEntity(const EntityState& c, const EntityState& b) {
    std::uint8_t m = 0;
    if (c.archetype != b.archetype) m |= entityfield::Archetype;
    if (c.pos != b.pos)             m |= entityfield::Pos;
    if (c.yaw != b.yaw)             m |= entityfield::Yaw;
    if (c.anim != b.anim)           m |= entityfield::Anim;
    if (c.health != b.health)       m |= entityfield::Health;
    if (c.data != b.data)           m |= entityfield::Data;
    return m;
}

void writeEntityFields(ByteWriter& w, const EntityState& e, std::uint8_t m) {
    if (m & entityfield::Archetype) w.write(e.archetype);
    if (m & entityfield::Pos)       writePos(w, e.pos);
    if (m & entityfield::Yaw)       w.write(e.yaw);
    if (m & entityfield::Anim)      w.write(e.anim);
    if (m & entityfield::Health)    w.write(e.health);
    if (m & entityfield::Data)      w.write(e.data);
}

bool readEntityFields(ByteReader& r, EntityState& e, std::uint8_t m) {
    if ((m & entityfield::Archetype) && !r.read(e.archetype)) return false;
    if ((m & entityfield::Pos)       && !readPos(r, e.pos))   return false;
    if ((m & entityfield::Yaw)       && !r.read(e.yaw))       return false;
    if ((m & entityfield::Anim)      && !r.read(e.anim))      return false;
    if ((m & entityfield::Health)    && !r.read(e.health))    return false;
    if ((m & entityfield::Data)      && !r.read(e.data))      return false;
    return true;
}

} // namespace

void encodeDelta(const SnapshotMsg& cur, const SnapshotMsg& baseline, ByteWriter& w) {
    w.write(cur.tick);
    w.write(baseline.tick); // 0 when baseline is the empty keyframe snapshot
    w.write(cur.lastCmdTick);

    // ---------------- players ----------------
    std::unordered_map<PeerId, const PlayerState*> basePlayers;
    for (const PlayerState& p : baseline.players) basePlayers.emplace(p.playerId, &p);

    std::unordered_set<PeerId> curPlayerIds;
    for (const PlayerState& p : cur.players) curPlayerIds.insert(p.playerId);

    // removed = in baseline, absent now
    ByteWriter removedP;
    std::uint16_t nRemovedP = 0;
    for (const PlayerState& p : baseline.players) {
        if (nRemovedP >= kMaxSnapshotPlayers) break;
        if (!curPlayerIds.count(p.playerId)) { removedP.write(p.playerId); ++nRemovedP; }
    }
    w.write(nRemovedP);
    w.writeBytes(removedP.data());

    // changed/new — buffer body first so the count is exact
    ByteWriter changedP;
    std::uint16_t nChangedP = 0;
    for (const PlayerState& p : cur.players) {
        if (nChangedP >= kMaxSnapshotPlayers) break;
        const auto it = basePlayers.find(p.playerId);
        const std::uint8_t mask =
            (it == basePlayers.end()) ? playerfield::All : diffPlayer(p, *it->second);
        if (mask == 0) continue; // unchanged: omit entirely
        changedP.write(p.playerId);
        changedP.write(mask);
        writePlayerFields(changedP, p, mask);
        ++nChangedP;
    }
    w.write(nChangedP);
    w.writeBytes(changedP.data());

    // ---------------- entities ----------------
    std::unordered_map<std::uint32_t, const EntityState*> baseEnt;
    for (const EntityState& e : baseline.entities) baseEnt.emplace(e.id, &e);

    std::unordered_set<std::uint32_t> curEntIds;
    for (const EntityState& e : cur.entities) curEntIds.insert(e.id);

    ByteWriter removedE;
    std::uint16_t nRemovedE = 0;
    for (const EntityState& e : baseline.entities) {
        if (nRemovedE >= kMaxSnapshotEntities) break;
        if (!curEntIds.count(e.id)) { removedE.write(e.id); ++nRemovedE; }
    }
    w.write(nRemovedE);
    w.writeBytes(removedE.data());

    ByteWriter changedE;
    std::uint16_t nChangedE = 0;
    for (const EntityState& e : cur.entities) {
        if (nChangedE >= kMaxSnapshotEntities) break;
        const auto it = baseEnt.find(e.id);
        const std::uint8_t mask =
            (it == baseEnt.end()) ? entityfield::All : diffEntity(e, *it->second);
        if (mask == 0) continue;
        changedE.write(e.id);
        changedE.write(mask);
        writeEntityFields(changedE, e, mask);
        ++nChangedE;
    }
    w.write(nChangedE);
    w.writeBytes(changedE.data());
}

bool decodeDelta(SnapshotMsg& out, const SnapshotMsg& baseline, ByteReader& r) {
    std::uint64_t baselineTick = 0;
    if (!r.read(out.tick) || !r.read(baselineTick) || !r.read(out.lastCmdTick)) return false;
    // The caller is responsible for having selected the right baseline; a mismatch
    // here means the ring/ack bookkeeping is wrong. Reject rather than corrupt.
    if (baselineTick != baseline.tick) return false;

    // ---------------- players ----------------
    std::unordered_map<PeerId, PlayerState> players;
    for (const PlayerState& p : baseline.players) players.emplace(p.playerId, p);

    std::uint16_t nRemovedP = 0;
    if (!r.read(nRemovedP) || nRemovedP > kMaxSnapshotPlayers) return false;
    for (std::uint16_t i = 0; i < nRemovedP; ++i) {
        PeerId id = 0;
        if (!r.read(id)) return false;
        players.erase(id);
    }
    std::uint16_t nChangedP = 0;
    if (!r.read(nChangedP) || nChangedP > kMaxSnapshotPlayers) return false;
    for (std::uint16_t i = 0; i < nChangedP; ++i) {
        PeerId id = 0;
        std::uint8_t mask = 0;
        if (!r.read(id) || !r.read(mask)) return false;
        PlayerState& s = players[id]; // baseline copy, or default-constructed if new
        s.playerId = id;
        if (!readPlayerFields(r, s, mask)) return false;
    }

    // ---------------- entities ----------------
    std::unordered_map<std::uint32_t, EntityState> entities;
    for (const EntityState& e : baseline.entities) entities.emplace(e.id, e);

    std::uint16_t nRemovedE = 0;
    if (!r.read(nRemovedE) || nRemovedE > kMaxSnapshotEntities) return false;
    for (std::uint16_t i = 0; i < nRemovedE; ++i) {
        std::uint32_t id = 0;
        if (!r.read(id)) return false;
        entities.erase(id);
    }
    std::uint16_t nChangedE = 0;
    if (!r.read(nChangedE) || nChangedE > kMaxSnapshotEntities) return false;
    for (std::uint16_t i = 0; i < nChangedE; ++i) {
        std::uint32_t id = 0;
        std::uint8_t mask = 0;
        if (!r.read(id) || !r.read(mask)) return false;
        EntityState& e = entities[id];
        e.id = id;
        if (!readEntityFields(r, e, mask)) return false;
    }

    if (!r.ok()) return false;

    out.players.clear();
    out.players.reserve(players.size());
    for (auto& [id, s] : players) out.players.push_back(s);
    out.entities.clear();
    out.entities.reserve(entities.size());
    for (auto& [id, e] : entities) out.entities.push_back(e);
    return true;
}

std::optional<std::uint64_t> peekDeltaBaseline(ByteReader r) {
    std::uint64_t tick = 0, baselineTick = 0;
    if (!r.read(tick) || !r.read(baselineTick)) return std::nullopt;
    return baselineTick;
}

} // namespace meat
