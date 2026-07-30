#pragma once
#include <glm/glm.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Wire serialization primitives. Little-endian on the wire; we require an LE
// host so writes are plain memcpy — revisit with byte swaps if that ever changes.
// The reader is bounds-checked and sticky-fails: malformed remote data must
// never crash, throw, or read out of bounds.
namespace meat {

static_assert(std::endian::native == std::endian::little,
              "wire format is little-endian; this engine assumes an LE host");

class ByteWriter {
public:
    template <typename T>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
    void write(T value) {
        std::byte raw[sizeof(T)];
        std::memcpy(raw, &value, sizeof(T));
        m_buf.insert(m_buf.end(), raw, raw + sizeof(T));
    }

    void write(bool value) { write(static_cast<std::uint8_t>(value ? 1 : 0)); }

    // u16 length prefix + bytes; length clamped so it always fits the prefix.
    void write(std::string_view value) {
        const std::size_t len = std::min<std::size_t>(value.size(), 0xFFFF);
        write(static_cast<std::uint16_t>(len));
        writeBytes(std::as_bytes(std::span<const char>{value.data(), len}));
    }

    void write(const glm::vec2& v) {
        write(v.x);
        write(v.y);
    }
    void write(const glm::vec3& v) {
        write(v.x);
        write(v.y);
        write(v.z);
    }
    void write(const glm::ivec3& v) {
        write(v.x);
        write(v.y);
        write(v.z);
    }

    void writeBytes(std::span<const std::byte> bytes) {
        m_buf.insert(m_buf.end(), bytes.begin(), bytes.end());
    }

    std::span<const std::byte> data() const { return m_buf; }
    std::size_t size() const { return m_buf.size(); }

    // Steal the buffer (used by pack() to avoid a copy). Writer is spent after.
    std::vector<std::byte> take() && { return std::move(m_buf); }

private:
    std::vector<std::byte> m_buf;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> bytes) : m_data(bytes) {}

    // Once any read overruns, ok() goes false and every subsequent read fails —
    // callers can decode a whole message and check the reader once at the end.
    bool ok() const { return !m_failed; }
    std::size_t remaining() const { return m_data.size() - m_pos; }

    template <typename T>
    std::optional<T> read() {
        T value{};
        if (!read(value)) {
            return std::nullopt;
        }
        return value;
    }

    template <typename T>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
    bool read(T& out) {
        const std::byte* src = take(sizeof(T));
        if (src == nullptr) {
            return false;
        }
        std::memcpy(&out, src, sizeof(T));
        return true;
    }

    bool read(bool& out) {
        std::uint8_t raw = 0;
        if (!read(raw)) {
            return false;
        }
        out = raw != 0;
        return true;
    }

    bool read(std::string& out) {
        std::uint16_t len = 0;
        if (!read(len)) {
            return false;
        }
        const std::byte* src = take(len);
        if (src == nullptr) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(src), len);
        return true;
    }

    bool read(glm::vec2& out) { return read(out.x) && read(out.y); }
    bool read(glm::vec3& out) { return read(out.x) && read(out.y) && read(out.z); }
    bool read(glm::ivec3& out) { return read(out.x) && read(out.y) && read(out.z); }

    bool readBytes(std::span<std::byte> out) {
        const std::byte* src = take(out.size());
        if (src == nullptr) {
            return false;
        }
        std::memcpy(out.data(), src, out.size());
        return true;
    }

private:
    // Advances the cursor and returns the read position, or nullptr (and sets
    // the sticky failure flag) if fewer than n bytes remain.
    const std::byte* take(std::size_t n) {
        if (m_failed || m_data.size() - m_pos < n) {
            m_failed = true;
            return nullptr;
        }
        const std::byte* src = m_data.data() + m_pos;
        m_pos += n;
        return src;
    }

    std::span<const std::byte> m_data;
    std::size_t m_pos = 0;
    bool m_failed = false;
};

} // namespace meat
