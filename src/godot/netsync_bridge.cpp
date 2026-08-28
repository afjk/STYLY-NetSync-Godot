// SPDX-License-Identifier: Apache-2.0
#include "netsync_bridge.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>

#include "core/device_id.hpp"
#include "godot/coordinate_converter.hpp"

using namespace godot;

namespace styly {
namespace netsync {

namespace {

std::string to_std(const String &value) { return std::string(value.utf8().get_data()); }

String to_godot(const std::string &value) { return String::utf8(value.c_str()); }

double monotonic_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

/// Godot Transform3D -> wire pose. The basis is orthonormalised first so a
/// scaled node still yields a unit rotation.
PoseTransform transform_to_wire(const Transform3D &transform) {
    const Quaternion rotation = transform.basis.orthonormalized().get_rotation_quaternion();
    PoseTransform godot_space;
    godot_space.position = Vec3(transform.origin.x, transform.origin.y, transform.origin.z);
    godot_space.rotation = Quat(rotation.x, rotation.y, rotation.z, rotation.w);
    return coordinates::transform_godot_to_netsync(godot_space);
}

Transform3D transform_from_wire(const PoseTransform &pose) {
    const PoseTransform godot_space = coordinates::transform_netsync_to_godot(pose);
    const Quaternion rotation(static_cast<real_t>(godot_space.rotation.x),
                              static_cast<real_t>(godot_space.rotation.y),
                              static_cast<real_t>(godot_space.rotation.z),
                              static_cast<real_t>(godot_space.rotation.w));
    Transform3D out;
    out.basis = Basis(rotation.normalized());
    out.origin = Vector3(static_cast<real_t>(godot_space.position.x),
                         static_cast<real_t>(godot_space.position.y),
                         static_cast<real_t>(godot_space.position.z));
    return out;
}

Vec3 vector_to_wire(const Vector3 &vector) {
    return coordinates::position_godot_to_netsync(Vec3(vector.x, vector.y, vector.z));
}

Vector3 vector_from_wire(const Vec3 &vector) {
    const Vec3 godot_space = coordinates::position_netsync_to_godot(vector);
    return Vector3(static_cast<real_t>(godot_space.x), static_cast<real_t>(godot_space.y),
                   static_cast<real_t>(godot_space.z));
}

bool dictionary_has_transform(const Dictionary &dictionary, const char *key) {
    if (!dictionary.has(key)) {
        return false;
    }
    return dictionary[key].get_type() == Variant::TRANSFORM3D;
}

}  // namespace

NetSyncBridge::NetSyncBridge() : client_(new NetSyncClient()) {}

NetSyncBridge::~NetSyncBridge() {
    if (client_) {
        client_->disconnect();
    }
}

void NetSyncBridge::_bind_methods() {
    ClassDB::bind_method(D_METHOD("connect_to_server", "config"),
                         &NetSyncBridge::connect_to_server);
    ClassDB::bind_method(D_METHOD("disconnect_from_server"),
                         &NetSyncBridge::disconnect_from_server);
    ClassDB::bind_method(D_METHOD("poll"), &NetSyncBridge::poll);

    ClassDB::bind_method(D_METHOD("is_connected_to_server"),
                         &NetSyncBridge::is_connected_to_server);
    ClassDB::bind_method(D_METHOD("is_ready"), &NetSyncBridge::is_ready);
    ClassDB::bind_method(D_METHOD("has_handshake"), &NetSyncBridge::has_handshake);
    ClassDB::bind_method(D_METHOD("has_network_variable_sync"),
                         &NetSyncBridge::has_network_variable_sync);
    ClassDB::bind_method(D_METHOD("get_connection_state"), &NetSyncBridge::get_connection_state);
    ClassDB::bind_method(D_METHOD("get_connection_state_name"),
                         &NetSyncBridge::get_connection_state_name);

    ClassDB::bind_method(D_METHOD("get_client_no"), &NetSyncBridge::get_client_no);
    ClassDB::bind_method(D_METHOD("get_device_id"), &NetSyncBridge::get_device_id);
    ClassDB::bind_method(D_METHOD("get_room_id"), &NetSyncBridge::get_room_id);
    ClassDB::bind_method(D_METHOD("get_server_address"), &NetSyncBridge::get_server_address);
    ClassDB::bind_method(D_METHOD("get_discovered_rest_api_port"),
                         &NetSyncBridge::get_discovered_rest_api_port);

    ClassDB::bind_method(D_METHOD("set_transform_send_rate", "rate"),
                         &NetSyncBridge::set_transform_send_rate);
    ClassDB::bind_method(D_METHOD("get_transform_send_rate"),
                         &NetSyncBridge::get_transform_send_rate);
    ClassDB::bind_method(D_METHOD("get_transport_stats"), &NetSyncBridge::get_transport_stats);

    ClassDB::bind_method(D_METHOD("set_local_pose", "pose"), &NetSyncBridge::set_local_pose);
    ClassDB::bind_method(D_METHOD("clear_local_pose"), &NetSyncBridge::clear_local_pose);

    ClassDB::bind_method(D_METHOD("get_remote_pose", "client_no"),
                         &NetSyncBridge::get_remote_pose);
    ClassDB::bind_method(D_METHOD("get_remote_client_numbers"),
                         &NetSyncBridge::get_remote_client_numbers);
    ClassDB::bind_method(D_METHOD("get_known_client_numbers"),
                         &NetSyncBridge::get_known_client_numbers);
    ClassDB::bind_method(D_METHOD("get_device_id_for", "client_no"),
                         &NetSyncBridge::get_device_id_for);
    ClassDB::bind_method(D_METHOD("get_client_no_for", "device_id"),
                         &NetSyncBridge::get_client_no_for);
    ClassDB::bind_method(D_METHOD("is_client_stealth", "client_no"),
                         &NetSyncBridge::is_client_stealth);
    ClassDB::bind_method(D_METHOD("get_last_room_broadcast_time"),
                         &NetSyncBridge::get_last_room_broadcast_time);

    ClassDB::bind_method(D_METHOD("send_rpc", "function_name", "args"), &NetSyncBridge::send_rpc);
    ClassDB::bind_method(D_METHOD("send_rpc_to", "target_client_no", "function_name", "args"),
                         &NetSyncBridge::send_rpc_to);
    ClassDB::bind_method(
        D_METHOD("send_rpc_to_many", "target_client_nos", "function_name", "args"),
        &NetSyncBridge::send_rpc_to_many);
    ClassDB::bind_method(
        D_METHOD("configure_rpc_rate_limit", "rpc_limit", "window_seconds", "warn_cooldown"),
        &NetSyncBridge::configure_rpc_rate_limit);

    ClassDB::bind_method(D_METHOD("set_global_variable", "name", "value"),
                         &NetSyncBridge::set_global_variable);
    ClassDB::bind_method(D_METHOD("get_global_variable", "name", "fallback"),
                         &NetSyncBridge::get_global_variable);
    ClassDB::bind_method(D_METHOD("get_all_global_variables"),
                         &NetSyncBridge::get_all_global_variables);
    ClassDB::bind_method(D_METHOD("set_client_variable", "name", "value"),
                         &NetSyncBridge::set_client_variable);
    ClassDB::bind_method(D_METHOD("set_client_variable_for", "target_client_no", "name", "value"),
                         &NetSyncBridge::set_client_variable_for);
    ClassDB::bind_method(D_METHOD("get_client_variable", "client_no", "name", "fallback"),
                         &NetSyncBridge::get_client_variable);
    ClassDB::bind_method(D_METHOD("get_all_client_variables", "client_no"),
                         &NetSyncBridge::get_all_client_variables);
    ClassDB::bind_method(D_METHOD("clear_my_client_variables"),
                         &NetSyncBridge::clear_my_client_variables);

    ClassDB::bind_method(D_METHOD("register_object", "object_id"),
                         &NetSyncBridge::register_object);
    ClassDB::bind_method(D_METHOD("unregister_object", "object_id"),
                         &NetSyncBridge::unregister_object);
    ClassDB::bind_method(D_METHOD("submit_object_pose", "object_id", "transform"),
                         &NetSyncBridge::submit_object_pose);
    ClassDB::bind_method(D_METHOD("get_object_state", "object_id"),
                         &NetSyncBridge::get_object_state);
    ClassDB::bind_method(D_METHOD("request_object_ownership", "object_id"),
                         &NetSyncBridge::request_object_ownership);
    ClassDB::bind_method(D_METHOD("release_object_ownership", "object_id"),
                         &NetSyncBridge::release_object_ownership);

    ClassDB::bind_static_method("NetSyncBridge", D_METHOD("generate_device_id"),
                                &NetSyncBridge::generate_device_id);
    ClassDB::bind_static_method("NetSyncBridge", D_METHOD("load_or_create_device_id", "path"),
                                &NetSyncBridge::load_or_create_device_id);
    ClassDB::bind_static_method("NetSyncBridge", D_METHOD("hash_object_id", "name"),
                                &NetSyncBridge::hash_object_id);
    ClassDB::bind_static_method("NetSyncBridge",
                                D_METHOD("godot_to_netsync_transform", "transform"),
                                &NetSyncBridge::godot_to_netsync_transform);
    ClassDB::bind_static_method("NetSyncBridge",
                                D_METHOD("netsync_to_godot_transform", "transform"),
                                &NetSyncBridge::netsync_to_godot_transform);

    ADD_SIGNAL(MethodInfo("connection_state_changed",
                          PropertyInfo(Variant::INT, "state"),
                          PropertyInfo(Variant::STRING, "state_name")));
    ADD_SIGNAL(MethodInfo("ready"));
    ADD_SIGNAL(MethodInfo("connection_error", PropertyInfo(Variant::STRING, "message")));
    ADD_SIGNAL(MethodInfo("server_discovered", PropertyInfo(Variant::STRING, "address"),
                          PropertyInfo(Variant::STRING, "server_name"),
                          PropertyInfo(Variant::INT, "control_port"),
                          PropertyInfo(Variant::INT, "transform_port"),
                          PropertyInfo(Variant::INT, "sub_port")));
    ADD_SIGNAL(MethodInfo("client_no_assigned", PropertyInfo(Variant::INT, "client_no")));
    ADD_SIGNAL(MethodInfo("avatar_connected", PropertyInfo(Variant::INT, "client_no"),
                          PropertyInfo(Variant::STRING, "device_id")));
    ADD_SIGNAL(MethodInfo("avatar_disconnected", PropertyInfo(Variant::INT, "client_no")));
    ADD_SIGNAL(MethodInfo("rpc_received", PropertyInfo(Variant::INT, "sender_client_no"),
                          PropertyInfo(Variant::STRING, "function_name"),
                          PropertyInfo(Variant::PACKED_STRING_ARRAY, "args")));
    ADD_SIGNAL(MethodInfo("global_variable_changed", PropertyInfo(Variant::STRING, "name"),
                          PropertyInfo(Variant::STRING, "old_value"),
                          PropertyInfo(Variant::STRING, "new_value")));
    ADD_SIGNAL(MethodInfo("client_variable_changed", PropertyInfo(Variant::INT, "client_no"),
                          PropertyInfo(Variant::STRING, "name"),
                          PropertyInfo(Variant::STRING, "old_value"),
                          PropertyInfo(Variant::STRING, "new_value")));
    ADD_SIGNAL(MethodInfo("object_ownership_changed", PropertyInfo(Variant::INT, "object_id"),
                          PropertyInfo(Variant::INT, "new_owner_client_no"),
                          PropertyInfo(Variant::INT, "previous_owner_client_no")));
    ADD_SIGNAL(MethodInfo("object_ownership_rejected", PropertyInfo(Variant::INT, "object_id"),
                          PropertyInfo(Variant::INT, "current_owner_client_no"),
                          PropertyInfo(Variant::INT, "reason_code")));
    ADD_SIGNAL(MethodInfo("server_version_received", PropertyInfo(Variant::INT, "major"),
                          PropertyInfo(Variant::INT, "minor"),
                          PropertyInfo(Variant::INT, "patch")));
    ADD_SIGNAL(MethodInfo("log_message", PropertyInfo(Variant::STRING, "message")));

    BIND_ENUM_CONSTANT(STATE_DISCONNECTED);
    BIND_ENUM_CONSTANT(STATE_CONNECTING);
    BIND_ENUM_CONSTANT(STATE_CONNECTED);
    BIND_ENUM_CONSTANT(STATE_SYNCHRONIZING);
    BIND_ENUM_CONSTANT(STATE_READY);
    BIND_ENUM_CONSTANT(STATE_ERROR);
}

// --- Lifecycle ---------------------------------------------------------------

bool NetSyncBridge::connect_to_server(const Dictionary &config) {
    ClientConfig client_config;
    if (config.has("server_address")) {
        client_config.server_address = to_std(config["server_address"]);
    }
    if (config.has("control_port")) {
        client_config.control_port = static_cast<int>(config["control_port"]);
    }
    if (config.has("transform_port")) {
        client_config.transform_port = static_cast<int>(config["transform_port"]);
    }
    if (config.has("sub_port")) {
        client_config.sub_port = static_cast<int>(config["sub_port"]);
    }
    if (config.has("discovery_port")) {
        client_config.discovery_port = static_cast<int>(config["discovery_port"]);
    }
    if (config.has("room_id")) {
        client_config.room_id = to_std(config["room_id"]);
    }
    if (config.has("device_id")) {
        client_config.device_id = to_std(config["device_id"]);
    }
    if (config.has("stealth_mode")) {
        client_config.stealth_mode = static_cast<bool>(config["stealth_mode"]);
    }
    if (config.has("transform_send_rate")) {
        client_config.transform_send_rate = static_cast<double>(config["transform_send_rate"]);
    }
    if (config.has("enable_discovery")) {
        client_config.enable_discovery = static_cast<bool>(config["enable_discovery"]);
    }

    if (client_config.device_id.empty()) {
        UtilityFunctions::push_error(
            "[STYLY NetSync] connect_to_server requires a non-empty device_id: the server keys "
            "room membership on it.");
        return false;
    }
    return client_->connect(client_config);
}

void NetSyncBridge::disconnect_from_server() { client_->disconnect(); }

void NetSyncBridge::poll() {
    // Every signal below is emitted on this thread, which is the caller's
    // (the addon calls poll() from _process). The network thread never gets here.
    for (const Event &event : client_->poll(monotonic_seconds())) {
        emit_event(event);
    }
}

void NetSyncBridge::emit_event(const Event &event) {
    switch (event.type) {
        case EventType::ConnectionStateChanged:
            emit_signal("connection_state_changed", event.value_a, to_godot(event.name));
            break;
        case EventType::Ready:
            emit_signal("ready");
            break;
        case EventType::ConnectionError:
            emit_signal("connection_error", to_godot(event.name));
            break;
        case EventType::ServerDiscovered:
            emit_signal("server_discovered", to_godot(event.name), to_godot(event.new_value),
                        event.value_a, event.value_b, event.value_c);
            break;
        case EventType::ClientNoAssigned:
            emit_signal("client_no_assigned", event.client_no);
            break;
        case EventType::AvatarConnected:
            emit_signal("avatar_connected", event.client_no, to_godot(event.name));
            break;
        case EventType::AvatarDisconnected:
            emit_signal("avatar_disconnected", event.client_no);
            break;
        case EventType::RpcReceived: {
            PackedStringArray args;
            for (const std::string &argument : event.args) {
                args.push_back(to_godot(argument));
            }
            emit_signal("rpc_received", event.client_no, to_godot(event.name), args);
            break;
        }
        case EventType::GlobalVariableChanged:
            emit_signal("global_variable_changed", to_godot(event.name),
                        to_godot(event.old_value), to_godot(event.new_value));
            break;
        case EventType::ClientVariableChanged:
            emit_signal("client_variable_changed", event.client_no, to_godot(event.name),
                        to_godot(event.old_value), to_godot(event.new_value));
            break;
        case EventType::ObjectOwnershipChanged:
            emit_signal("object_ownership_changed", static_cast<int64_t>(event.object_id),
                        event.value_a, event.value_b);
            break;
        case EventType::ObjectOwnershipRejected:
            emit_signal("object_ownership_rejected", static_cast<int64_t>(event.object_id),
                        event.value_a, event.value_b);
            break;
        case EventType::ServerVersion:
            emit_signal("server_version_received", event.value_a, event.value_b, event.value_c);
            break;
        case EventType::Log:
            emit_signal("log_message", to_godot(event.name));
            break;
    }
}

bool NetSyncBridge::is_connected_to_server() const { return client_->is_connected(); }
bool NetSyncBridge::is_ready() const { return client_->is_ready(); }
bool NetSyncBridge::has_handshake() const { return client_->has_handshake(); }
bool NetSyncBridge::has_network_variable_sync() const {
    return client_->has_network_variable_sync();
}
int NetSyncBridge::get_connection_state() const { return static_cast<int>(client_->state()); }
String NetSyncBridge::get_connection_state_name() const {
    return String(connection_state_name(client_->state()));
}
int NetSyncBridge::get_client_no() const { return client_->client_no(); }
String NetSyncBridge::get_device_id() const { return to_godot(client_->device_id()); }
String NetSyncBridge::get_room_id() const { return to_godot(client_->room_id()); }
String NetSyncBridge::get_server_address() const { return to_godot(client_->server_address()); }
int NetSyncBridge::get_discovered_rest_api_port() const {
    return client_->discovered_rest_api_port();
}

void NetSyncBridge::set_transform_send_rate(double rate) {
    client_->set_transform_send_rate(rate);
}
double NetSyncBridge::get_transform_send_rate() const { return client_->transform_send_rate(); }

Dictionary NetSyncBridge::get_transport_stats() const {
    const TransportStats stats = client_->transport_stats();
    Dictionary out;
    out["messages_sent"] = static_cast<int64_t>(stats.messages_sent);
    out["messages_received"] = static_cast<int64_t>(stats.messages_received);
    out["dropped_transform_frames"] = static_cast<int64_t>(stats.dropped_transform_frames);
    out["would_block_count"] = static_cast<int64_t>(stats.would_block_count);
    out["control_dropped_full"] = static_cast<int64_t>(stats.control_dropped_full);
    out["control_dropped_expired"] = static_cast<int64_t>(stats.control_dropped_expired);
    out["control_queue_length"] = stats.control_queue_length;
    return out;
}

// --- Local pose ---------------------------------------------------------------

void NetSyncBridge::set_local_pose(const Dictionary &pose) {
    ClientPoseBody body;
    std::uint8_t flags = 0;

    const bool moving_floor_local =
        pose.has("moving_floor_local") && static_cast<bool>(pose["moving_floor_local"]);
    if (moving_floor_local) {
        flags |= POSE_FLAG_MOVING_FLOOR_LOCAL;
    }

    if (dictionary_has_transform(pose, "head")) {
        flags |= POSE_FLAG_HEAD_VALID;
        body.head = transform_to_wire(pose["head"]);
    }
    if (dictionary_has_transform(pose, "right_hand")) {
        flags |= POSE_FLAG_RIGHT_VALID;
        body.right_hand = transform_to_wire(pose["right_hand"]);
    }
    if (dictionary_has_transform(pose, "left_hand")) {
        flags |= POSE_FLAG_LEFT_VALID;
        body.left_hand = transform_to_wire(pose["left_hand"]);
    }
    if (dictionary_has_transform(pose, "physical")) {
        body.physical = transform_to_wire(pose["physical"]);
    }

    if (pose.has("virtuals") && pose["virtuals"].get_type() == Variant::ARRAY) {
        const Array virtuals = pose["virtuals"];
        for (int i = 0; i < virtuals.size(); ++i) {
            if (virtuals[i].get_type() != Variant::TRANSFORM3D) {
                continue;
            }
            body.virtuals.push_back(transform_to_wire(virtuals[i]));
        }
        if (!body.virtuals.empty()) {
            flags |= POSE_FLAG_VIRTUALS_VALID;
        }
    }

    if (pose.has("xr_origin_delta_position")) {
        body.xr_origin_delta_position = vector_to_wire(pose["xr_origin_delta_position"]);
    }
    if (pose.has("xr_origin_delta_yaw")) {
        body.xr_origin_delta_yaw = coordinates::yaw_degrees_godot_to_netsync(
            static_cast<double>(pose["xr_origin_delta_yaw"]));
    }

    // Upstream always sets PhysicalValid for a non-stealth avatar: the receiver
    // needs the XR-origin delta to reconstruct the real-world (physical) pose.
    const bool physical_valid =
        pose.has("physical_valid") ? static_cast<bool>(pose["physical_valid"]) : true;
    if (physical_valid) {
        flags |= POSE_FLAG_PHYSICAL_VALID;
    }

    body.flags = flags;
    client_->set_local_pose(body);
}

void NetSyncBridge::clear_local_pose() { client_->clear_local_pose(); }

// --- Remote state -------------------------------------------------------------

Dictionary NetSyncBridge::get_remote_pose(int client_no) const {
    Dictionary out;
    RemoteClientPose pose;
    if (!client_->get_remote_pose(client_no, pose)) {
        return out;
    }

    const std::uint8_t flags = pose.body.flags;
    out["client_no"] = pose.client_no;
    out["pose_time"] = pose.pose_time;
    out["broadcast_time"] = pose.broadcast_time;
    out["pose_seq"] = static_cast<int>(pose.body.pose_seq);
    out["flags"] = static_cast<int>(flags);
    out["is_stealth"] = (flags & POSE_FLAG_STEALTH) != 0;
    out["has_head"] = (flags & POSE_FLAG_HEAD_VALID) != 0;
    out["has_right_hand"] = (flags & POSE_FLAG_RIGHT_VALID) != 0;
    out["has_left_hand"] = (flags & POSE_FLAG_LEFT_VALID) != 0;
    out["has_virtuals"] = (flags & POSE_FLAG_VIRTUALS_VALID) != 0;
    out["has_physical"] = (flags & POSE_FLAG_PHYSICAL_VALID) != 0;
    out["moving_floor_local"] = (flags & POSE_FLAG_MOVING_FLOOR_LOCAL) != 0;

    out["head"] = transform_from_wire(pose.body.head);
    out["right_hand"] = transform_from_wire(pose.body.right_hand);
    out["left_hand"] = transform_from_wire(pose.body.left_hand);
    out["physical"] = transform_from_wire(pose.body.physical);
    out["xr_origin_delta_position"] = vector_from_wire(pose.body.xr_origin_delta_position);
    out["xr_origin_delta_yaw"] =
        coordinates::yaw_degrees_netsync_to_godot(pose.body.xr_origin_delta_yaw);

    Array virtuals;
    for (const PoseTransform &entry : pose.body.virtuals) {
        virtuals.push_back(transform_from_wire(entry));
    }
    out["virtuals"] = virtuals;
    return out;
}

PackedInt32Array NetSyncBridge::get_remote_client_numbers() const {
    PackedInt32Array out;
    for (int client_no : client_->remote_client_numbers()) {
        out.push_back(client_no);
    }
    return out;
}

PackedInt32Array NetSyncBridge::get_known_client_numbers() const {
    PackedInt32Array out;
    for (int client_no : client_->known_client_numbers()) {
        out.push_back(client_no);
    }
    return out;
}

String NetSyncBridge::get_device_id_for(int client_no) const {
    return to_godot(client_->device_id_for(client_no));
}

int NetSyncBridge::get_client_no_for(const String &device_id) const {
    return client_->client_no_for(to_std(device_id));
}

bool NetSyncBridge::is_client_stealth(int client_no) const {
    return client_->is_client_stealth(client_no);
}

double NetSyncBridge::get_last_room_broadcast_time() const {
    return client_->last_room_broadcast_time();
}

// --- RPC ----------------------------------------------------------------------

namespace {

std::vector<std::string> to_std_vector(const PackedStringArray &values) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(values.size()));
    for (int i = 0; i < values.size(); ++i) {
        out.push_back(to_std(values[i]));
    }
    return out;
}

}  // namespace

