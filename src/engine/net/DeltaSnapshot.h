#pragma once
#include "engine/net/ByteStream.h"
#include "engine/net/Messages.h"

#include <cstdint>
#include <optional>

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
