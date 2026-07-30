# MeatEngine — Delta-Compressed Snapshots (design + drop-in code)

**Date:** 2026-07-30
**Author:** netcode harvest pass (research + design only — no engine source was edited)
**Provenance:** technique studied from **Cafu Engine** `Libs/Network/State.{hpp,cpp}` (MIT).
**Fills:** `ENGINE_REUSE_SURVEY.md` shortlist **#1 — snapshot delta compression**.

MeatEngine today sends the **full** player + entity list in every `SnapshotMsg`, every 3rd
sim tick (60 Hz sim → 20 Hz snapshots; `ServerSim.cpp:17`, `:736`, `:1009-1050`). Bandwidth grows
with `O(players + entities)` per snapshot regardless of how little changed. This document specifies a
**per-client, baseline-relative delta** encoding that transmits only changed entities and, within
each, only changed fields — reusing the existing `ByteStream` and leaving the in-memory `SnapshotMsg`,
prediction, and rewind/reconciliation paths untouched.

---

## 1. License verification (done before any study)

Cloned the GitHub mirror `github.com/DNS/Cafu` (canonical: `bitbucket.org/cafu/cafu`) into scratch,
then read `LICENSE.txt` directly rather than trusting GitHub's auto-detector. It is a **verbatim MIT
grant**. Header quoted exactly:

```
Copyright (c) Carsten Fuchs and other contributors.

This software consists of voluntary contributions made by many individuals.
For exact contribution history, see the revision history available at
https://bitbucket.org/cafu/cafu

The following license applies to all parts of this software except as
documented below:

====

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, ...
```

Both `Libs/Network/State.hpp` and `State.cpp` additionally carry a per-file banner:
`Cafu Engine, http://www.cafu.de/ ... This project is licensed under the terms of the MIT license.`
(The `ExtLibs/` carve-out at the bottom of `LICENSE.txt` does **not** touch `Libs/Network/`.)

**Verdict:** MIT, copy-eligible for MeatEngine (Apache-2.0). This design is a **clean reimplementation
of the technique**, not a source copy (see §7 for why, and the attribution we carry anyway).

---

## 2. What Cafu actually does (studied, cited)

Cafu's `cf::Network::StateT` (`State.hpp:46-76`) holds one entity's serialized state as an **opaque
byte array** `m_Data`. Serialization is via `OutStreamT`/`InStreamT` (big-endian `htonl`/`htons`,
`State.hpp:81-375`). The delta lives in two functions:

- **`StateT::GetDeltaMessage(Other, Compress)`** (`State.cpp:146-208`): XORs the current bytes against
  the baseline bytes — `DeltaData[i] = m_Data[i] ^ Other.m_Data[i]` (`State.cpp:153-154`). **Unchanged
  fields become runs of zero bytes.** It then optionally RLE-compresses (`PackBits`, `State.cpp:37-66`)
  and prepends a 1-byte compression flag (`State.cpp:161` / `:173`).
- **`StateT::StateT(Other, DeltaMessage)`** (`State.cpp:125-143`): un-RLE if flagged, then XOR against
  the baseline again — XOR is its own inverse, so `bytes ^ baseline` reconstructs the current state.
- **`IsDeltaMessageEmpty`** (`State.cpp:211-245`): "did anything change" without decoding.

The RLE header byte `N` (`State.cpp:15-19`): `N∈[0,63]` = copy next `N+1` verbatim; `N∈[64,255]` =
repeat the next byte `N-61` times — tuned to crush the zero-runs the XOR produces.

**Key architectural takeaways we keep:** (a) diff against a **baseline** state; (b) transmit **only the
difference**; (c) a **keyframe** path when there is no baseline (in Cafu, `Other` is an empty `StateT`,
so the XOR is against zero and the whole state ships). **What we deliberately change:** Cafu's
byte-level XOR+RLE requires a fixed serialized layout and a scratch buffer per entity, and is opaque to
field structure. MeatEngine already has a *typed* `ByteWriter` and small, fixed field sets, so a
**per-field changed-bitmask** is simpler, allocation-light, and needs no RLE pass — the same net win
(only changes on the wire) with less machinery. We cite Cafu for the technique; the mechanism is
MeatEngine-native.

