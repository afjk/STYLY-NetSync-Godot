// SPDX-License-Identifier: Apache-2.0
// Bounds-checked little-endian binary reader for the STYLY NetSync wire format.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace styly {
namespace netsync {

/// Reader over a borrowed byte range.
///
/// Every accessor is bounds-checked. On overrun the reader latches a permanent
/// failure flag and subsequent reads return zero, so a malformed payload can
/// never read out of bounds or spin. Callers check `ok()` once at the end of a
/// message rather than after every field.
class BinaryReader {
public:
    BinaryReader(const std::uint8_t *data, std::size_t size) : data_(data), size_(size) {}
    explicit BinaryReader(const std::vector<std::uint8_t> &data)
        : data_(data.data()), size_(data.size()) {}

    std::uint8_t read_u8();
    std::int8_t read_i8() { return static_cast<std::int8_t>(read_u8()); }
    std::uint16_t read_u16();
    std::int16_t read_i16() { return static_cast<std::int16_t>(read_u16()); }
    std::uint32_t read_u32();
    std::int32_t read_i32() { return static_cast<std::int32_t>(read_u32()); }

    /// Signed 24-bit little-endian, sign-extended to 32 bits.
    std::int32_t read_i24();

    /// IEEE-754 binary64, little-endian.
    double read_f64();

    /// UTF-8 bytes preceded by a single-byte length.
    std::string read_string8();

    /// UTF-8 bytes preceded by a two-byte little-endian length.
    std::string read_string16();

    /// Raw bytes; returns an empty vector when the range would overrun.
    std::vector<std::uint8_t> read_bytes(std::size_t count);

    /// Advance without materialising the bytes.
    void skip(std::size_t count);

    bool ok() const { return ok_; }
    std::size_t offset() const { return offset_; }
    std::size_t size() const { return size_; }
    std::size_t remaining() const { return ok_ && offset_ <= size_ ? size_ - offset_ : 0; }

    /// Pointer to the raw buffer, for callers that need to slice a sub-range
    /// (e.g. the object pose body cache).
    const std::uint8_t *data() const { return data_; }

private:
    bool require(std::size_t count);

    const std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
    bool ok_ = true;
};

}  // namespace netsync
}  // namespace styly