void NetSyncBridge::send_rpc(const String &function_name, const PackedStringArray &args) {
    client_->rpc(to_std(function_name), to_std_vector(args));
}

void NetSyncBridge::send_rpc_to(int target_client_no, const String &function_name,
                                const PackedStringArray &args) {
    client_->rpc_to(target_client_no, to_std(function_name), to_std_vector(args));
}

void NetSyncBridge::send_rpc_to_many(const PackedInt32Array &target_client_nos,
                                     const String &function_name,
                                     const PackedStringArray &args) {
    std::vector<int> targets;
    targets.reserve(static_cast<std::size_t>(target_client_nos.size()));
    for (int i = 0; i < target_client_nos.size(); ++i) {
        targets.push_back(target_client_nos[i]);
    }
    client_->rpc_to_many(targets, to_std(function_name), to_std_vector(args));
}

void NetSyncBridge::configure_rpc_rate_limit(int rpc_limit, double window_seconds,
                                             double warn_cooldown) {
    client_->configure_rpc_rate_limit(rpc_limit, window_seconds, warn_cooldown);
}

// --- Network variables ---------------------------------------------------------

bool NetSyncBridge::set_global_variable(const String &name, const String &value) {
    return client_->set_global_variable(to_std(name), to_std(value));
}

String NetSyncBridge::get_global_variable(const String &name, const String &fallback) const {
    return to_godot(client_->get_global_variable(to_std(name), to_std(fallback)));
}