---

## 3. MeatEngine-native design

### 3.1 Principle

The in-memory representation stays **`SnapshotMsg`** (unchanged: `Messages.h:67-72`). We add a second
*wire encoding* for it:

- **Full/keyframe encode** = today's behaviour, but each record gains a 1-byte field mask (all bits
  set). Used when a client has no usable baseline.
- **Delta encode** = diff `cur` against a **baseline `SnapshotMsg`** (a past snapshot the client has
  confirmed it holds). Emits per set: a list of **removed** ids, and for each **changed/new** entity an
  `id + u8 fieldMask + only the changed fields`. Entities with an empty mask are omitted entirely.

Both directions run through one uniform reader: *look up the id in the baseline (copy if present, else
default-construct), then overlay the masked fields.* A "new" entity is just a record whose id is absent
from the baseline and whose mask has every bit set — no separate spawn opcode needed.

### 3.2 Why this composes with the existing client cleanly

`Client::applySnapshot` (`Client.cpp:100-126`) already consumes a **full** `SnapshotMsg` and does
rewind-and-replay. We keep that untouched. The only new step on the client is: **reconstruct** the full
`SnapshotMsg` from `baseline + delta` *before* calling `applySnapshot`. Prediction, the `m_unacked`
replay (`Client.cpp:125`), remote interpolation (`Client.cpp:128-164`), and the "absent player =
disconnected" prune (`Client.cpp:113-116`) all keep working, because the reconstructed snapshot is a
complete authoritative state exactly as before.

### 3.3 Baselines and the ring (shared, because there is no interest management yet)

With no per-client interest filtering (that's the follow-up, §8), **every client sees the same
authoritative entity set**. So the server keeps **one ring of its last N emitted snapshots**
(`tick → SnapshotMsg`) and, per client, just the **last snapshot tick that client acked**. To build a
client's packet: `baseline = ring[client.ackedTick]` (or the empty snapshot if the client has acked
nothing / its ack aged out of the ring → keyframe). The client keeps a **symmetric ring** of the
snapshots it has *reconstructed*, keyed by tick, and acks the newest tick it holds.

`N = 32` on both sides (≈1.6 s at 20 Hz — matches the existing remote-history depth,
`Client.cpp:110`). Self-healing: if a client's acked tick has fallen out of the server ring, the server
sends a keyframe (`baselineTick = 0`), which the client can always decode against the empty baseline.

### 3.4 Wire format (`MsgType::DeltaSnapshot`)

```
u64  tick            // this snapshot's sequence number (== SnapshotMsg::tick)
u64  baselineTick    // tick this delta is relative to; 0 = keyframe (empty baseline)
u64  lastCmdTick     // per-recipient ack of THEIR input (unchanged from SnapshotMsg)

// --- players (keyed by playerId) ---
u16  numRemovedPlayers
     u32 playerId          × numRemovedPlayers      // in baseline, gone now
u16  numChangedPlayers
     { u32 playerId; u8 mask; <changed fields, in bit order> } × numChangedPlayers

// --- entities (keyed by id) ---
u16  numRemovedEntities
     u32 id                × numRemovedEntities
u16  numChangedEntities
     { u32 id; u8 mask; <changed fields, in bit order> } × numChangedEntities
```

**Player field bits** (`PlayerState`, `Messages.h:44-53`; `playerId` is the key, not in the mask):

| bit | field  | payload            |
|-----|--------|--------------------|
| 0   | Pos    | vec3 (12 B)        |
| 1   | Vel    | vec3 (12 B)        |
| 2   | Yaw    | f32 (4 B)          |
| 3   | Pitch  | f32 (4 B)          |
| 4   | Flags  | u8 (`onGround|crouched<<1`) |
| 5   | Health | f32 (4 B)          |

**Entity field bits** (`EntityState`, `Messages.h:57-65`; `id` is the key):

