// SPDX-License-Identifier: Apache-2.0
#include "protocol_v8.hpp"

#include <algorithm>

namespace styly {
namespace netsync {

namespace {

/// Names and values are sliced by code point before encoding, matching the
/// Python reference (`value[:64]` / `value[:1024]`).
std::string clamp_variable_name(const std::string &name) {
    return truncate_utf8_code_points(name, kMaxVariableNameChars);
}

std::string clamp_variable_value(const std::string &value) {
    return truncate_utf8_code_points(value, kMaxVariableValueChars);
}

void write_pose_body(BinaryWriter &writer, const ClientPoseBody &body) {
    const std::uint8_t flags = sanitize_pose_flags(body.flags);
    const std::uint8_t encoding_flags = compute_encoding_flags(flags);

    writer.write_u16(body.pose_seq);
    writer.write_u8(flags);
    writer.write_u8(encoding_flags);

    const bool physical_valid = (flags & POSE_FLAG_PHYSICAL_VALID) != 0;
    const bool head_valid = (flags & POSE_FLAG_HEAD_VALID) != 0;
    const bool right_valid = head_valid && (flags & POSE_FLAG_RIGHT_VALID) != 0;
    const bool left_valid = head_valid && (flags & POSE_FLAG_LEFT_VALID) != 0;
    const bool virtual_valid = head_valid && (flags & POSE_FLAG_VIRTUALS_VALID) != 0;
    const bool moving_floor_local = (flags & POSE_FLAG_MOVING_FLOOR_LOCAL) != 0;

    const Quat head_rot_normalized = normalize_quaternion(body.head.rotation);

    if (physical_valid) {
        if (moving_floor_local) {
            const double physical_yaw = quaternion_to_yaw_degrees(body.physical.rotation);
            writer.write_i16(quantize_signed(body.physical.position.x, kLocoPosScale));
            writer.write_i16(quantize_signed(body.physical.position.y, kLocoPosScale));
            writer.write_i16(quantize_signed(body.physical.position.z, kLocoPosScale));
            writer.write_i16(quantize_signed(physical_yaw, kPhysicalYawScale));
        } else {
            writer.write_i16(quantize_signed(body.xr_origin_delta_position.x, kLocoPosScale));
            writer.write_i16(quantize_signed(body.xr_origin_delta_position.y, kLocoPosScale));
            writer.write_i16(quantize_signed(body.xr_origin_delta_position.z, kLocoPosScale));
            writer.write_i16(quantize_signed(body.xr_origin_delta_yaw, kPhysicalYawScale));
        }
    }

    if (head_valid) {
        writer.write_i24(quantize_signed_int24(body.head.position.x, kAbsPosScale));
        writer.write_i24(quantize_signed_int24(body.head.position.y, kAbsPosScale));
        writer.write_i24(quantize_signed_int24(body.head.position.z, kAbsPosScale));
        writer.write_u32(compress_quaternion_smallest_three(head_rot_normalized));
    }

    const Quat inv_head_rot = quaternion_inverse(head_rot_normalized);

    // Hands and virtuals share one relative-encoding shape.
    const auto write_relative = [&](const PoseTransform &transform) {
        const Quat rot = normalize_quaternion(transform.rotation);
        const Vec3 rel_pos(transform.position.x - body.head.position.x,
                           transform.position.y - body.head.position.y,
                           transform.position.z - body.head.position.z);
        const Quat rel_rot = quaternion_multiply(inv_head_rot, rot);
        writer.write_i16(quantize_signed(rel_pos.x, kRelPosScale));
        writer.write_i16(quantize_signed(rel_pos.y, kRelPosScale));
        writer.write_i16(quantize_signed(rel_pos.z, kRelPosScale));
        writer.write_u32(compress_quaternion_smallest_three(rel_rot));
    };

    if (right_valid) {
        write_relative(body.right_hand);
    }
    if (left_valid) {
        write_relative(body.left_hand);
    }

    std::size_t virtual_count = 0;
    if (virtual_valid) {
        virtual_count = std::min<std::size_t>(body.virtuals.size(),
                                              static_cast<std::size_t>(kMaxVirtualTransforms));
    }
    writer.write_u8(static_cast<std::uint8_t>(virtual_count));
    for (std::size_t i = 0; i < virtual_count; ++i) {
        write_relative(body.virtuals[i]);
    }
}

bool read_pose_body(BinaryReader &reader, ClientPoseBody &body) {
    body.pose_seq = reader.read_u16();
    body.flags = reader.read_u8();
    body.encoding_flags = reader.read_u8();
    if (!reader.ok()) {
        return false;
    }

    const std::uint8_t flags = body.flags;
    const bool physical_valid = (flags & POSE_FLAG_PHYSICAL_VALID) != 0;
    const bool head_valid = (flags & POSE_FLAG_HEAD_VALID) != 0;
    const bool right_valid = head_valid && (flags & POSE_FLAG_RIGHT_VALID) != 0;
    const bool left_valid = head_valid && (flags & POSE_FLAG_LEFT_VALID) != 0;
    const bool virtual_valid = head_valid && (flags & POSE_FLAG_VIRTUALS_VALID) != 0;
    const bool moving_floor_local = (flags & POSE_FLAG_MOVING_FLOOR_LOCAL) != 0;

    body.physical = PoseTransform();
    body.head = PoseTransform();
    body.right_hand = PoseTransform();
    body.left_hand = PoseTransform();
    body.virtuals.clear();
    body.xr_origin_delta_position = Vec3();
    body.xr_origin_delta_yaw = 0.0;

    std::int16_t dx_q = 0;
    std::int16_t dy_q = 0;
    std::int16_t dz_q = 0;
    std::int16_t dyaw_q = 0;

    if (physical_valid) {
        if (!moving_floor_local &&
            (body.encoding_flags & ENCODING_PHYSICAL_IS_XRORIGIN_DELTA) == 0) {
            // Upstream raises here; reject rather than mis-decode.
            return false;
        }
        dx_q = reader.read_i16();
        dy_q = reader.read_i16();
        dz_q = reader.read_i16();
        dyaw_q = reader.read_i16();
        if (!moving_floor_local) {
            body.xr_origin_delta_position =
                Vec3(dequantize_signed(dx_q, kLocoPosScale), dequantize_signed(dy_q, kLocoPosScale),
                     dequantize_signed(dz_q, kLocoPosScale));
            body.xr_origin_delta_yaw = dequantize_signed(dyaw_q, kPhysicalYawScale);
        }
    }

    Vec3 head_pos;
    Quat head_rot;
    if (head_valid) {
        const std::int32_t hx = reader.read_i24();
        const std::int32_t hy = reader.read_i24();
        const std::int32_t hz = reader.read_i24();
        const std::uint32_t packed_head = reader.read_u32();
        if (!reader.ok()) {
            return false;
        }
        head_pos = Vec3(dequantize_signed(hx, kAbsPosScale), dequantize_signed(hy, kAbsPosScale),
                        dequantize_signed(hz, kAbsPosScale));
        head_rot = decompress_quaternion_smallest_three(packed_head);
        body.head.position = head_pos;
        body.head.rotation = head_rot;
    }

    // Physical pose reconstruction, mirroring the reference decoder.
    if (physical_valid && moving_floor_local) {
        body.physical.position =
            Vec3(dequantize_signed(dx_q, kLocoPosScale), dequantize_signed(dy_q, kLocoPosScale),
                 dequantize_signed(dz_q, kLocoPosScale));
        body.physical.rotation =
            yaw_degrees_to_quaternion(dequantize_signed(dyaw_q, kPhysicalYawScale));
    } else if (physical_valid && head_valid) {
        const Vec3 translated(head_pos.x - body.xr_origin_delta_position.x,
                              head_pos.y - body.xr_origin_delta_position.y,
                              head_pos.z - body.xr_origin_delta_position.z);
        body.physical.position = rotate_yaw_vector(translated, -body.xr_origin_delta_yaw);
        const double head_yaw = quaternion_to_yaw_degrees(head_rot);
        const double physical_yaw = normalize_yaw_degrees(head_yaw - body.xr_origin_delta_yaw);
        body.physical.rotation = yaw_degrees_to_quaternion(physical_yaw);
    }
    // physical_valid && !head_valid: unreconstructible; leave identity (upstream warns).

    const auto read_relative = [&](PoseTransform &out) {
        const std::int16_t rx = reader.read_i16();
        const std::int16_t ry = reader.read_i16();
        const std::int16_t rz = reader.read_i16();
        const std::uint32_t packed = reader.read_u32();
        if (!reader.ok()) {
            return;
        }
        const Vec3 rel_pos(dequantize_signed(rx, kRelPosScale), dequantize_signed(ry, kRelPosScale),
                           dequantize_signed(rz, kRelPosScale));
        const Quat rel_rot = decompress_quaternion_smallest_three(packed);
        out.position = Vec3(head_pos.x + rel_pos.x, head_pos.y + rel_pos.y, head_pos.z + rel_pos.z);
        out.rotation = normalize_quaternion(quaternion_multiply(head_rot, rel_rot));
    };

    if (right_valid) {
        read_relative(body.right_hand);
    }
    if (left_valid) {
        read_relative(body.left_hand);
    }

    std::uint8_t virtual_count = reader.read_u8();
    if (!reader.ok()) {
        return false;
    }
    if (virtual_count > kMaxVirtualTransforms) {
        virtual_count = static_cast<std::uint8_t>(kMaxVirtualTransforms);
    }
    for (std::uint8_t i = 0; i < virtual_count; ++i) {
        PoseTransform entry;
        read_relative(entry);
        if (!reader.ok()) {
            return false;
        }
        // A count without the VirtualsValid flag is malformed; upstream still
        // consumes the bytes to stay aligned but discards the values.
        if (virtual_valid) {
            body.virtuals.push_back(entry);
        }
    }

    return reader.ok();
}

}  // namespace

std::uint8_t sanitize_pose_flags(std::uint8_t flags) {
    if ((flags & POSE_FLAG_STEALTH) != 0) {
        return POSE_FLAG_STEALTH;
    }
    if ((flags & POSE_FLAG_HEAD_VALID) == 0) {
        flags = static_cast<std::uint8_t>(
            flags & ~(POSE_FLAG_RIGHT_VALID | POSE_FLAG_LEFT_VALID | POSE_FLAG_VIRTUALS_VALID));
    }
    return flags;
}

std::uint8_t compute_encoding_flags(std::uint8_t flags) {
    int encoding_flags = ENCODING_FLAGS_DEFAULT;
    if ((flags & POSE_FLAG_MOVING_FLOOR_LOCAL) != 0) {
        encoding_flags &= ~ENCODING_PHYSICAL_IS_XRORIGIN_DELTA;
    }
    return static_cast<std::uint8_t>(encoding_flags & 0xFF);
}

// --- Client → server --------------------------------------------------------

std::vector<std::uint8_t> serialize_client_hello(const std::string &device_id, bool is_stealth) {
    BinaryWriter writer(8 + device_id.size());
    writer.write_u8(MSG_CLIENT_HELLO);
    writer.write_u8(kProtocolVersion);
    writer.write_u8(is_stealth ? kClientHelloFlagStealth : 0);
    writer.write_string8(device_id);
    return writer.take();
}

std::vector<std::uint8_t> serialize_client_pose(const ClientPoseMessage &message) {
    BinaryWriter writer(256);
    writer.write_u8(MSG_CLIENT_POSE);
    writer.write_u8(kProtocolVersion);
    writer.write_string8(message.device_id);
    write_pose_body(writer, message.body);
    return writer.take();
}

std::vector<std::uint8_t> serialize_stealth_handshake_pose(const std::string &device_id) {
    // Byte-for-byte equivalent of Unity's SerializeStealthHandshakeInto, which
    // writes the *default* encoding byte rather than the derived one. With
    // MovingFloorLocal clear the two are identical.
    BinaryWriter writer(16 + device_id.size());
    writer.write_u8(MSG_CLIENT_POSE);
    writer.write_u8(kProtocolVersion);
    writer.write_string8(device_id);
    writer.write_u16(0);
    writer.write_u8(POSE_FLAG_STEALTH);
    writer.write_u8(ENCODING_FLAGS_DEFAULT);
    writer.write_u8(0);
    return writer.take();
}

std::vector<std::uint8_t> serialize_rpc(const RpcMessage &message) {
    BinaryWriter writer(64 + message.arguments_json.size());
    writer.write_u8(MSG_RPC);
    writer.write_u16(message.sender_client_no);
    writer.write_string8(message.device_id);
    const std::size_t target_count = std::min<std::size_t>(message.target_client_nos.size(), 255);
    writer.write_u8(static_cast<std::uint8_t>(target_count));
    for (std::size_t i = 0; i < target_count; ++i) {
        writer.write_u16(message.target_client_nos[i]);
    }
    writer.write_string8(message.function_name);
    writer.write_string16(message.arguments_json);
    return writer.take();
}

std::vector<std::uint8_t> serialize_global_var_set(const GlobalVarSetMessage &message) {
    BinaryWriter writer(64 + message.variable_value.size());
    writer.write_u8(MSG_GLOBAL_VAR_SET);
    writer.write_u16(message.sender_client_no);
    writer.write_string8(message.device_id);
    writer.write_string8(clamp_variable_name(message.variable_name));
    writer.write_string16(clamp_variable_value(message.variable_value));
    return writer.take();
}

std::vector<std::uint8_t> serialize_client_var_set(const ClientVarSetMessage &message) {
    BinaryWriter writer(64 + message.variable_value.size());
    writer.write_u8(MSG_CLIENT_VAR_SET);
    writer.write_u16(message.sender_client_no);
    writer.write_string8(message.device_id);
    writer.write_u16(message.target_client_no);
    writer.write_string8(clamp_variable_name(message.variable_name));
    writer.write_string16(clamp_variable_value(message.variable_value));
    return writer.take();
}

std::vector<std::uint8_t> serialize_client_var_clear(const ClientVarClearMessage &message) {
    BinaryWriter writer(8 + message.device_id.size());
    writer.write_u8(MSG_CLIENT_VAR_CLEAR);
    writer.write_u16(message.sender_client_no);
    writer.write_string8(message.device_id);
    return writer.take();
}

std::vector<std::uint8_t> serialize_object_pose(const ObjectPoseMessage &message) {
    BinaryWriter writer(32 + message.device_id.size());
    writer.write_u8(MSG_OBJECT_POSE);
    writer.write_u8(kProtocolVersion);
    writer.write_string8(message.device_id);
    writer.write_u32(message.object_id);
    writer.write_u16(message.pose_seq);
    writer.write_i24(quantize_signed_int24(message.position.x, kAbsPosScale));
    writer.write_i24(quantize_signed_int24(message.position.y, kAbsPosScale));
    writer.write_i24(quantize_signed_int24(message.position.z, kAbsPosScale));
    writer.write_u32(compress_quaternion_smallest_three(message.rotation));
    return writer.take();
}

std::vector<std::uint8_t> serialize_object_ownership_request(
    const ObjectOwnershipRequestMessage &message) {
    BinaryWriter writer(16 + message.device_id.size());
    writer.write_u8(MSG_OBJECT_OWNERSHIP_REQUEST);
    writer.write_u8(kProtocolVersion);
    writer.write_string8(message.device_id);
    writer.write_u8(message.operation_type);
    writer.write_u32(message.object_id);
    return writer.take();
}

// --- Server-originated ------------------------------------------------------

std::vector<std::uint8_t> serialize_room_pose(const RoomPoseMessage &message) {
    BinaryWriter writer(256);
    writer.write_u8(MSG_ROOM_POSE);
    writer.write_u8(kProtocolVersion);
    writer.write_string8(message.room_id);
    writer.write_f64(message.broadcast_time);
    writer.write_u16(static_cast<std::uint16_t>(message.clients.size()));
    for (const RoomPoseClient &client : message.clients) {
        writer.write_u16(client.client_no);
        writer.write_f64(client.pose_time);
        write_pose_body(writer, client.body);
    }
    return writer.take();
}

std::vector<std::uint8_t> serialize_device_id_mapping(const DeviceIdMappingMessage &message) {
    BinaryWriter writer(64);
    writer.write_u8(MSG_DEVICE_ID_MAPPING);
    writer.write_u8(message.server_version_major);
    writer.write_u8(message.server_version_minor);
    writer.write_u8(message.server_version_patch);
    writer.write_u16(static_cast<std::uint16_t>(message.mappings.size()));
    for (const DeviceIdMappingEntry &entry : message.mappings) {
        writer.write_u16(entry.client_no);
        writer.write_u8(entry.is_stealth ? 0x01 : 0x00);
        writer.write_string8(entry.device_id);
    }
    return writer.take();
}

std::vector<std::uint8_t> serialize_global_var_sync(const GlobalVarSyncMessage &message) {
    BinaryWriter writer(64);
    writer.write_u8(MSG_GLOBAL_VAR_SYNC);
    writer.write_u16(static_cast<std::uint16_t>(message.variables.size()));
    for (const NetworkVariableEntry &entry : message.variables) {
        writer.write_string8(clamp_variable_name(entry.name));
        writer.write_string16(clamp_variable_value(entry.value));
        writer.write_u16(entry.last_writer_client_no);
    }
    return writer.take();
}

std::vector<std::uint8_t> serialize_client_var_sync(const ClientVarSyncMessage &message) {
    BinaryWriter writer(64);
    writer.write_u8(MSG_CLIENT_VAR_SYNC);
    writer.write_u16(static_cast<std::uint16_t>(message.clients.size()));
    for (const auto &client : message.clients) {
        writer.write_u16(client.first);
        writer.write_u16(static_cast<std::uint16_t>(client.second.size()));
        for (const NetworkVariableEntry &entry : client.second) {
            writer.write_string8(clamp_variable_name(entry.name));
            writer.write_string16(clamp_variable_value(entry.value));
            writer.write_u16(entry.last_writer_client_no);
        }
    }
    return writer.take();
}

std::vector<std::uint8_t> serialize_room_objects(const RoomObjectsMessage &message) {
    BinaryWriter writer(64);
    writer.write_u8(MSG_ROOM_OBJECTS);
    writer.write_u8(kProtocolVersion);
    writer.write_f64(message.broadcast_time);
    writer.write_u16(static_cast<std::uint16_t>(message.objects.size()));
    for (const RoomObjectState &object : message.objects) {
        writer.write_u32(object.object_id);
        writer.write_u16(object.owner_client_no);
        writer.write_u16(object.pose_seq);
        writer.write_f64(object.pose_time);
        writer.write_i24(quantize_signed_int24(object.position.x, kAbsPosScale));
        writer.write_i24(quantize_signed_int24(object.position.y, kAbsPosScale));
        writer.write_i24(quantize_signed_int24(object.position.z, kAbsPosScale));
        writer.write_u32(compress_quaternion_smallest_three(object.rotation));
    }
    return writer.take();
}

std::vector<std::uint8_t> serialize_object_ownership_changed(
    const ObjectOwnershipChangedMessage &message) {
    BinaryWriter writer(16);
    writer.write_u8(MSG_OBJECT_OWNERSHIP_CHANGED);
    writer.write_u32(message.object_id);
    writer.write_u16(message.new_owner_client_no);
    writer.write_u16(message.previous_owner_client_no);
    return writer.take();
}

std::vector<std::uint8_t> serialize_object_ownership_rejected(
    const ObjectOwnershipRejectedMessage &message) {
    BinaryWriter writer(16);
    writer.write_u8(MSG_OBJECT_OWNERSHIP_REJECTED);
    writer.write_u32(message.object_id);
    writer.write_u16(message.current_owner_client_no);
    writer.write_u8(message.reason_code);
    return writer.take();
}

// --- Deserialisation --------------------------------------------------------

std::uint8_t peek_message_type(const std::uint8_t *data, std::size_t size) {
    return (data == nullptr || size == 0) ? 0 : data[0];
}

namespace {

/// Consume the message-type byte and verify it, then optionally the version byte.
bool begin_message(BinaryReader &reader, std::uint8_t expected_type, bool has_version) {
    if (reader.read_u8() != expected_type) {
        return false;
    }
    if (has_version && reader.read_u8() != kProtocolVersion) {
        return false;
    }
    return reader.ok();
}

}  // namespace

bool deserialize_client_hello(const std::uint8_t *data, std::size_t size,
                              ClientHelloMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_CLIENT_HELLO, true)) {
        return false;
    }
    out.flags = reader.read_u8();
    out.device_id = reader.read_string8();
    return reader.ok();
}