Dictionary NetSyncBridge::get_all_global_variables() const {
    Dictionary out;
    for (const auto &entry : client_->get_all_global_variables()) {
        out[to_godot(entry.first)] = to_godot(entry.second);
    }
    return out;
}

bool NetSyncBridge::set_client_variable(const String &name, const String &value) {
    return client_->set_client_variable(to_std(name), to_std(value));
}

bool NetSyncBridge::set_client_variable_for(int target_client_no, const String &name,
                                            const String &value) {
    return client_->set_client_variable_for(target_client_no, to_std(name), to_std(value));
}

String NetSyncBridge::get_client_variable(int client_no, const String &name,
                                          const String &fallback) const {
    return to_godot(client_->get_client_variable(client_no, to_std(name), to_std(fallback)));
}

Dictionary NetSyncBridge::get_all_client_variables(int client_no) const {
    Dictionary out;
    for (const auto &entry : client_->get_all_client_variables(client_no)) {
        out[to_godot(entry.first)] = to_godot(entry.second);
    }
    return out;
}

bool NetSyncBridge::clear_my_client_variables() { return client_->clear_my_client_variables(); }

// --- Objects --------------------------------------------------------------------

namespace {

/// Object ids are unsigned 32-bit on the wire; GDScript integers are signed
/// 64-bit, so both a positive and a sign-extended value must land on the same id.
std::uint32_t to_object_id(int64_t value) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) & 0xFFFFFFFFull);
}

}  // namespace