| bit | field     | payload   |
|-----|-----------|-----------|
| 0   | Archetype | u8        |
| 1   | Pos       | vec3      |
| 2   | Yaw       | f32       |
| 3   | Anim      | u8        |
| 4   | Health    | f32       |
| 5   | Data      | u16       |

Field-bit order and layout are **part of the wire format** — never reorder without a protocol bump.
Float equality is exact bitwise (the server diffs its own current value against its own past value,
both from deterministic sim state — no epsilon needed and none wanted).

---

## 4. Drop-in code (namespace `meat`, uses the existing `ByteStream`)

Two new files under `src/engine/net/`. They depend only on `ByteStream.h` and `Messages.h` — nothing
else in the engine. The in-memory `SnapshotMsg` is unchanged.

### 4.1 `src/engine/net/DeltaSnapshot.h`

```cpp
#pragma once
#include "engine/net/ByteStream.h"
#include "engine/net/Messages.h"

// Baseline-relative delta encoding for SnapshotMsg. The in-memory SnapshotMsg is
// unchanged; this is purely a wire codec. A record whose id is absent from the
// baseline, with every field-bit set, IS the "spawn" case — no separate opcode.
//
// Technique (diff-vs-baseline, changed-only, keyframe-when-no-baseline) adapted
// from Cafu Engine Libs/Network/State.cpp:146-208 (MIT). This is a clean
// reimplementation over MeatEngine's typed ByteStream: a per-field changed-bitmask
// instead of Cafu's byte-level XOR + RLE. See docs/NETCODE_DELTA_COMPRESSION.md.
namespace meat {

// Field bit assignments — part of the wire format, do NOT reorder.
namespace playerfield {
inline constexpr std::uint8_t Pos    = 1u << 0;
inline constexpr std::uint8_t Vel    = 1u << 1;
inline constexpr std::uint8_t Yaw    = 1u << 2;
inline constexpr std::uint8_t Pitch  = 1u << 3;
inline constexpr std::uint8_t Flags  = 1u << 4; // onGround | crouched<<1
inline constexpr std::uint8_t Health = 1u << 5;
inline constexpr std::uint8_t All = Pos | Vel | Yaw | Pitch | Flags | Health;
} // namespace playerfield

namespace entityfield {
inline constexpr std::uint8_t Archetype = 1u << 0;
inline constexpr std::uint8_t Pos       = 1u << 1;
inline constexpr std::uint8_t Yaw       = 1u << 2;
inline constexpr std::uint8_t Anim      = 1u << 3;
inline constexpr std::uint8_t Health    = 1u << 4;
inline constexpr std::uint8_t Data      = 1u << 5;
inline constexpr std::uint8_t All = Archetype | Pos | Yaw | Anim | Health | Data;
} // namespace entityfield

// Encode `cur` as a delta against `baseline`. Pass an empty SnapshotMsg (tick 0,
// no records) as `baseline` to emit a full keyframe.
void encodeDelta(const SnapshotMsg& cur, const SnapshotMsg& baseline, ByteWriter& w);

// Reconstruct a full snapshot from `baseline` + the delta bytes in `r`. The
// caller must pass the SAME baseline the server diffed against (the snapshot whose
// tick equals the delta's baselineTick). Sticky-fails through the reader: on false
// the out-param is garbage and must be discarded. `out` may alias neither argument.
bool decodeDelta(SnapshotMsg& out, const SnapshotMsg& baseline, ByteReader& r);

// Peek the baselineTick without decoding the body, so the client can pick the
// right baseline out of its ring. Returns nullopt if the header is truncated.
std::optional<std::uint64_t> peekDeltaBaseline(ByteReader r); // by value: non-consuming

} // namespace meat
```

### 4.2 `src/engine/net/DeltaSnapshot.cpp`

