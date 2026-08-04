#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace meat::compression {

// Thin, safe wrapper over LZAV (MIT, third_party/lzav) for in-memory byte buffers:
// save files, resource-archive entries, cooked assets, and large reliable network
// payloads. Output is a self-describing blob (magic + original size + payload), so
// decompress() needs only the blob. Lossless. Incompressible input is stored raw
// (never inflates). LZAV is bounds-checked, so a corrupt or hostile blob returns
// std::nullopt rather than crashing — safe to run on untrusted saves/packets.

// Compress `input` into a self-describing blob. Empty input yields a valid blob.
std::vector<std::byte> compress(std::span<const std::byte> input);

// Decompress a blob produced by compress(). Returns std::nullopt if the header is
// invalid, the payload is corrupt, or the result size disagrees with the header.
std::optional<std::vector<std::byte>> decompress(std::span<const std::byte> blob);

} // namespace meat::compression
