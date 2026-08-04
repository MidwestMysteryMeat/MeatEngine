#include "engine/core/Compression.h"

// Vendored third-party header (MIT) — not held to the project's /W4 -Wall -Wextra.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4505) // unreferenced local function (lzav_compress_hi)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "lzav.h" // third_party/lzav (MIT)
#ifdef _MSC_VER
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstdint>
#include <cstring>

namespace meat::compression {

namespace {

// Header: magic(4) | originalSize(4, LE). Magic doubles as the storage mode so a
// buffer LZAV couldn't shrink is kept verbatim instead of inflated.
constexpr std::uint32_t kMagicLzav = 0x315A414Du; // "MAZ1" — LZAV-compressed
constexpr std::uint32_t kMagicRaw = 0x30574152u;  // "RAW0" — stored uncompressed
constexpr std::size_t kHeader = 8;

void writeU32(std::byte* p, std::uint32_t v) { std::memcpy(p, &v, sizeof v); }
std::uint32_t readU32(const std::byte* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof v);
    return v;
}

std::vector<std::byte> withHeader(std::uint32_t magic, std::uint32_t origSize) {
    std::vector<std::byte> out(kHeader);
    writeU32(out.data(), magic);
    writeU32(out.data() + 4, origSize);
    return out;
}

std::vector<std::byte> storeRaw(std::span<const std::byte> input) {
    std::vector<std::byte> out = withHeader(kMagicRaw, static_cast<std::uint32_t>(input.size()));
    out.insert(out.end(), input.begin(), input.end());
    return out;
}

} // namespace

std::vector<std::byte> compress(std::span<const std::byte> input) {
    const int srclen = static_cast<int>(input.size());
    if (srclen <= 0) return withHeader(kMagicRaw, 0); // empty → header only

    const int bound = lzav_compress_bound(srclen);
    if (bound <= 0) return storeRaw(input);

    std::vector<std::byte> out = withHeader(kMagicLzav, static_cast<std::uint32_t>(srclen));
    out.resize(kHeader + static_cast<std::size_t>(bound));
    const int clen =
        lzav_compress_default(input.data(), out.data() + kHeader, srclen, bound);
    // Keep the compressed form only if it actually shrank; otherwise store raw so
    // incompressible data never costs more than its bytes plus the header.
    if (clen <= 0 || clen >= srclen) return storeRaw(input);
    out.resize(kHeader + static_cast<std::size_t>(clen));
    return out;
}

std::optional<std::vector<std::byte>> decompress(std::span<const std::byte> blob) {
    if (blob.size() < kHeader) return std::nullopt;
    const std::uint32_t magic = readU32(blob.data());
    const std::uint32_t origSize = readU32(blob.data() + 4);
    const std::span<const std::byte> payload = blob.subspan(kHeader);

    if (magic == kMagicRaw) {
        if (payload.size() != origSize) return std::nullopt; // header/payload disagree
        return std::vector<std::byte>(payload.begin(), payload.end());
    }
    if (magic != kMagicLzav) return std::nullopt; // unknown format

    std::vector<std::byte> out(origSize);
    const int got = lzav_decompress(payload.data(), out.data(),
                                    static_cast<int>(payload.size()),
                                    static_cast<int>(origSize));
    if (got < 0 || static_cast<std::uint32_t>(got) != origSize) return std::nullopt;
    return out;
}

} // namespace meat::compression