```cpp
#include "engine/net/DeltaSnapshot.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace meat {
namespace {

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
    return m;
}

void writePlayerFields(ByteWriter& w, const PlayerState& s, std::uint8_t m) {
    if (m & playerfield::Pos)    w.write(s.pos);
    if (m & playerfield::Vel)    w.write(s.vel);
    if (m & playerfield::Yaw)    w.write(s.yaw);
    if (m & playerfield::Pitch)  w.write(s.pitch);
    if (m & playerfield::Flags)  w.write(playerFlags(s));
    if (m & playerfield::Health) w.write(s.health);
}

bool readPlayerFields(ByteReader& r, PlayerState& s, std::uint8_t m) {
    if ((m & playerfield::Pos)   && !r.read(s.pos))   return false;
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
    if (m & entityfield::Pos)       w.write(e.pos);
    if (m & entityfield::Yaw)       w.write(e.yaw);
    if (m & entityfield::Anim)      w.write(e.anim);
    if (m & entityfield::Health)    w.write(e.health);
    if (m & entityfield::Data)      w.write(e.data);
}

bool readEntityFields(ByteReader& r, EntityState& e, std::uint8_t m) {
    if ((m & entityfield::Archetype) && !r.read(e.archetype)) return false;
    if ((m & entityfield::Pos)       && !r.read(e.pos))       return false;
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
```

**Notes on the code**
- The reconstructed player/entity **order is not preserved** (map iteration). That's fine: the client
  re-keys everything by id, and the next baseline is rebuilt from maps again — order never matters.
- Sticky failure: because `ByteReader` latches `ok()=false` on the first overrun, the final
  `if (!r.ok()) return false;` catches truncation even if an intermediate `read` was masked out.
- No allocation per field; two small scratch `ByteWriter`s per set so the count prefix is exact
  without a two-pass count. If you prefer zero scratch buffers, reserve a `count` placeholder and
  patch it — but `ByteWriter` has no in-place patch API today, so the buffer approach is the clean fit.

---

## 5. Integration sketch (for the coordinator — NOT applied here)

These are the small edits to existing files. Shown as guidance; left for the integrator to avoid merge
conflicts.

**a) `Messages.h` — add the type, keep `SnapshotMsg` as-is:**
```cpp
enum class MsgType : std::uint8_t {
    Hello = 1, Welcome, Command, Snapshot, VoxelOp, Inventory, BatchVoxelOp, DeltaSnapshot
};
```
**⚠ Latent bug to fix while here:** `peekType()` (`Messages.cpp:178-179`) currently rejects
`raw > VoxelOp` (=5). That already makes `Inventory`(6) and `BatchVoxelOp`(7) un-peekable — they only
work in `Client.cpp` because that switch reads the type byte itself, bypassing `peekType`. The bound
must become the true max type. Replace the upper check with `raw > static_cast<std::uint8_t>(MsgType::DeltaSnapshot)`.

**b) `CommandMsg` carries the snapshot ack (piggyback — commands already flow at 60 Hz):**
```cpp
struct CommandMsg {
    PlayerCommand cmd;
    std::uint64_t ackSnapshotTick = 0; // newest snapshot the client holds; 0 = none yet
};
// encode: encode(msg.cmd, w); w.write(msg.ackSnapshotTick);
// decode: return decode(msg.cmd, r) && r.read(msg.ackSnapshotTick);
```

**c) `ServerSim` — ring of recent snapshots + per-client acked tick.** In `broadcastSnapshot`
(`ServerSim.cpp:1009-1050`) build the `SnapshotMsg snap` exactly as today, then:
```cpp
// after building `snap` (players + entities), before the send loop:
m_snapshotRing[snap.tick] = snap;                  // std::map<uint64,SnapshotMsg>, keep last 32
while (m_snapshotRing.size() > 32) m_snapshotRing.erase(m_snapshotRing.begin());

static const SnapshotMsg kEmptyBaseline{};         // tick 0, no records => keyframe
for (auto& [peer, player] : m_players) {
    snap.lastCmdTick = player->lastCmdTick;         // per-recipient input ack (unchanged)
    const SnapshotMsg* base = &kEmptyBaseline;
    auto it = m_snapshotRing.find(player->ackedSnapshotTick);
    if (player->ackedSnapshotTick != 0 && it != m_snapshotRing.end()) base = &it->second;
    ByteWriter w;
    w.write(static_cast<std::uint8_t>(MsgType::DeltaSnapshot));
    encodeDelta(snap, *base, w);
    transport.send(peer, std::move(w).take(), false); // still unreliable
}
```
Set `player->ackedSnapshotTick = cmdMsg.ackSnapshotTick` when handling `Command`
(only ever advance it: `ackedSnapshotTick = std::max(ackedSnapshotTick, msg.ackSnapshotTick)`),
and add `std::uint64_t ackedSnapshotTick = 0;` to `ServerSim::Player` plus
`std::map<std::uint64_t, SnapshotMsg> m_snapshotRing;`. `lastCmdTick` is unchanged — note the two acks
are independent and opposite: `lastCmdTick` = server→client "your input up to T is applied";
`ackSnapshotTick` = client→server "I hold snapshot up to S".

