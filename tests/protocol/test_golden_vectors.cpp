// SPDX-License-Identifier: Apache-2.0
//
// Golden binary compatibility: for every case in tests/golden/vectors.json the
// bytes produced by this implementation must equal, byte for byte, the bytes
// produced by the upstream STYLY-NetSync Python serializer for the same input.
//
// Server-originated messages are additionally decoded and their fields checked,
// so both directions of every message type are covered.

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "../golden_loader.hpp"
#include "../test_support.hpp"
#include "core/json_util.hpp"
#include "protocol/protocol_v8.hpp"

using namespace styly::netsync;
using namespace styly::netsync::test;

namespace {

/// Serialise one case, dispatching on its declared message type.
/// Returns false when the case declares a type this test does not encode.
bool encode_case(const std::string &message, const JsonValue &input,
                 std::vector<std::uint8_t> &out) {
    if (message == "client_hello") {
        out = serialize_client_hello(input["device_id"].string_value(),
                                     input["is_stealth"].bool_value());
        return true;
    }
    if (message == "client_pose") {
        ClientPoseMessage pose;
        pose.device_id = input["device_id"].string_value();
        pose.body = body_from_json(input["body"]);
        out = serialize_client_pose(pose);
        return true;
    }
    if (message == "room_pose") {
        RoomPoseMessage room;
        room.room_id = input["room_id"].string_value();
        room.broadcast_time = input["broadcast_time"].number_value();
        for (const JsonValue &entry : input["clients"].array_items()) {
            RoomPoseClient client;
            client.client_no = static_cast<std::uint16_t>(entry["client_no"].int_value());
            client.pose_time = entry["pose_time"].number_value();
            client.body = body_from_json(entry["body"]);
            room.clients.push_back(client);
        }
        out = serialize_room_pose(room);
        return true;
    }
    if (message == "rpc") {
        RpcMessage rpc;
        rpc.sender_client_no = static_cast<std::uint16_t>(input["sender_client_no"].int_value());
        rpc.device_id = input["device_id"].string_value();
        for (const JsonValue &target : input["target_client_nos"].array_items()) {
            rpc.target_client_nos.push_back(static_cast<std::uint16_t>(target.int_value()));
        }
        rpc.function_name = input["function_name"].string_value();
        rpc.arguments_json = input["arguments_json"].string_value();
        out = serialize_rpc(rpc);
        return true;
    }
    if (message == "device_id_mapping") {
        DeviceIdMappingMessage mapping;
        const JsonValue &version = input["server_version"];
        mapping.server_version_major =
            static_cast<std::uint8_t>(version[static_cast<std::size_t>(0)].int_value());
        mapping.server_version_minor =
            static_cast<std::uint8_t>(version[static_cast<std::size_t>(1)].int_value());
        mapping.server_version_patch =
            static_cast<std::uint8_t>(version[static_cast<std::size_t>(2)].int_value());
        for (const JsonValue &entry : input["mappings"].array_items()) {
            DeviceIdMappingEntry item;
            item.client_no = static_cast<std::uint16_t>(entry["client_no"].int_value());
            item.device_id = entry["device_id"].string_value();
            item.is_stealth = entry["is_stealth"].bool_value();
            mapping.mappings.push_back(item);
        }
        out = serialize_device_id_mapping(mapping);
        return true;
    }
    if (message == "global_var_set") {
        GlobalVarSetMessage set;
        set.sender_client_no = static_cast<std::uint16_t>(input["sender_client_no"].int_value());
        set.device_id = input["device_id"].string_value();
        set.variable_name = input["variable_name"].string_value();
        set.variable_value = input["variable_value"].string_value();
        out = serialize_global_var_set(set);
        return true;
    }
    if (message == "client_var_set") {
        ClientVarSetMessage set;
        set.sender_client_no = static_cast<std::uint16_t>(input["sender_client_no"].int_value());
        set.device_id = input["device_id"].string_value();
        set.target_client_no = static_cast<std::uint16_t>(input["target_client_no"].int_value());
        set.variable_name = input["variable_name"].string_value();
        set.variable_value = input["variable_value"].string_value();
        out = serialize_client_var_set(set);
        return true;
    }
    if (message == "client_var_clear") {
        ClientVarClearMessage clear;
        clear.sender_client_no = static_cast<std::uint16_t>(input["sender_client_no"].int_value());
        clear.device_id = input["device_id"].string_value();
        out = serialize_client_var_clear(clear);
        return true;
    }
    if (message == "global_var_sync") {
        GlobalVarSyncMessage sync;
        for (const JsonValue &entry : input["variables"].array_items()) {
            sync.variables.push_back(variable_from_json(entry));
        }
        out = serialize_global_var_sync(sync);
        return true;
    }
    if (message == "client_var_sync") {
        ClientVarSyncMessage sync;
        for (const JsonValue &entry : input["clients"].array_items()) {
            std::vector<NetworkVariableEntry> variables;
            for (const JsonValue &variable : entry["variables"].array_items()) {
                variables.push_back(variable_from_json(variable));
            }
            sync.clients.emplace_back(static_cast<std::uint16_t>(entry["client_no"].int_value()),
                                      variables);
        }
        out = serialize_client_var_sync(sync);
        return true;
    }
    if (message == "object_pose") {
        ObjectPoseMessage pose;
        pose.device_id = input["device_id"].string_value();
        pose.object_id = static_cast<std::uint32_t>(input["object_id"].int_value());
        pose.pose_seq = static_cast<std::uint16_t>(input["pose_seq"].int_value());
        pose.position = vec3_from_json(input["position"]);
        pose.rotation = quat_from_json(input["rotation"]);
        out = serialize_object_pose(pose);
        return true;
    }
    if (message == "room_objects") {
        RoomObjectsMessage objects;
        objects.broadcast_time = input["broadcast_time"].number_value();
        for (const JsonValue &entry : input["objects"].array_items()) {
            RoomObjectState state;
            state.object_id = static_cast<std::uint32_t>(entry["object_id"].int_value());
            state.owner_client_no =
                static_cast<std::uint16_t>(entry["owner_client_no"].int_value());
            state.pose_seq = static_cast<std::uint16_t>(entry["pose_seq"].int_value());
            state.pose_time = entry["pose_time"].number_value();
            state.position = vec3_from_json(entry["position"]);
            state.rotation = quat_from_json(entry["rotation"]);
            objects.objects.push_back(state);
        }
        out = serialize_room_objects(objects);
        return true;
    }
    if (message == "object_ownership_request") {
        ObjectOwnershipRequestMessage request;
        request.device_id = input["device_id"].string_value();
        request.operation_type = static_cast<std::uint8_t>(input["operation_type"].int_value());
        request.object_id = static_cast<std::uint32_t>(input["object_id"].int_value());
        out = serialize_object_ownership_request(request);
        return true;
    }
    if (message == "ownership_changed") {
        ObjectOwnershipChangedMessage changed;
        changed.object_id = static_cast<std::uint32_t>(input["object_id"].int_value());
        changed.new_owner_client_no =
            static_cast<std::uint16_t>(input["new_owner_client_no"].int_value());
        changed.previous_owner_client_no =
            static_cast<std::uint16_t>(input["previous_owner_client_no"].int_value());
        out = serialize_object_ownership_changed(changed);
        return true;
    }
    if (message == "ownership_rejected") {
        ObjectOwnershipRejectedMessage rejected;
        rejected.object_id = static_cast<std::uint32_t>(input["object_id"].int_value());
        rejected.current_owner_client_no =
            static_cast<std::uint16_t>(input["current_owner_client_no"].int_value());
        rejected.reason_code = static_cast<std::uint8_t>(input["reason_code"].int_value());
        out = serialize_object_ownership_rejected(rejected);
        return true;
    }
    return false;
}

/// Decode the golden bytes and check that the recovered fields match the input.
/// Quantised fields are compared with the tolerance implied by their scale.
void check_decode(const std::string &name, const std::string &message, const JsonValue &input,
                  const std::vector<std::uint8_t> &bytes) {
    const std::uint8_t *data = bytes.data();
    const std::size_t size = bytes.size();

    if (message == "client_hello") {
        ClientHelloMessage decoded;
        CHECK_MSG(deserialize_client_hello(data, size, decoded), name);
        CHECK_EQ(decoded.is_stealth(), input["is_stealth"].bool_value());
        return;
    }
    if (message == "device_id_mapping") {
        DeviceIdMappingMessage decoded;
        CHECK_MSG(deserialize_device_id_mapping(data, size, decoded), name);
        CHECK_EQ(decoded.mappings.size(), input["mappings"].array_items().size());
        for (std::size_t i = 0; i < decoded.mappings.size(); ++i) {
            const JsonValue &expected = input["mappings"][i];
            CHECK_EQ(decoded.mappings[i].client_no,
                     static_cast<std::uint16_t>(expected["client_no"].int_value()));
            CHECK_EQ(decoded.mappings[i].device_id, expected["device_id"].string_value());
            CHECK_EQ(decoded.mappings[i].is_stealth, expected["is_stealth"].bool_value());
        }
        return;
    }
    if (message == "rpc") {
        RpcMessage decoded;
        CHECK_MSG(deserialize_rpc(data, size, decoded), name);
        CHECK_EQ(decoded.sender_client_no,
                 static_cast<std::uint16_t>(input["sender_client_no"].int_value()));
        CHECK_EQ(decoded.device_id, input["device_id"].string_value());
        CHECK_EQ(decoded.function_name, input["function_name"].string_value());
        CHECK_EQ(decoded.arguments_json, input["arguments_json"].string_value());
        CHECK_EQ(decoded.target_client_nos.size(),
                 input["target_client_nos"].array_items().size());
        // Round-trip the argument array through the JSON codec.
        std::vector<std::string> arguments;
        CHECK_MSG(decode_json_string_array(decoded.arguments_json, arguments), name);
        CHECK_EQ(arguments.size(), input["arguments"].array_items().size());
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            CHECK_EQ(arguments[i], input["arguments"][i].string_value());
        }
        return;
    }
    if (message == "global_var_sync") {
        GlobalVarSyncMessage decoded;
        CHECK_MSG(deserialize_global_var_sync(data, size, decoded), name);
        CHECK_EQ(decoded.variables.size(), input["variables"].array_items().size());
        for (std::size_t i = 0; i < decoded.variables.size(); ++i) {
            CHECK_EQ(decoded.variables[i].name, input["variables"][i]["name"].string_value());
            CHECK_EQ(decoded.variables[i].value, input["variables"][i]["value"].string_value());
            CHECK_EQ(decoded.variables[i].last_writer_client_no,
                     static_cast<std::uint16_t>(
                         input["variables"][i]["last_writer_client_no"].int_value()));
        }
        return;
    }
    if (message == "client_var_sync") {
        ClientVarSyncMessage decoded;
        CHECK_MSG(deserialize_client_var_sync(data, size, decoded), name);
        CHECK_EQ(decoded.clients.size(), input["clients"].array_items().size());
        for (std::size_t i = 0; i < decoded.clients.size(); ++i) {
            CHECK_EQ(decoded.clients[i].first,
                     static_cast<std::uint16_t>(input["clients"][i]["client_no"].int_value()));
            CHECK_EQ(decoded.clients[i].second.size(),
                     input["clients"][i]["variables"].array_items().size());
        }
        return;
    }
    if (message == "room_pose") {
        RoomPoseMessage decoded;
        CHECK_MSG(deserialize_room_pose(data, size, decoded), name);
        CHECK_EQ(decoded.room_id, input["room_id"].string_value());
        CHECK_EQ(decoded.broadcast_time, input["broadcast_time"].number_value());
        CHECK_EQ(decoded.clients.size(), input["clients"].array_items().size());
        for (std::size_t i = 0; i < decoded.clients.size(); ++i) {
            const JsonValue &expected = input["clients"][i];
            CHECK_EQ(decoded.clients[i].client_no,
                     static_cast<std::uint16_t>(expected["client_no"].int_value()));
            CHECK_EQ(decoded.clients[i].pose_time, expected["pose_time"].number_value());
            const ClientPoseBody expected_body = body_from_json(expected["body"]);
            const std::uint8_t expected_flags = sanitize_pose_flags(expected_body.flags);
            CHECK_EQ(decoded.clients[i].body.flags, expected_flags);
            CHECK_EQ(decoded.clients[i].body.pose_seq, expected_body.pose_seq);
            if ((expected_flags & POSE_FLAG_HEAD_VALID) != 0) {
                // Head position is quantised at 0.01 m, so half a step is the bound.
                CHECK_NEAR(decoded.clients[i].body.head.position.x,
                           expected_body.head.position.x, kAbsPosScale);
                CHECK_NEAR(decoded.clients[i].body.head.position.y,
                           expected_body.head.position.y, kAbsPosScale);
                CHECK_NEAR(decoded.clients[i].body.head.position.z,
                           expected_body.head.position.z, kAbsPosScale);
            }
        }
        return;
    }
    if (message == "room_objects") {
        RoomObjectsMessage decoded;
        CHECK_MSG(deserialize_room_objects(data, size, decoded), name);
        CHECK_EQ(decoded.broadcast_time, input["broadcast_time"].number_value());
        CHECK_EQ(decoded.objects.size(), input["objects"].array_items().size());
        for (std::size_t i = 0; i < decoded.objects.size(); ++i) {
            const JsonValue &expected = input["objects"][i];
            CHECK_EQ(decoded.objects[i].object_id,
                     static_cast<std::uint32_t>(expected["object_id"].int_value()));
            CHECK_EQ(decoded.objects[i].owner_client_no,
                     static_cast<std::uint16_t>(expected["owner_client_no"].int_value()));
            CHECK_EQ(decoded.objects[i].pose_seq,
                     static_cast<std::uint16_t>(expected["pose_seq"].int_value()));
            CHECK_EQ(decoded.objects[i].pose_time, expected["pose_time"].number_value());
            const Vec3 position = vec3_from_json(expected["position"]);
            CHECK_NEAR(decoded.objects[i].position.x, position.x, kAbsPosScale);
            CHECK_NEAR(decoded.objects[i].position.y, position.y, kAbsPosScale);
            CHECK_NEAR(decoded.objects[i].position.z, position.z, kAbsPosScale);
        }
        return;
    }
    if (message == "ownership_changed") {
        ObjectOwnershipChangedMessage decoded;
        CHECK_MSG(deserialize_object_ownership_changed(data, size, decoded), name);
        CHECK_EQ(decoded.object_id, static_cast<std::uint32_t>(input["object_id"].int_value()));
        CHECK_EQ(decoded.new_owner_client_no,
                 static_cast<std::uint16_t>(input["new_owner_client_no"].int_value()));
        CHECK_EQ(decoded.previous_owner_client_no,
                 static_cast<std::uint16_t>(input["previous_owner_client_no"].int_value()));
        return;
    }
    if (message == "ownership_rejected") {
        ObjectOwnershipRejectedMessage decoded;
        CHECK_MSG(deserialize_object_ownership_rejected(data, size, decoded), name);
        CHECK_EQ(decoded.object_id, static_cast<std::uint32_t>(input["object_id"].int_value()));
        CHECK_EQ(decoded.current_owner_client_no,
                 static_cast<std::uint16_t>(input["current_owner_client_no"].int_value()));
        CHECK_EQ(decoded.reason_code, static_cast<std::uint8_t>(input["reason_code"].int_value()));
        return;
    }
    if (message == "client_pose") {
        ClientPoseMessage decoded;
        CHECK_MSG(deserialize_client_pose(data, size, decoded), name);
        CHECK_EQ(decoded.device_id,
                 truncate_utf8_bytes(input["device_id"].string_value(), 255));
        return;
    }
    if (message == "object_pose") {
        ObjectPoseMessage decoded;
        CHECK_MSG(deserialize_object_pose(data, size, decoded), name);
        CHECK_EQ(decoded.object_id, static_cast<std::uint32_t>(input["object_id"].int_value()));
        CHECK_EQ(decoded.pose_seq, static_cast<std::uint16_t>(input["pose_seq"].int_value()));
        return;
    }
    if (message == "object_ownership_request") {
        ObjectOwnershipRequestMessage decoded;
        CHECK_MSG(deserialize_object_ownership_request(data, size, decoded), name);
        CHECK_EQ(decoded.device_id, input["device_id"].string_value());
        CHECK_EQ(decoded.operation_type,
                 static_cast<std::uint8_t>(input["operation_type"].int_value()));
        CHECK_EQ(decoded.object_id, static_cast<std::uint32_t>(input["object_id"].int_value()));
        return;
    }
    if (message == "global_var_set" || message == "client_var_set" ||
        message == "client_var_clear") {
        // Client → server messages: the server decodes these, so verify our own
        // decoder agrees with our encoder on the truncated field values.
        if (message == "client_var_clear") {
            ClientVarClearMessage decoded;
            CHECK_MSG(deserialize_client_var_clear(data, size, decoded), name);
            CHECK_EQ(decoded.device_id, input["device_id"].string_value());
        } else if (message == "global_var_set") {
            GlobalVarSetMessage decoded;
            CHECK_MSG(deserialize_global_var_set(data, size, decoded), name);
            CHECK_EQ(decoded.variable_name,
                     truncate_utf8_code_points(input["variable_name"].string_value(),
                                               kMaxVariableNameChars));
        } else {
            ClientVarSetMessage decoded;
            CHECK_MSG(deserialize_client_var_set(data, size, decoded), name);
            CHECK_EQ(decoded.target_client_no,
                     static_cast<std::uint16_t>(input["target_client_no"].int_value()));
        }
        return;
    }
}