bool deserialize_client_pose(const std::uint8_t *data, std::size_t size, ClientPoseMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_CLIENT_POSE, true)) {
        return false;
    }
    out.device_id = reader.read_string8();
    if (!reader.ok()) {
        return false;
    }
    return read_pose_body(reader, out.body);
}

bool deserialize_room_pose(const std::uint8_t *data, std::size_t size, RoomPoseMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_ROOM_POSE, true)) {
        return false;
    }
    out.room_id = reader.read_string8();
    out.broadcast_time = reader.read_f64();
    const std::uint16_t client_count = reader.read_u16();
    if (!reader.ok()) {
        return false;
    }
    out.clients.clear();
    out.clients.reserve(client_count);
    for (std::uint16_t i = 0; i < client_count; ++i) {
        RoomPoseClient client;
        client.client_no = reader.read_u16();
        client.pose_time = reader.read_f64();
        if (!reader.ok() || !read_pose_body(reader, client.body)) {
            return false;
        }
        out.clients.push_back(std::move(client));
    }
    return reader.ok();
}

bool deserialize_rpc(const std::uint8_t *data, std::size_t size, RpcMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_RPC, false)) {
        return false;
    }
    out.sender_client_no = reader.read_u16();
    out.device_id = reader.read_string8();
    const std::uint8_t target_count = reader.read_u8();
    if (!reader.ok()) {
        return false;
    }
    out.target_client_nos.clear();
    out.target_client_nos.reserve(target_count);
    for (std::uint8_t i = 0; i < target_count; ++i) {
        out.target_client_nos.push_back(reader.read_u16());
    }
    out.function_name = reader.read_string8();
    out.arguments_json = reader.read_string16();
    return reader.ok();
}

