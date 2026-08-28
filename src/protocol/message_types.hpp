// SPDX-License-Identifier: Apache-2.0
// STYLY NetSync protocol v8 — message type identifiers and protocol constants.
//
// Mirrors upstream:
//   STYLY-NetSync-Server/src/styly_netsync/binary_serializer.py
//   STYLY-NetSync-Unity/.../Runtime/Internal Scripts/BinarySerializer.cs
//
// This header must not depend on Godot or on any transport implementation.
#pragma once

#include <cstdint>

namespace styly {
namespace netsync {

/// Wire protocol version. Rides on pose/object messages and the client hello.
inline constexpr std::uint8_t kProtocolVersion = 8;

enum MessageType : std::uint8_t {
    MSG_CLIENT_TRANSFORM = 1,  ///< legacy, never emitted at v8
    MSG_ROOM_TRANSFORM = 2,    ///< legacy, never emitted at v8
    MSG_RPC = 3,
    MSG_RPC_SERVER = 4,  ///< reserved — do not repurpose
    MSG_RPC_CLIENT = 5,  ///< reserved — do not repurpose
    MSG_DEVICE_ID_MAPPING = 6,
    MSG_GLOBAL_VAR_SET = 7,
    MSG_GLOBAL_VAR_SYNC = 8,
    MSG_CLIENT_VAR_SET = 9,
    MSG_CLIENT_VAR_SYNC = 10,
    MSG_CLIENT_POSE = 11,
    MSG_ROOM_POSE = 12,
    MSG_OBJECT_POSE = 13,
    MSG_ROOM_OBJECTS = 14,
    MSG_OBJECT_OWNERSHIP_REQUEST = 15,
    MSG_OBJECT_OWNERSHIP_CHANGED = 16,
    MSG_OBJECT_OWNERSHIP_REJECTED = 17,
    MSG_CLIENT_VAR_CLEAR = 18,
    MSG_CLIENT_HELLO = 19,
};

/// Inclusive range accepted by upstream deserializers.
inline constexpr std::uint8_t kMessageTypeMin = MSG_CLIENT_TRANSFORM;
inline constexpr std::uint8_t kMessageTypeMax = MSG_CLIENT_HELLO;

/// Flag byte of MSG_CLIENT_HELLO.
inline constexpr std::uint8_t kClientHelloFlagStealth = 0x01;

/// Pose flags (`flags` byte of the client pose body).
enum PoseFlags : std::uint8_t {
    POSE_FLAG_NONE = 0,
    POSE_FLAG_STEALTH = 1 << 0,
    POSE_FLAG_PHYSICAL_VALID = 1 << 1,
    POSE_FLAG_HEAD_VALID = 1 << 2,
    POSE_FLAG_RIGHT_VALID = 1 << 3,
    POSE_FLAG_LEFT_VALID = 1 << 4,
    POSE_FLAG_VIRTUALS_VALID = 1 << 5,
    POSE_FLAG_MOVING_FLOOR_LOCAL = 1 << 6,
};

/// Encoding flags (`encodingFlags` byte). Derived from the pose flags, never chosen.
enum EncodingFlags : std::uint8_t {
    ENCODING_PHYSICAL_YAW_ONLY = 1 << 0,
    ENCODING_RIGHT_REL_HEAD = 1 << 1,
    ENCODING_LEFT_REL_HEAD = 1 << 2,
    ENCODING_VIRTUAL_REL_HEAD = 1 << 3,
    ENCODING_PHYSICAL_IS_XRORIGIN_DELTA = 1 << 4,
    ENCODING_FLAGS_DEFAULT = ENCODING_PHYSICAL_YAW_ONLY | ENCODING_RIGHT_REL_HEAD |
                             ENCODING_LEFT_REL_HEAD | ENCODING_VIRTUAL_REL_HEAD |
                             ENCODING_PHYSICAL_IS_XRORIGIN_DELTA,
};

/// Ownership request operation types (`operationType` byte of message 15).
enum OwnershipOperation : std::uint8_t {
    OWNERSHIP_OP_RELEASE = 1,
    OWNERSHIP_OP_REQUEST = 2,
};

/// Ownership rejection reasons emitted by the server (`reasonCode` of message 17).
enum OwnershipRejectReason : std::uint8_t {
    OWNERSHIP_REJECT_NOT_OWNER = 1,
};

/// Maximum number of virtual transforms carried in one pose body.
inline constexpr int kMaxVirtualTransforms = 50;

/// Network-variable limits shared with the server.
inline constexpr int kMaxVariableNameChars = 64;
inline constexpr int kMaxVariableValueChars = 1024;

/// Object PUB topic suffix: roomId + "\0obj".
inline constexpr char kObjectTopicSuffix[4] = {'\0', 'o', 'b', 'j'};
inline constexpr int kObjectTopicSuffixLength = 4;

/// Default ports.
inline constexpr int kDefaultControlPort = 5555;
inline constexpr int kDefaultSubPort = 5556;
inline constexpr int kDefaultTransformPort = 5557;
inline constexpr int kDefaultDiscoveryPort = 9999;

}  // namespace netsync
}  // namespace styly