void test_quaternion_vectors(const JsonValue &document) {
    const std::vector<JsonValue> &vectors = document["quaternion_vectors"].array_items();
    CHECK(!vectors.empty());
    std::size_t mismatches = 0;
    for (const JsonValue &entry : vectors) {
        const Quat q = quat_from_json(entry["q"]);
        const std::uint32_t expected = static_cast<std::uint32_t>(entry["packed"].int_value());
        const std::uint32_t actual = compress_quaternion_smallest_three(q);
        if (actual != expected) {
            if (mismatches < 5) {
                char buffer[256];
                std::snprintf(buffer, sizeof(buffer),
                              "quaternion pack mismatch for (%.17g, %.17g, %.17g, %.17g): "
                              "0x%08x != 0x%08x",
                              q.x, q.y, q.z, q.w, actual, expected);
                report_failure(__FILE__, __LINE__, buffer);
            }
            ++mismatches;
        }
        ++check_count();
    }
    if (mismatches > 5) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "... and %zu further quaternion mismatches",
                      mismatches - 5);
        report_failure(__FILE__, __LINE__, buffer);
    }
    std::printf("  quaternion vectors checked: %zu\n", vectors.size());
}

void test_quantization_vectors(const JsonValue &document) {
    const std::vector<JsonValue> &vectors = document["quantization_vectors"].array_items();
    CHECK(!vectors.empty());
    for (const JsonValue &entry : vectors) {
        const std::string scale_name = entry["scale"].string_value();
        double scale = kAbsPosScale;
        if (scale_name == "rel") {
            scale = kRelPosScale;
        } else if (scale_name == "loco") {
            scale = kLocoPosScale;
        } else if (scale_name == "yaw") {
            scale = kPhysicalYawScale;
        }
        const double value = entry["value"].number_value();
        CHECK_EQ(static_cast<std::int64_t>(quantize_signed(value, scale)),
                 entry["i16"].int_value());
        CHECK_EQ(static_cast<std::int64_t>(quantize_signed_int24(value, scale)),
                 entry["i24"].int_value());
    }
    std::printf("  quantisation vectors checked: %zu\n", vectors.size());
}

}  // namespace

