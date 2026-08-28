// SPDX-License-Identifier: Apache-2.0
// Little-endian binary writer for the STYLY NetSync wire format.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace styly {
namespace netsync {

/// Append-only little-endian writer over a std::vector<uint8_t>.
///
/// Mirrors the byte-level behaviour of System.IO.BinaryWriter as used by the
/// upstream Unity serializer and of Python's `struct.pack("<...")`.
class BinaryWriter {
public:
    BinaryWriter() = default;
    explicit BinaryWriter(std::size_t reserve) { buffer_.reserve(reserve); }

    void write_u8(std::uint8_t value) { buffer_.push_back(value); }
    void write_i8(std::int8_t value) { buffer_.push_back(static_cast<std::uint8_t>(value)); }

    void write_u16(std::uint16_t value) {
        buffer_.push_back(static_cast<std::uint8_t>(value & 0xFF));
        buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    }
    void write_i16(std::int16_t value) { write_u16(static_cast<std::uint16_t>(value)); }

    void write_u32(std::uint32_t value) {
        buffer_.push_back(static_cast<std::uint8_t>(value & 0xFF));
        buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        buffer_.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        buffer_.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    }
    void write_i32(std::int32_t value) { write_u32(static_cast<std::uint32_t>(value)); }

    /// Signed 24-bit little-endian, clamped to the int24 range.
    void write_i24(std::int32_t value);

    /// IEEE-754 binary64, little-endian.
    void write_f64(double value);

    /// UTF-8 bytes with a single-byte length prefix. Truncated to 255 bytes on a
    /// UTF-8 character boundary, matching upstream `Math.Min(bytes.Length, 255)`
    /// with the extra guarantee that a multi-byte sequence is never split.
    void write_string8(const std::string &value);

    /// UTF-8 bytes with a two-byte little-endian length prefix.
    void write_string16(const std::string &value);

    /// Raw bytes, no prefix.
    void write_bytes(const std::uint8_t *data, std::size_t size) {
        buffer_.insert(buffer_.end(), data, data + size);
    }
    void write_bytes(const std::vector<std::uint8_t> &data) {
        buffer_.insert(buffer_.end(), data.begin(), data.end());
    }

    const std::vector<std::uint8_t> &buffer() const { return buffer_; }
    std::vector<std::uint8_t> take() { return std::move(buffer_); }
    std::size_t size() const { return buffer_.size(); }
    void clear() { buffer_.clear(); }
    void reserve(std::size_t n) { buffer_.reserve(n); }

private:
    std::vector<std::uint8_t> buffer_;
};

/// Truncate a UTF-8 string to at most `max_bytes` bytes without splitting a
/// multi-byte sequence.
std::string truncate_utf8_bytes(const std::string &value, std::size_t max_bytes);

/// Truncate a UTF-8 string to at most `max_code_points` Unicode code points.
///
/// This reproduces the Python serializer's `value[:N]` slicing, which is the
/// reference behaviour for network-variable names and values. (The Unity client
/// slices by UTF-16 code unit instead; the two agree for all BMP text — see
/// docs/UPSTREAM_COMPATIBILITY.md.)
std::string truncate_utf8_code_points(const std::string &value, std::size_t max_code_points);

}  // namespace netsync
}  // namespace styly