bool deserialize_device_id_mapping(const std::uint8_t *data, std::size_t size,
                                   DeviceIdMappingMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_DEVICE_ID_MAPPING, false)) {
        return false;
    }
    out.server_version_major = reader.read_u8();
    out.server_version_minor = reader.read_u8();
    out.server_version_patch = reader.read_u8();
    const std::uint16_t count = reader.read_u16();
    if (!reader.ok()) {
        return false;
    }
    out.mappings.clear();
    out.mappings.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        DeviceIdMappingEntry entry;
        entry.client_no = reader.read_u16();
        entry.is_stealth = reader.read_u8() == 0x01;
        entry.device_id = reader.read_string8();
        if (!reader.ok()) {
            return false;
        }
        out.mappings.push_back(std::move(entry));
    }
    return reader.ok();
}

bool deserialize_global_var_set(const std::uint8_t *data, std::size_t size,
                                GlobalVarSetMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_GLOBAL_VAR_SET, false)) {
        return false;
    }
    out.sender_client_no = reader.read_u16();
    out.device_id = reader.read_string8();
    out.variable_name = reader.read_string8();
    out.variable_value = reader.read_string16();
    return reader.ok();
}

bool deserialize_global_var_sync(const std::uint8_t *data, std::size_t size,
                                 GlobalVarSyncMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_GLOBAL_VAR_SYNC, false)) {
        return false;
    }
    const std::uint16_t count = reader.read_u16();
    if (!reader.ok()) {
        return false;
    }
    out.variables.clear();
    out.variables.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        NetworkVariableEntry entry;
        entry.name = reader.read_string8();
        entry.value = reader.read_string16();
        entry.last_writer_client_no = reader.read_u16();
        if (!reader.ok()) {
            return false;
        }
        out.variables.push_back(std::move(entry));
    }
    return reader.ok();
}

