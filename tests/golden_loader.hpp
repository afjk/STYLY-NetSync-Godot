// SPDX-License-Identifier: Apache-2.0
// Helpers for loading tests/golden/vectors.json into the C++ test suites.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "core/json_util.hpp"
#include "protocol/protocol_v8.hpp"

namespace styly {
namespace netsync {
namespace test {

inline bool read_file(const std::string &path, std::string &out) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    out.clear();
    char buffer[8192];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, read);
    }
    std::fclose(file);
    return true;
}

inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

inline std::vector<std::uint8_t> from_hex(const std::string &hex) {
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int high = hex_nibble(hex[i]);
        const int low = hex_nibble(hex[i + 1]);
        if (high < 0 || low < 0) {
            return std::vector<std::uint8_t>();
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

/// Load the golden document. `argv[1]`, then `$STYLY_GOLDEN_VECTORS`, then the
/// path baked in at compile time are tried in order.
inline bool load_golden(int argc, char **argv, JsonValue &out, std::string &path_used) {
    std::vector<std::string> candidates;
    if (argc > 1) {
        candidates.emplace_back(argv[1]);
    }
    if (const char *from_env = std::getenv("STYLY_GOLDEN_VECTORS")) {
        candidates.emplace_back(from_env);
    }
#ifdef STYLY_GOLDEN_VECTORS_PATH
    candidates.emplace_back(STYLY_GOLDEN_VECTORS_PATH);
#endif
    candidates.emplace_back("tests/golden/vectors.json");

    for (const std::string &candidate : candidates) {
        std::string text;
        if (!read_file(candidate, text)) {
            continue;
        }
        std::string error;
        if (!JsonValue::parse(text, out, &error)) {
            std::fprintf(stderr, "failed to parse %s: %s\n", candidate.c_str(), error.c_str());
            return false;
        }
        path_used = candidate;
        return true;
    }
    std::fprintf(stderr, "could not open tests/golden/vectors.json (tried %zu paths)\n",
                 candidates.size());
    return false;
}

// --- JSON → protocol structure builders -------------------------------------

inline Vec3 vec3_from_json(const JsonValue &value) {
    return Vec3(value[static_cast<std::size_t>(0)].number_value(),
                value[static_cast<std::size_t>(1)].number_value(),
                value[static_cast<std::size_t>(2)].number_value());
}

inline Quat quat_from_json(const JsonValue &value) {
    return Quat(value[static_cast<std::size_t>(0)].number_value(),
                value[static_cast<std::size_t>(1)].number_value(),
                value[static_cast<std::size_t>(2)].number_value(),
                value[static_cast<std::size_t>(3)].number_value());
}

inline PoseTransform transform_from_json(const JsonValue &value) {
    PoseTransform out;
    out.position = vec3_from_json(value["p"]);
    out.rotation = quat_from_json(value["q"]);
    return out;
}

inline ClientPoseBody body_from_json(const JsonValue &value) {
    ClientPoseBody body;
    body.pose_seq = static_cast<std::uint16_t>(value["pose_seq"].int_value());
    body.flags = static_cast<std::uint8_t>(value["flags"].int_value());
    body.xr_origin_delta_position = vec3_from_json(value["xr_origin_delta"]);
    body.xr_origin_delta_yaw = value["xr_origin_delta_yaw"].number_value();
    body.physical = transform_from_json(value["physical"]);
    body.head = transform_from_json(value["head"]);
    body.right_hand = transform_from_json(value["right_hand"]);
    body.left_hand = transform_from_json(value["left_hand"]);
    for (const JsonValue &entry : value["virtuals"].array_items()) {
        body.virtuals.push_back(transform_from_json(entry));
    }
    return body;
}

inline NetworkVariableEntry variable_from_json(const JsonValue &value) {
    NetworkVariableEntry entry;
    entry.name = value["name"].string_value();
    entry.value = value["value"].string_value();
    entry.last_writer_client_no =
        static_cast<std::uint16_t>(value["last_writer_client_no"].int_value());
    return entry;
}

}  // namespace test
}  // namespace netsync
}  // namespace styly