bool NetSyncBridge::register_object(int64_t object_id) {
    return client_->register_object(to_object_id(object_id));
}

void NetSyncBridge::unregister_object(int64_t object_id) {
    client_->unregister_object(to_object_id(object_id));
}

void NetSyncBridge::submit_object_pose(int64_t object_id, const Transform3D &transform) {
    client_->submit_object_pose(to_object_id(object_id), transform_to_wire(transform));
}

Dictionary NetSyncBridge::get_object_state(int64_t object_id) const {
    Dictionary out;
    ObjectState state;
    if (!client_->get_object_state(to_object_id(object_id), state)) {
        out["found"] = false;
        return out;
    }
    out["found"] = true;
    out["object_id"] = static_cast<int64_t>(state.object_id);
    out["owner_client_no"] = state.owner_client_no;
    out["pose_seq"] = static_cast<int>(state.pose_seq);
    out["pose_time"] = state.pose_time;
    out["broadcast_time"] = state.broadcast_time;
    out["has_pose"] = state.has_pose;
    out["transform"] = transform_from_wire(state.pose);
    out["is_owned_by_me"] =
        state.owner_client_no != 0 && state.owner_client_no == client_->client_no();
    return out;
}

bool NetSyncBridge::request_object_ownership(int64_t object_id) {
    return client_->request_object_ownership(to_object_id(object_id));
}

