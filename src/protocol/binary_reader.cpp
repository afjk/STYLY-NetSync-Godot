// SPDX-License-Identifier: Apache-2.0
#include "binary_reader.hpp"

#include <cstring>

namespace styly {
namespace netsync {

bool BinaryReader::require(std::size_t count) {
    if (!ok_) {
        return false;
    }
    if (offset_ + count > size_ || offset_ + count < offset_) {
        ok_ = false;
        return false;
    }
    return true;
}

std::uint8_t BinaryReader::read_u8() {
    if (!require(1)) {
        return 0;
    }
    return data_[offset_++];
}

std::uint16_t BinaryReader::read_u16() {
    if (!require(2)) {
        return 0;
    }
    const std::uint16_t value = static_cast<std::uint16_t>(data_[offset_]) |
                                (static_cast<std::uint16_t>(data_[offset_ + 1]) << 8);
    offset_ += 2;
    return value;
}

std::uint32_t BinaryReader::read_u32() {
    if (!require(4)) {
        return 0;
    }
    const std::uint32_t value = static_cast<std::uint32_t>(data_[offset_]) |
                                (static_cast<std::uint32_t>(data_[offset_ + 1]) << 8) |
                                (static_cast<std::uint32_t>(data_[offset_ + 2]) << 16) |
                                (static_cast<std::uint32_t>(data_[offset_ + 3]) << 24);
    offset_ += 4;
    return value;
}

std::int32_t BinaryReader::read_i24() {
    if (!require(3)) {
        return 0;
    }
    std::int32_t value = static_cast<std::int32_t>(data_[offset_]) |
                         (static_cast<std::int32_t>(data_[offset_ + 1]) << 8) |
                         (static_cast<std::int32_t>(data_[offset_ + 2]) << 16);
    offset_ += 3;
    if ((value & 0x800000) != 0) {
        value |= static_cast<std::int32_t>(0xFF000000u);
    }
    return value;
}

double BinaryReader::read_f64() {
    if (!require(8)) {
        return 0.0;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(i)])
                << (8 * i);
    }
    offset_ += 8;
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string BinaryReader::read_string8() {
    const std::uint8_t length = read_u8();
    if (!require(length)) {
        return std::string();
    }
    std::string value(reinterpret_cast<const char *>(data_ + offset_), length);
    offset_ += length;
    return value;
}

std::string BinaryReader::read_string16() {
    const std::uint16_t length = read_u16();
    if (!require(length)) {
        return std::string();
    }
    std::string value(reinterpret_cast<const char *>(data_ + offset_), length);
    offset_ += length;
    return value;
}

std::vector<std::uint8_t> BinaryReader::read_bytes(std::size_t count) {
    if (!require(count)) {
        return std::vector<std::uint8_t>();
    }
    std::vector<std::uint8_t> value(data_ + offset_, data_ + offset_ + count);
    offset_ += count;
    return value;
}

void BinaryReader::skip(std::size_t count) {
    if (require(count)) {
        offset_ += count;
    }
}

}  // namespace netsync
}  // namespace styly