bool deserialize_client_var_set(const std::uint8_t *data, std::size_t size,
                                ClientVarSetMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_CLIENT_VAR_SET, false)) {
        return false;
    }
    out.sender_client_no = reader.read_u16();
    out.device_id = reader.read_string8();
    out.target_client_no = reader.read_u16();
    out.variable_name = reader.read_string8();
    out.variable_value = reader.read_string16();
    return reader.ok();
}

bool deserialize_client_var_sync(const std::uint8_t *data, std::size_t size,
                                 ClientVarSyncMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_CLIENT_VAR_SYNC, false)) {
        return false;
    }
    const std::uint16_t client_count = reader.read_u16();
    if (!reader.ok()) {
        return false;
    }
    out.clients.clear();
    out.clients.reserve(client_count);
    for (std::uint16_t i = 0; i < client_count; ++i) {
        const std::uint16_t client_no = reader.read_u16();
        const std::uint16_t var_count = reader.read_u16();
        if (!reader.ok()) {
            return false;
        }
        std::vector<NetworkVariableEntry> entries;
        entries.reserve(var_count);
        for (std::uint16_t j = 0; j < var_count; ++j) {
            NetworkVariableEntry entry;
            entry.name = reader.read_string8();
            entry.value = reader.read_string16();
            entry.last_writer_client_no = reader.read_u16();
            if (!reader.ok()) {
                return false;
            }
            entries.push_back(std::move(entry));
        }
        out.clients.emplace_back(client_no, std::move(entries));
    }
    return reader.ok();
}

