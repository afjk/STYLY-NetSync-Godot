// SPDX-License-Identifier: Apache-2.0
#include "binary_writer.hpp"

#include <cstring>
#include <limits>

namespace styly {
namespace netsync {

namespace {

constexpr std::int32_t kInt24Min = -(1 << 23);
constexpr std::int32_t kInt24Max = (1 << 23) - 1;

/// True when `byte` is a UTF-8 continuation byte (10xxxxxx).
inline bool is_continuation(std::uint8_t byte) { return (byte & 0xC0) == 0x80; }

}  // namespace

void BinaryWriter::write_i24(std::int32_t value) {
    std::int32_t clamped = value;
    if (clamped < kInt24Min) {
        clamped = kInt24Min;
    }
    if (clamped > kInt24Max) {
        clamped = kInt24Max;
    }
    const std::uint32_t unsigned_value = static_cast<std::uint32_t>(clamped) & 0xFFFFFFu;
    buffer_.push_back(static_cast<std::uint8_t>(unsigned_value & 0xFF));
    buffer_.push_back(static_cast<std::uint8_t>((unsigned_value >> 8) & 0xFF));
    buffer_.push_back(static_cast<std::uint8_t>((unsigned_value >> 16) & 0xFF));
}

void BinaryWriter::write_f64(double value) {
    static_assert(sizeof(double) == 8, "double must be IEEE-754 binary64");
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        buffer_.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
    }
}

void BinaryWriter::write_string8(const std::string &value) {
    const std::string truncated = truncate_utf8_bytes(value, 255);
    buffer_.push_back(static_cast<std::uint8_t>(truncated.size()));
    buffer_.insert(buffer_.end(), truncated.begin(), truncated.end());
}

void BinaryWriter::write_string16(const std::string &value) {
    const std::size_t length =
        value.size() > std::numeric_limits<std::uint16_t>::max()
            ? truncate_utf8_bytes(value, std::numeric_limits<std::uint16_t>::max()).size()
            : value.size();
    write_u16(static_cast<std::uint16_t>(length));
    buffer_.insert(buffer_.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length));
}

std::string truncate_utf8_bytes(const std::string &value, std::size_t max_bytes) {
    if (value.size() <= max_bytes) {
        return value;
    }
    std::size_t cut = max_bytes;
    // Walk back to the start of the character that straddles the cut point.
    while (cut > 0 && is_continuation(static_cast<std::uint8_t>(value[cut]))) {
        --cut;
    }
    return value.substr(0, cut);
}

std::string truncate_utf8_code_points(const std::string &value, std::size_t max_code_points) {
    std::size_t code_points = 0;
    std::size_t index = 0;
    while (index < value.size()) {
        if (!is_continuation(static_cast<std::uint8_t>(value[index]))) {
            if (code_points == max_code_points) {
                return value.substr(0, index);
            }
            ++code_points;
        }
        ++index;
    }
    return value;
}

}  // namespace netsync
}  // namespace styly