**d) `Client` — reconstruct, then reuse `applySnapshot` verbatim.** Add a baseline ring +
`m_ackTick`, and a `MsgType::DeltaSnapshot` case in `pump` (`Client.cpp:62-72` is the model):
```cpp
case MsgType::DeltaSnapshot: {
    const auto baseTick = peekDeltaBaseline(reader);      // reader is positioned past the type byte
    if (!baseTick) break;
    static const SnapshotMsg kEmpty{};
    const SnapshotMsg* base = &kEmpty;
    if (*baseTick != 0) {
        auto it = m_snapRing.find(*baseTick);
        if (it == m_snapRing.end()) break;                // baseline lost: wait for a keyframe
        base = &it->second;
    }
    SnapshotMsg snap;
    if (!decodeDelta(snap, *base, reader)) break;
    if (snap.tick <= m_latestSnapshotTick) break;         // unreliable channel: drop stale
    m_latestSnapshotTick = snap.tick;
    m_ackTick = snap.tick;                                 // piggybacked on the next CommandMsg
    m_snapRing[snap.tick] = snap;                          // keep last 32
    while (m_snapRing.size() > 32) m_snapRing.erase(m_snapRing.begin());
    while (!m_unacked.empty() && m_unacked.front().tick <= snap.lastCmdTick)
        m_unacked.pop_front();
    applySnapshot(snap, physics, player);                  // UNCHANGED path
    break;
}
```
and in `sendCommand` (`Client.cpp:25-30`) send `CommandMsg{cmd, m_ackTick}` instead of `CommandMsg{cmd}`.
`m_snapRing` is `std::map<std::uint64_t, SnapshotMsg>`.

**e) During the transition** you can keep emitting the old `MsgType::Snapshot` too and gate delta on a
capability flag in `WelcomeMsg`, or just cut over — the keyframe path (baselineTick 0) is byte-for-byte
sufficient for a fresh client, so a hard cutover is safe.

**f) `CMakeLists.txt`:** add `src/engine/net/DeltaSnapshot.cpp` to the engine target.

---

## 6. Ack protocol (concise)

```
Server                                             Client
------                                             ------
build authoritative SnapshotMsg (tick S)
store ring[S] = snapshot; evict > 32 old
per client:
  base = ring[client.ackedTick]  (or empty ⇒ keyframe, baselineTick 0)
  send DeltaSnapshot(S, baselineTick, ...)  ──────▶ recv delta
                                                     base = ring[baselineTick] (or empty)
                                                     if base missing ⇒ drop, await keyframe
                                                     snap = decodeDelta(base, bytes)
                                                     ring[S] = snap; evict > 32 old
                                                     m_ackTick = S
  set client.ackedTick =                    ◀────── CommandMsg{ cmd, ackSnapshotTick = S }
      max(ackedTick, msg.ackSnapshotTick)
```

- **Channel:** deltas stay on the **unreliable** ENet channel (`transport.send(..., false)`), exactly
  as full snapshots are today. Loss is fine — the baseline is whatever the client last *acked*, not
  "the previous packet", so a dropped delta never breaks the chain; the next delta still diffs against
  an acked baseline. This is the crucial property that makes unreliable delta safe.