int main(int argc, char **argv) {
    JsonValue document;
    std::string path;
    if (!load_golden(argc, argv, document, path)) {
        return 2;
    }
    std::printf("golden vectors: %s (upstream %s)\n", path.c_str(),
                document["upstream_commit"].string_value().c_str());

    CHECK_EQ(document["protocol_version"].int_value(), static_cast<std::int64_t>(kProtocolVersion));

    test_quaternion_vectors(document);
    test_quantization_vectors(document);

    std::size_t encoded = 0;
    std::size_t decoded = 0;
    for (const JsonValue &entry : document["cases"].array_items()) {
        const std::string name = entry["name"].string_value();
        const std::string message = entry["message"].string_value();
        const JsonValue &input = entry["input"];

        std::vector<std::uint8_t> actual;
        if (!encode_case(message, input, actual)) {
            report_failure(__FILE__, __LINE__, "unhandled message type in case " + name + ": " + message);
            continue;
        }

        if (entry["bytes"].is_string()) {
            const std::vector<std::uint8_t> expected = from_hex(entry["bytes"].string_value());
            ++check_count();
            if (actual != expected) {
                report_failure(__FILE__, __LINE__,
                               "golden byte mismatch for case " + name +
                                   describe_diff(std::vector<unsigned char>(actual.begin(),
                                                                            actual.end()),
                                                 std::vector<unsigned char>(expected.begin(),
                                                                            expected.end())));
            }
            ++encoded;
        }

        check_decode(name, message, input, actual);
        ++decoded;
    }
    std::printf("  message cases: %zu encoded against upstream bytes, %zu decoded\n", encoded,
                decoded);

    // Also verify the JSON string-array encoder against the generator's
    // independent Newtonsoft-compatible implementation.
    for (const JsonValue &entry : document["cases"].array_items()) {
        if (entry["message"].string_value() != "rpc") {
            continue;
        }
        std::vector<std::string> arguments;
        for (const JsonValue &argument : entry["input"]["arguments"].array_items()) {
            arguments.push_back(argument.string_value());
        }
        CHECK_EQ(encode_json_string_array(arguments),
                 entry["input"]["arguments_json"].string_value());
    }

    return summary("protocol golden vectors");
}