bool deserialize_client_var_clear(const std::uint8_t *data, std::size_t size,
                                  ClientVarClearMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_CLIENT_VAR_CLEAR, false)) {
        return false;
    }
    out.sender_client_no = reader.read_u16();
    out.device_id = reader.read_string8();
    return reader.ok();
}

bool deserialize_object_pose(const std::uint8_t *data, std::size_t size, ObjectPoseMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_OBJECT_POSE, true)) {
        return false;
    }
    out.device_id = reader.read_string8();
    out.object_id = reader.read_u32();
    out.pose_seq = reader.read_u16();
    const std::int32_t px = reader.read_i24();
    const std::int32_t py = reader.read_i24();
    const std::int32_t pz = reader.read_i24();
    const std::uint32_t packed = reader.read_u32();
    if (!reader.ok()) {
        return false;
    }
    out.position = Vec3(dequantize_signed(px, kAbsPosScale), dequantize_signed(py, kAbsPosScale),
                        dequantize_signed(pz, kAbsPosScale));
    out.rotation = decompress_quaternion_smallest_three(packed);
    return true;
}

bool deserialize_room_objects(const std::uint8_t *data, std::size_t size,
                              RoomObjectsMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_ROOM_OBJECTS, true)) {
        return false;
    }
    out.broadcast_time = reader.read_f64();
    const std::uint16_t object_count = reader.read_u16();
    if (!reader.ok()) {
        return false;
    }
    out.objects.clear();
    out.objects.reserve(object_count);
    for (std::uint16_t i = 0; i < object_count; ++i) {
        RoomObjectState object;
        object.object_id = reader.read_u32();
        object.owner_client_no = reader.read_u16();
        object.pose_seq = reader.read_u16();
        object.pose_time = reader.read_f64();
        const std::int32_t px = reader.read_i24();
        const std::int32_t py = reader.read_i24();
        const std::int32_t pz = reader.read_i24();
        const std::uint32_t packed = reader.read_u32();
        if (!reader.ok()) {
            return false;
        }
        object.position =
            Vec3(dequantize_signed(px, kAbsPosScale), dequantize_signed(py, kAbsPosScale),
                 dequantize_signed(pz, kAbsPosScale));
        object.rotation = decompress_quaternion_smallest_three(packed);
        out.objects.push_back(std::move(object));
    }
    return reader.ok();
}