- **Ack cadence:** the ack rides every `CommandMsg` (60 Hz) — no dedicated ack packet, near-zero cost
  (one u64).
- **Cold start / recovery:** `ackedTick == 0`, or an ack that has aged out of the server ring, ⇒ the
  server sends a keyframe (`baselineTick = 0`) against the empty baseline. First snapshot after connect
  is always a keyframe. Self-healing after any burst loss ≥ ring depth (~1.6 s).
- **Reordering:** the existing `snap.tick <= m_latestSnapshotTick` guard (`Client.cpp:65`) drops stale
  deltas. A late delta whose baseline the client still holds could be applied, but dropping is simpler
  and matches today's behaviour.
- **Ring depth 32** ≈ 1.6 s at 20 Hz. Memory: 32 × (players + entities) × record size — at the
  `kMaxSnapshotEntities = 256` cap, ≈ 32 × 256 × ~28 B ≈ 230 KB worst case per side. Fine.

---

## 7. Provenance & attribution

This is a **clean reimplementation of the technique**, not a copy of Cafu source: the mechanism here is
a **typed per-field changed-bitmask over MeatEngine's `ByteStream`**, whereas Cafu uses **opaque
byte-level XOR + RLE over `htonl`-serialized blobs** (`State.cpp:37-66`, `:146-208`). No Cafu lines are
transliterated. MIT does not *require* attribution for an independent reimplementation, but we carry a
citation anyway (good open-source hygiene, and it documents the lineage):

