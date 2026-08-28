// SPDX-License-Identifier: Apache-2.0
#include "device_id.hpp"

#include <cstdio>
#include <random>

namespace styly {
namespace netsync {

namespace {

std::string trim(const std::string &value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
    };
    while (begin < end && is_space(value[begin])) {
        ++begin;
    }
    while (end > begin && is_space(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

}  // namespace

std::string generate_uuid_v4() {
    std::random_device source;
    std::mt19937_64 engine(
        (static_cast<std::uint64_t>(source()) << 32) ^ static_cast<std::uint64_t>(source()));
    std::uniform_int_distribution<std::uint32_t> byte_distribution(0, 255);

    std::uint8_t bytes[16];
    for (std::uint8_t &byte : bytes) {
        byte = static_cast<std::uint8_t>(byte_distribution(engine));
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);  // variant 1

    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0F]);
    }
    return out;
}

std::string read_device_id(const std::string &path) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return std::string();
    }
    char buffer[512];
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[read] = '\0';
    return trim(std::string(buffer, read));
}

bool write_device_id(const std::string &path, const std::string &device_id) {
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const std::size_t written = std::fwrite(device_id.data(), 1, device_id.size(), file);
    const bool ok = written == device_id.size();
    std::fclose(file);
    return ok;
}

std::string load_or_create_device_id(const std::string &path) {
    const std::string existing = read_device_id(path);
    if (!existing.empty()) {
        return existing;
    }
    const std::string generated = generate_uuid_v4();
    write_device_id(path, generated);
    return generated;
}

}  // namespace netsync
}  // namespace styly
