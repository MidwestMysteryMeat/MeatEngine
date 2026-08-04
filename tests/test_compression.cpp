// Compression wrapper (LZAV) round-trip + safety. The blob is self-describing, so
// these pin the guarantees a save/asset/packet path relies on: lossless round trip
// for empty, compressible, and incompressible data; a real size win on repetitive
// data; and std::nullopt (never a crash) on corrupt or malformed input.

#include "Harness.h"

#include "engine/core/Compression.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using meattest::check;

std::vector<std::byte> bytesOf(const std::string& s) {
    std::vector<std::byte> v(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) v[i] = static_cast<std::byte>(s[i]);
    return v;
}

bool roundTrips(const std::vector<std::byte>& original) {
    const std::vector<std::byte> blob = meat::compression::compress(original);
    const auto back = meat::compression::decompress(blob);
    return back.has_value() && *back == original;
}

void testRoundTrips() {
    std::printf("compression round-trips empty, compressible, and incompressible data\n");
    check(roundTrips({}), "empty input round-trips");
    check(roundTrips(bytesOf("hello world")), "a short string round-trips");

    // Highly compressible: a long repetitive buffer.
    std::vector<std::byte> repet(4096, std::byte{0xAB});
    for (std::size_t i = 0; i < repet.size(); i += 7) repet[i] = std::byte{0x01};
    check(roundTrips(repet), "a large repetitive buffer round-trips");

    // Incompressible: deterministic pseudo-random bytes (exercises the raw-store path).
    std::vector<std::byte> noise(4096);
    std::uint64_t rng = 0x9E3779B97F4A7C15ull;
    for (std::byte& b : noise) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        b = static_cast<std::byte>(rng & 0xFF);
    }
    check(roundTrips(noise), "incompressible noise round-trips (stored raw)");
}

void testCompressibleDataShrinks() {
    std::printf("compressible data actually gets smaller\n");
    std::vector<std::byte> repet(8192, std::byte{0x42});
    const std::vector<std::byte> blob = meat::compression::compress(repet);
    check(blob.size() < repet.size() / 2,
          "a repetitive 8 KB buffer compresses to under half its size");
}

void testCorruptInputFailsClosed() {
    std::printf("corrupt or malformed blobs decompress to nullopt, not a crash\n");
    check(!meat::compression::decompress({}).has_value(), "an empty blob is rejected");
    check(!meat::compression::decompress(bytesOf("xx")).has_value(),
          "a too-short blob (no header) is rejected");
    check(!meat::compression::decompress(bytesOf("NOPEsize....payload")).has_value(),
          "an unknown magic is rejected");

    // A valid compressed blob with its payload bytes mangled must fail, not crash.
    std::vector<std::byte> repet(2048, std::byte{0x7E});
    for (std::size_t i = 0; i < repet.size(); i += 3) repet[i] = std::byte{0x11};
    std::vector<std::byte> blob = meat::compression::compress(repet);
    if (blob.size() > 12)
        for (std::size_t i = 8; i < blob.size(); ++i)
            blob[i] = static_cast<std::byte>(~static_cast<unsigned>(blob[i]) & 0xFF);
    const auto back = meat::compression::decompress(blob);
    check(!back.has_value() || *back != repet,
          "a mangled payload is detected (nullopt) or at least never returns the original");
}

} // namespace

namespace meattest {

void runCompression() {
    testRoundTrips();
    testCompressibleDataShrinks();
    testCorruptInputFailsClosed();
}

} // namespace meattest
