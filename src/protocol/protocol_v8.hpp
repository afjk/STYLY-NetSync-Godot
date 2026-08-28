// SPDX-License-Identifier: Apache-2.0
// STYLY NetSync protocol v8 message structures and (de)serialisers.
//
// Pure data + byte manipulation. No Godot, no ZeroMQ, no threading.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "binary_reader.hpp"
#include "binary_writer.hpp"
#include "message_types.hpp"
#include "pose_codec.hpp"

namespace styly {
namespace netsync {

/// Position + rotation in wire (NetSync/Unity) coordinates.
struct PoseTransform {
    Vec3 position;
    Quat rotation;
};

/// The pose body shared by MSG_CLIENT_POSE and each record of MSG_ROOM_POSE.
struct ClientPoseBody {
    std::uint16_t pose_seq = 0;
    /// Pose flags as supplied by the caller; sanitised during serialisation.
    std::uint8_t flags = 0;
    /// Populated on deserialisation. Ignored on serialisation (always derived).
    std::uint8_t encoding_flags = 0;

    Vec3 xr_origin_delta_position;
    double xr_origin_delta_yaw = 0.0;

    PoseTransform physical;
    PoseTransform head;
    PoseTransform right_hand;
    PoseTransform left_hand;
    std::vector<PoseTransform> virtuals;
};

struct ClientHelloMessage {
    std::string device_id;
    std::uint8_t flags = 0;