- Source comment in `DeltaSnapshot.h` cites `Cafu Libs/Network/State.cpp:146-208 (MIT)`.
- Add to `THIRD_PARTY.md`:
  > **Delta-snapshot technique** — the baseline-relative "diff a state against a prior baseline,
  > transmit only the differences, send a full keyframe when there is no baseline" pattern is adapted
  > from **Cafu Engine** (`Libs/Network/State.cpp`, MIT, © Carsten Fuchs and contributors,
  > https://bitbucket.org/cafu/cafu). MeatEngine's implementation is an independent reimplementation
  > (per-field bitmask over its own ByteStream); no Cafu source is copied.

If a future change instead **copies** Cafu's `PackBits`/`UnpackBits` RLE verbatim (see §9), the full MIT
header from `State.cpp:1-5` **must** be retained on that file and the THIRD_PARTY entry upgraded to a
verbatim-copy notice.

---

## 8. Follow-up: interest management (separate task)

Delta compression answers *"encode the changes to what I send"*. It does **not** answer *"which of N
entities should this client receive at all"* — today every client still gets every entity. The ranked
survey pairs this with **shortlist #3, Torque3D's scope → prioritize → bandwidth-budget → ghost
pipeline** (`ENGINE_REUSE_SURVEY.md:339`, Torque3D MIT). The clean split:

1. **Scope** — per client, compute the visible/relevant entity set (voxel/grid visibility or radius;
   Cafu uses BSP-PVS, which doesn't fit a voxel world — use a grid/AABB test instead).
2. **Prioritize** — rank scoped entities by relevance (distance, recency of change, threat).
3. **Budget** — fill a per-packet byte budget highest-priority-first; starve the rest to later ticks.
4. **Delta** — encode the chosen set with *this* codec.

That changes the baseline model: with per-client scoping, the server can no longer share one snapshot
ring across clients — each client's baseline becomes *"the per-client view I last sent and it acked"*,
so the ring must move **per client** (or store per-client sent-sets). Design that when interest
management lands; the codec in §4 is unchanged (it already diffs arbitrary `SnapshotMsg` pairs).
**Keep interest management a separate task** — it is a larger, orthogonal workstream (est. 24-40 h in
the survey) and this delta codec is independently shippable and testable now.

---

## 9. Optional alternative: Cafu-style XOR+RLE (when to prefer it)

For **large, fixed-layout blobs** — e.g. the `BatchVoxelOp` crater path (`Client.cpp:84-93`) or a full
chunk — the per-field bitmask has nothing to bite on, but Cafu's byte-XOR+RLE shines (long zero-runs
from unchanged regions). If that need arises, port `PackBits`/`UnpackBits` (`State.cpp:37-91`) as a
standalone `rleEncode/rleDecode` over `std::span<const std::byte>`, drop Cafu's big-endian `htonl`
serialization (MeatEngine is LE), and — because that would be a near-verbatim copy — **retain the MIT
header**. Not needed for the entity-snapshot problem this document solves; noted so the option is on
record.

---

## For the reviewer (Codex)

- **License checked at source, not metadata.** Cloned `github.com/DNS/Cafu`, read `LICENSE.txt` +
  the per-file banners in `State.{hpp,cpp}` — verbatim **MIT**, © Carsten Fuchs; the `ExtLibs/`
  carve-out does not cover `Libs/Network/`. Header quoted in full in §1. Copy-eligible for Apache-2.0
  MeatEngine.
- **Technique, not transliteration.** Cafu's delta is **opaque byte-XOR + RLE** over big-endian blobs
  (`State.cpp:146-208`, `:37-91`). This design is a **typed per-field changed-bitmask** over the
  existing `ByteStream` — simpler, allocation-light, no RLE, no endian conversion. Independent
  reimplementation; attribution carried anyway (§7). The verbatim-copy path (RLE for voxel blobs) is
  §9 with the retain-the-MIT-header caveat spelled out.
- **Zero disruption to prediction/reconciliation.** The in-memory `SnapshotMsg` is untouched; the delta
  is purely a wire codec. Client reconstructs a full `SnapshotMsg` then calls the **existing**
  `applySnapshot` (`Client.cpp:100-126`) — rewind/replay, remote interpolation, and disconnect-prune
  all unchanged. Uniform reader: absent-id + all-bits-set == spawn, no separate opcode.
- **Unreliable-safe by construction.** Baseline = last **acked** snapshot (not "previous packet"), so a
  dropped delta never breaks the chain. Deltas stay on ENet's unreliable channel like today. Keyframe
  (baselineTick 0, empty baseline) is the cold-start and loss-recovery path and is self-healing past
  the 32-deep ring (~1.6 s @ 20 Hz).
- **Two independent acks, don't conflate them.** Existing `lastCmdTick` = server→client input ack
  (kept). New `ackSnapshotTick` = client→server snapshot ack, piggybacked on `CommandMsg` (one u64 at
  60 Hz). Server keeps `ackedTick` monotonic (`std::max`).
- **Latent bug found & flagged (§5a):** `peekType()` (`Messages.cpp:178-179`) rejects `raw > VoxelOp`,
  which already makes `Inventory`(6)/`BatchVoxelOp`(7) un-peekable; adding `DeltaSnapshot`(8) requires
  widening that bound to the true max type. Worth fixing regardless of this feature.
- **Correctness watch-items:** (1) reconstructed record **order is not preserved** (map iteration) — by
  design, everything is re-keyed by id, so it's safe, but flagged in case a test asserts vector order.
  (2) Float diffs use **exact bitwise equality** — correct here because the server compares its own
  deterministic current vs past state; do **not** add an epsilon (it would suppress real updates).
  (3) `decodeDelta` rejects a `baselineTick` that doesn't match the passed baseline — surfaces
  ring/ack bookkeeping bugs instead of silently corrupting state.
- **Not done (out of scope, called out):** interest management (§8, Torque3D shortlist #3) — separate,
  larger task; it will move the baseline ring per-client but leaves this codec unchanged. Lag
  compensation (survey #4) is independent and untouched here.
- **Suggested tests:** round-trip `encodeDelta`→`decodeDelta` equals `cur` for (a) keyframe/empty
  baseline, (b) no-change (expect empty player/entity change lists), (c) single-field change per record,
  (d) spawn + despawn in one delta, (e) truncated/garbage bytes ⇒ `decodeDelta` returns false and never
  reads out of bounds (leans on `ByteReader`'s sticky fail). A property test diffing random snapshot
  pairs against a full-encode oracle is worthwhile.
```