bool NetSyncBridge::release_object_ownership(int64_t object_id) {
    return client_->release_object_ownership(to_object_id(object_id));
}

// --- Utilities --------------------------------------------------------------------

String NetSyncBridge::generate_device_id() { return to_godot(generate_uuid_v4()); }

String NetSyncBridge::load_or_create_device_id(const String &path) {
    return to_godot(styly::netsync::load_or_create_device_id(to_std(path)));
}

int64_t NetSyncBridge::hash_object_id(const String &name) {
    // 32-bit FNV-1a over the UTF-8 bytes.
    const std::string text = to_std(name);
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 16777619u;
    }
    // 0 means "unassigned" in the protocol, so never return it.
    if (hash == 0u) {
        hash = 1u;
    }
    return static_cast<int64_t>(hash);
}

Transform3D NetSyncBridge::godot_to_netsync_transform(const Transform3D &transform) {
    const PoseTransform wire = transform_to_wire(transform);
    const Quaternion rotation(
        static_cast<real_t>(wire.rotation.x), static_cast<real_t>(wire.rotation.y),
        static_cast<real_t>(wire.rotation.z), static_cast<real_t>(wire.rotation.w));
    Transform3D out;
    out.basis = Basis(rotation.normalized());
    out.origin = Vector3(static_cast<real_t>(wire.position.x),
                         static_cast<real_t>(wire.position.y),
                         static_cast<real_t>(wire.position.z));
    return out;
}

Transform3D NetSyncBridge::netsync_to_godot_transform(const Transform3D &transform) {
    const Quaternion rotation = transform.basis.orthonormalized().get_rotation_quaternion();
    PoseTransform wire;
    wire.position = Vec3(transform.origin.x, transform.origin.y, transform.origin.z);
    wire.rotation = Quat(rotation.x, rotation.y, rotation.z, rotation.w);
    return transform_from_wire(wire);
}

}  // namespace netsync
}  // namespace styly