    bool is_stealth() const { return (flags & kClientHelloFlagStealth) != 0; }
};

struct ClientPoseMessage {
    std::string device_id;
    ClientPoseBody body;
};

struct RoomPoseClient {
    std::uint16_t client_no = 0;
    double pose_time = 0.0;
    ClientPoseBody body;
};

struct RoomPoseMessage {
    std::string room_id;
    double broadcast_time = 0.0;
    std::vector<RoomPoseClient> clients;
};

struct RpcMessage {
    std::uint16_t sender_client_no = 0;
    std::string device_id;
    /// Empty means "broadcast to the whole room" (the sender included).
    std::vector<std::uint16_t> target_client_nos;
    std::string function_name;
    /// JSON array of JSON strings, e.g. `["a","b"]`.
    std::string arguments_json;
};

struct DeviceIdMappingEntry {
    std::uint16_t client_no = 0;
    std::string device_id;
    bool is_stealth = false;
};

struct DeviceIdMappingMessage {
    std::uint8_t server_version_major = 0;
    std::uint8_t server_version_minor = 0;
    std::uint8_t server_version_patch = 0;
    std::vector<DeviceIdMappingEntry> mappings;
};

struct GlobalVarSetMessage {
    std::uint16_t sender_client_no = 0;
    std::string device_id;
    std::string variable_name;
    std::string variable_value;
};

struct ClientVarSetMessage {
    std::uint16_t sender_client_no = 0;
    std::string device_id;
    std::uint16_t target_client_no = 0;
    std::string variable_name;
    std::string variable_value;
};

struct ClientVarClearMessage {
    std::uint16_t sender_client_no = 0;
    std::string device_id;
};

struct NetworkVariableEntry {
    std::string name;
    std::string value;
    std::uint16_t last_writer_client_no = 0;
};

struct GlobalVarSyncMessage {
    std::vector<NetworkVariableEntry> variables;
};

struct ClientVarSyncMessage {
    /// One authoritative snapshot per client number, in wire order.
    std::vector<std::pair<std::uint16_t, std::vector<NetworkVariableEntry>>> clients;
};

struct ObjectPoseMessage {
    std::string device_id;
    std::uint32_t object_id = 0;
    std::uint16_t pose_seq = 0;
    Vec3 position;
    Quat rotation;
};

struct RoomObjectState {
    std::uint32_t object_id = 0;
    std::uint16_t owner_client_no = 0;
    std::uint16_t pose_seq = 0;
    double pose_time = 0.0;
    Vec3 position;
    Quat rotation;
};

struct RoomObjectsMessage {
    double broadcast_time = 0.0;
    std::vector<RoomObjectState> objects;
};

struct ObjectOwnershipRequestMessage {
    std::string device_id;
    std::uint8_t operation_type = 0;
    std::uint32_t object_id = 0;
};

struct ObjectOwnershipChangedMessage {
    std::uint32_t object_id = 0;
    std::uint16_t new_owner_client_no = 0;
    std::uint16_t previous_owner_client_no = 0;
};

struct ObjectOwnershipRejectedMessage {
    std::uint32_t object_id = 0;
    std::uint16_t current_owner_client_no = 0;
    std::uint8_t reason_code = 0;
};

// --- Flag helpers -----------------------------------------------------------

/// Apply the sender-side flag sanitisation, exactly as the Python reference does:
/// stealth collapses to `IsStealth` alone; without `HeadValid` the head-relative
/// bits are cleared.
std::uint8_t sanitize_pose_flags(std::uint8_t flags);

/// Derive the encoding-flags byte from sanitised pose flags.
std::uint8_t compute_encoding_flags(std::uint8_t flags);

// --- Serialisation (client → server) ---------------------------------------

std::vector<std::uint8_t> serialize_client_hello(const std::string &device_id, bool is_stealth);
std::vector<std::uint8_t> serialize_client_pose(const ClientPoseMessage &message);
std::vector<std::uint8_t> serialize_stealth_handshake_pose(const std::string &device_id);
std::vector<std::uint8_t> serialize_rpc(const RpcMessage &message);
std::vector<std::uint8_t> serialize_global_var_set(const GlobalVarSetMessage &message);
std::vector<std::uint8_t> serialize_client_var_set(const ClientVarSetMessage &message);
std::vector<std::uint8_t> serialize_client_var_clear(const ClientVarClearMessage &message);
std::vector<std::uint8_t> serialize_object_pose(const ObjectPoseMessage &message);
std::vector<std::uint8_t> serialize_object_ownership_request(
    const ObjectOwnershipRequestMessage &message);

// --- Serialisation of server-originated messages ---------------------------
// Not used by the client at runtime; present so tests can round-trip and so a
// loopback/offline harness can synthesise server traffic.

std::vector<std::uint8_t> serialize_room_pose(const RoomPoseMessage &message);
std::vector<std::uint8_t> serialize_device_id_mapping(const DeviceIdMappingMessage &message);
std::vector<std::uint8_t> serialize_global_var_sync(const GlobalVarSyncMessage &message);
std::vector<std::uint8_t> serialize_client_var_sync(const ClientVarSyncMessage &message);
std::vector<std::uint8_t> serialize_room_objects(const RoomObjectsMessage &message);
std::vector<std::uint8_t> serialize_object_ownership_changed(
    const ObjectOwnershipChangedMessage &message);
std::vector<std::uint8_t> serialize_object_ownership_rejected(
    const ObjectOwnershipRejectedMessage &message);

// --- Deserialisation --------------------------------------------------------
// Each returns false when the payload is truncated, carries an unsupported
// protocol version, or violates a structural invariant that upstream rejects.

/// Message type byte, or 0 when the payload is empty.
std::uint8_t peek_message_type(const std::uint8_t *data, std::size_t size);

bool deserialize_client_hello(const std::uint8_t *data, std::size_t size,
                              ClientHelloMessage &out);
bool deserialize_client_pose(const std::uint8_t *data, std::size_t size, ClientPoseMessage &out);
bool deserialize_room_pose(const std::uint8_t *data, std::size_t size, RoomPoseMessage &out);
bool deserialize_rpc(const std::uint8_t *data, std::size_t size, RpcMessage &out);
bool deserialize_device_id_mapping(const std::uint8_t *data, std::size_t size,
                                   DeviceIdMappingMessage &out);
bool deserialize_global_var_set(const std::uint8_t *data, std::size_t size,
                                GlobalVarSetMessage &out);
bool deserialize_global_var_sync(const std::uint8_t *data, std::size_t size,
                                 GlobalVarSyncMessage &out);
bool deserialize_client_var_set(const std::uint8_t *data, std::size_t size,
                                ClientVarSetMessage &out);
bool deserialize_client_var_sync(const std::uint8_t *data, std::size_t size,
                                 ClientVarSyncMessage &out);
bool deserialize_client_var_clear(const std::uint8_t *data, std::size_t size,
                                  ClientVarClearMessage &out);
bool deserialize_object_pose(const std::uint8_t *data, std::size_t size, ObjectPoseMessage &out);
bool deserialize_room_objects(const std::uint8_t *data, std::size_t size,
                              RoomObjectsMessage &out);
bool deserialize_object_ownership_request(const std::uint8_t *data, std::size_t size,
                                          ObjectOwnershipRequestMessage &out);
bool deserialize_object_ownership_changed(const std::uint8_t *data, std::size_t size,
                                          ObjectOwnershipChangedMessage &out);
bool deserialize_object_ownership_rejected(const std::uint8_t *data, std::size_t size,
                                           ObjectOwnershipRejectedMessage &out);

}  // namespace netsync
}  // namespace styly