bool deserialize_object_ownership_request(const std::uint8_t *data, std::size_t size,
                                          ObjectOwnershipRequestMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_OBJECT_OWNERSHIP_REQUEST, true)) {
        return false;
    }
    out.device_id = reader.read_string8();
    out.operation_type = reader.read_u8();
    out.object_id = reader.read_u32();
    return reader.ok();
}

bool deserialize_object_ownership_changed(const std::uint8_t *data, std::size_t size,
                                          ObjectOwnershipChangedMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_OBJECT_OWNERSHIP_CHANGED, false)) {
        return false;
    }
    out.object_id = reader.read_u32();
    out.new_owner_client_no = reader.read_u16();
    out.previous_owner_client_no = reader.read_u16();
    return reader.ok();
}

bool deserialize_object_ownership_rejected(const std::uint8_t *data, std::size_t size,
                                           ObjectOwnershipRejectedMessage &out) {
    BinaryReader reader(data, size);
    if (!begin_message(reader, MSG_OBJECT_OWNERSHIP_REJECTED, false)) {
        return false;
    }
    out.object_id = reader.read_u32();
    out.current_owner_client_no = reader.read_u16();
    out.reason_code = reader.read_u8();
    return reader.ok();
}

}  // namespace netsync
}  // namespace styly
