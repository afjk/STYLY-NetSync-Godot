// SPDX-License-Identifier: Apache-2.0
#include "netsync_client.hpp"

#include <algorithm>
#include <cmath>

#include "core/json_util.hpp"

namespace styly {
namespace netsync {

namespace {

/// Upstream heartbeat: an unchanged pose is still re-sent this often.
constexpr double kPoseHeartbeatSeconds = 1.0;

/// Build the change-detection signature for a pose body.
///
/// Upstream hashes the quantised fields with FNV-1a, excluding poseSeq and the
/// device id. Serialising the body with poseSeq zeroed discriminates on exactly
/// the same information, without needing a second implementation of the
/// quantisation to stay in step with the serializer.
std::vector<std::uint8_t> pose_signature(const ClientPoseBody &body) {
    ClientPoseBody copy = body;
    copy.pose_seq = 0;
    BinaryWriter writer(128);
    ClientPoseMessage message;
    message.device_id.clear();
    message.body = copy;
    return serialize_client_pose(message);
}

}  // namespace

const char *connection_state_name(ConnectionState state) {
    switch (state) {
        case ConnectionState::Disconnected:
            return "disconnected";
        case ConnectionState::Connecting:
            return "connecting";
        case ConnectionState::Connected:
            return "connected";
        case ConnectionState::Synchronizing:
            return "synchronizing";
        case ConnectionState::Ready:
            return "ready";
        case ConnectionState::Error:
            return "error";
    }
    return "unknown";
}

NetSyncClient::NetSyncClient() {
    transport_.set_payload_callback(
        [this](TransportLane lane, const std::uint8_t *data, std::size_t size) {
            handle_payload(lane, data, size);
        });
    transport_.set_connected_callback([this]() {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        PendingNotification notification;
        notification.kind = PendingNotification::Kind::Connected;
        notifications_.push_back(notification);
    });
    transport_.set_error_callback([this](const std::string &message) {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        PendingNotification notification;
        notification.kind = PendingNotification::Kind::Error;
        notification.text = message;
        notifications_.push_back(notification);
    });
    transport_.set_log_callback([this](const std::string &message) { push_log(message); });

    discovery_.set_log_callback([this](const std::string &message) { push_log(message); });
    discovery_.set_found_callback([this](const DiscoveredServer &server) {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        PendingNotification notification;
        notification.kind = PendingNotification::Kind::Discovered;
        notification.server = server;
        notifications_.push_back(notification);
    });

    // Control-lane senders. All three managers share the transport's outbox and
    // therefore its priority, TTL and capacity rules.
    const auto control_sender = [this](std::vector<std::uint8_t> payload) {
        return transport_.try_enqueue_control(std::move(payload));
    };
    rpc_manager_.set_control_sender(control_sender);
    variables_.set_control_sender(control_sender);
    objects_.set_control_sender(control_sender);
    objects_.set_object_transform_sender(
        [this](std::uint32_t object_id, std::vector<std::uint8_t> payload) {
            transport_.set_latest_object_transform(object_id, std::move(payload));
        });

    const auto log_callback = [this](const std::string &message) { push_log(message); };
    rpc_manager_.set_log_callback(log_callback);
    variables_.set_log_callback(log_callback);
    objects_.set_log_callback(log_callback);
}

NetSyncClient::~NetSyncClient() { disconnect(); }

void NetSyncClient::push_log(const std::string &message) {
    std::lock_guard<std::mutex> lock(inbox_mutex_);
    PendingNotification notification;
    notification.kind = PendingNotification::Kind::Log;
    notification.text = message;
    notifications_.push_back(notification);
}

bool NetSyncClient::connect(const ClientConfig &config) {
    if (state_ != ConnectionState::Disconnected && state_ != ConnectionState::Error) {
        return false;
    }

    // Reconnecting after an error: the previous discovery and network threads
    // may still be alive, and start() would refuse. Stop them first so a retry
    // after a failed connection actually reconnects.
    discovery_.stop();
    transport_.stop();

    config_ = config;
    config_.transform_send_rate = std::min(60.0, std::max(0.5, config_.transform_send_rate));
    if (config_.room_id.empty()) {
        config_.room_id = "default_room";
    }

    reset_session_state();

    mapping_.set_local_device_id(config_.device_id);
    variables_.set_local_identity(0, config_.device_id);
    objects_.set_local_identity(0, config_.device_id);

    state_ = ConnectionState::Connecting;

    if (!config_.server_address.empty()) {
        resolved_address_ = config_.server_address;
        start_transport(resolved_address_, config_.control_port, config_.transform_port,
                        config_.sub_port);
        return true;
    }

    if (!config_.enable_discovery) {
        state_ = ConnectionState::Error;
        push_log("no server address configured and discovery is disabled");
        return false;
    }

    discovery_.set_port(config_.discovery_port);
    if (!discovery_.start()) {
        state_ = ConnectionState::Error;
        push_log("failed to start server discovery");
        return false;
    }
    return true;
}

void NetSyncClient::start_transport(const std::string &address, int control_port,
                                    int transform_port, int sub_port) {
    TransportConfig transport_config;
    transport_config.server_address = address;
    transport_config.control_port = control_port;
    transport_config.transform_port = transform_port;
    transport_config.sub_port = sub_port;
    transport_config.room_id = config_.room_id;
    hello_enqueued_ = false;
    if (!transport_.start(transport_config)) {
        push_log("transport is already running");
    }
}

void NetSyncClient::disconnect() {
    discovery_.stop();
    transport_.stop();
    reset_session_state();
    state_ = ConnectionState::Disconnected;
}

void NetSyncClient::reset_session_state() {
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        control_inbox_.clear();
        latest_room_pose_.clear();
        has_room_pose_ = false;
        latest_room_objects_.clear();
        has_room_objects_ = false;
        notifications_.clear();
    }
    mapping_.clear();
    mapping_.set_local_device_id(config_.device_id);
    rpc_manager_.clear_pending();
    variables_.clear_all();
    objects_.clear_room_scoped_state();
    remote_poses_.clear();
    last_room_broadcast_time_ = 0.0;
    ready_fired_ = false;
    version_checked_ = false;
    hello_enqueued_ = false;
    has_local_pose_ = false;
    local_pose_ = ClientPoseBody();
    local_pose_seq_ = 0;
    last_pose_send_time_ = -1e9;
    last_pose_change_send_time_ = -1e9;
    has_last_pose_signature_ = false;
    last_pose_signature_.clear();
    discovered_rest_api_port_ = 0;
}

bool NetSyncClient::is_connected() const {
    return transport_.is_running() && !transport_.has_error();
}

bool NetSyncClient::is_ready() const {
    return is_connected() && has_handshake() && has_network_variable_sync();
}

void NetSyncClient::set_transform_send_rate(double rate) {
    config_.transform_send_rate = std::min(60.0, std::max(0.5, rate));
}

void NetSyncClient::set_state(ConnectionState state, std::vector<Event> &events) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    Event event;
    event.type = EventType::ConnectionStateChanged;
    event.value_a = static_cast<int>(state);
    event.name = connection_state_name(state);
    events.push_back(event);
}

void NetSyncClient::enqueue_hello() {
    // The hello must be the first control message of the connection: the server
    // binds the control identity from it and only then unicasts the ID mapping
    // and the network-variable snapshot.
    if (transport_.try_enqueue_control(
            serialize_client_hello(config_.device_id, config_.stealth_mode))) {
        hello_enqueued_ = true;
    }
}

void NetSyncClient::handle_payload(TransportLane lane, const std::uint8_t *data,
                                   std::size_t size) {
    // Network thread: copy and queue only. No decoding, no state mutation.
    std::lock_guard<std::mutex> lock(inbox_mutex_);
    switch (lane) {
        case TransportLane::Control:
            if (control_inbox_.size() >= kControlInboxMax) {
                control_inbox_.pop_front();
                ++control_inbox_dropped_;
            }
            control_inbox_.emplace_back(data, data + size);
            break;
        case TransportLane::RoomPose:
            // Latest-wins, depth 1: older room snapshots have no value.
            latest_room_pose_.assign(data, data + size);
            has_room_pose_ = true;
            break;
        case TransportLane::RoomObjects:
            latest_room_objects_.assign(data, data + size);
            has_room_objects_ = true;
            break;
    }
}

void NetSyncClient::drain_internal_events(std::vector<Event> &events) {
    std::deque<PendingNotification> notifications;
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        notifications.swap(notifications_);
    }

    for (const PendingNotification &notification : notifications) {
        switch (notification.kind) {
            case PendingNotification::Kind::Connected: {
                // The initial-sync timeout is seeded in poll(), which has the
                // host's clock; the network thread has no business setting it.
                set_state(ConnectionState::Connected, events);
                break;
            }
            case PendingNotification::Kind::Error: {
                set_state(ConnectionState::Error, events);
                Event event;
                event.type = EventType::ConnectionError;
                event.name = notification.text;
                events.push_back(event);
                break;
            }
            case PendingNotification::Kind::Log: {
                Event event;
                event.type = EventType::Log;
                event.name = notification.text;
                events.push_back(event);
                break;
            }
            case PendingNotification::Kind::Discovered: {
                resolved_address_ = notification.server.address;
                discovered_rest_api_port_ = notification.server.rest_api_port;
                Event event;
                event.type = EventType::ServerDiscovered;
                event.name = notification.server.address;
                event.new_value = notification.server.server_name;
                event.value_a = notification.server.control_port;
                event.value_b = notification.server.transform_port;
                event.value_c = notification.server.sub_port;
                events.push_back(event);
                start_transport(notification.server.address, notification.server.control_port,
                                notification.server.transform_port, notification.server.sub_port);
                break;
            }
        }
    }
}

std::vector<Event> NetSyncClient::poll(double now_seconds) {
    std::vector<Event> events;
    if (state_ == ConnectionState::Disconnected) {
        return events;
    }

    drain_internal_events(events);

    // The connection-established timestamp seeds the NV initial-sync timeout;
    // it is taken here because the network thread has no clock of the host's.
    if (state_ == ConnectionState::Connected && !hello_enqueued_) {
        variables_.on_connection_established(now_seconds);
        enqueue_hello();
        if (hello_enqueued_) {
            set_state(ConnectionState::Synchronizing, events);
        }
    }

    // Drain inbound payloads.
    std::deque<std::vector<std::uint8_t>> control;
    std::vector<std::uint8_t> room_pose;
    std::vector<std::uint8_t> room_objects;
    bool had_room_pose = false;
    bool had_room_objects = false;
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        control.swap(control_inbox_);
        if (has_room_pose_) {
            room_pose.swap(latest_room_pose_);
            has_room_pose_ = false;
            had_room_pose = true;
        }
        if (has_room_objects_) {
            room_objects.swap(latest_room_objects_);
            has_room_objects_ = false;
            had_room_objects = true;
        }
    }

    if (had_room_pose) {
        process_room_pose(room_pose, events);
    }
    for (const std::vector<std::uint8_t> &payload : control) {
        process_control_payload(payload, events);
    }
    if (had_room_objects) {
        process_room_objects(room_objects, events);
    }

    // Control traffic is flushed before the pose so a low-bandwidth link does
    // not starve RPC and network variables, matching upstream's ordering.
    variables_.tick(now_seconds);
    rpc_manager_.flush_pending(now_seconds, is_ready(), mapping_.local_client_no(),
                               config_.device_id);
    send_local_pose_if_due(now_seconds);
    objects_.tick(now_seconds, config_.transform_send_rate);

    if (variables_.check_initial_sync_timeout(now_seconds)) {
        // A room with no variables never sends a sync; the timeout is what
        // makes such a room become ready at all.
    }

    update_ready(events);
    return events;
}

void NetSyncClient::update_ready(std::vector<Event> &events) {
    if (state_ == ConnectionState::Error || state_ == ConnectionState::Disconnected) {
        return;
    }
    if (is_ready()) {
        set_state(ConnectionState::Ready, events);
        if (!ready_fired_) {
            ready_fired_ = true;
            Event event;
            event.type = EventType::Ready;
            event.client_no = mapping_.local_client_no();
            events.push_back(event);
        }
    } else if (state_ == ConnectionState::Ready) {
        // Readiness was lost (e.g. the handshake was reset); go back to
        // synchronising so the host can reflect that.
        set_state(ConnectionState::Synchronizing, events);
        ready_fired_ = false;
    }
}

void NetSyncClient::process_control_payload(const std::vector<std::uint8_t> &payload,
                                            std::vector<Event> &events) {
    const std::uint8_t type = peek_message_type(payload.data(), payload.size());
    switch (type) {
        case MSG_DEVICE_ID_MAPPING: {
            DeviceIdMappingMessage message;
            if (!deserialize_device_id_mapping(payload.data(), payload.size(), message)) {
                push_log("malformed device id mapping message");
                return;
            }
            const DeviceMapping::ApplyResult result = mapping_.apply(message);
            variables_.set_local_identity(mapping_.local_client_no(), config_.device_id);
            objects_.set_local_identity(mapping_.local_client_no(), config_.device_id);

            if (!version_checked_) {
                version_checked_ = true;
                Event event;
                event.type = EventType::ServerVersion;
                event.value_a = result.server_version_major;
                event.value_b = result.server_version_minor;
                event.value_c = result.server_version_patch;
                event.name = std::to_string(result.server_version_major) + "." +
                             std::to_string(result.server_version_minor) + "." +
                             std::to_string(result.server_version_patch);
                // Reported once per connection. Upstream Unity compares this
                // against its own package version and warns on a major/minor
                // mismatch; this client has no package version of its own to
                // compare against, so it surfaces the server's version and
                // leaves the policy to the host.
                events.push_back(event);
            }

            if (result.local_client_no_changed) {
                Event event;
                event.type = EventType::ClientNoAssigned;
                event.client_no = result.local_client_no;
                events.push_back(event);
            }
            return;
        }
        case MSG_RPC: {
            RpcMessage message;
            if (!deserialize_rpc(payload.data(), payload.size(), message)) {
                push_log("malformed RPC message");
                return;
            }
            Event event;
            event.type = EventType::RpcReceived;
            event.client_no = static_cast<int>(message.sender_client_no);
            event.name = message.function_name;
            if (!decode_json_string_array(message.arguments_json, event.args)) {
                push_log("RPC '" + message.function_name +
                         "' carried arguments that are not a JSON array; delivering none");
                event.args.clear();
            }
            events.push_back(event);
            return;
        }
        case MSG_GLOBAL_VAR_SYNC: {
            GlobalVarSyncMessage message;
            if (!deserialize_global_var_sync(payload.data(), payload.size(), message)) {
                push_log("malformed global variable sync message");
                return;
            }
            std::vector<VariableChange> changes;
            variables_.handle_global_sync(message, changes);
            for (const VariableChange &change : changes) {
                Event event;
                event.type = EventType::GlobalVariableChanged;
                event.name = change.name;
                event.old_value = change.old_value;
                event.new_value = change.new_value;
                event.had_old_value = change.had_old_value;
                event.removed = change.removed;
                events.push_back(event);
            }
            return;
        }
        case MSG_CLIENT_VAR_SYNC: {
            ClientVarSyncMessage message;
            if (!deserialize_client_var_sync(payload.data(), payload.size(), message)) {
                push_log("malformed client variable sync message");
                return;
            }
            std::vector<VariableChange> changes;
            variables_.handle_client_sync(message, changes);
            for (const VariableChange &change : changes) {
                Event event;
                event.type = EventType::ClientVariableChanged;
                event.client_no = change.client_no;
                event.name = change.name;
                event.old_value = change.old_value;
                event.new_value = change.new_value;
                event.had_old_value = change.had_old_value;
                event.removed = change.removed;
                events.push_back(event);
            }
            return;
        }
        case MSG_OBJECT_OWNERSHIP_CHANGED: {
            ObjectOwnershipChangedMessage message;
            if (!deserialize_object_ownership_changed(payload.data(), payload.size(), message)) {
                push_log("malformed object ownership changed message");
                return;
            }
            OwnershipChange change;
            objects_.handle_ownership_changed(message, change);
            Event event;
            event.type = EventType::ObjectOwnershipChanged;
            event.object_id = change.object_id;
            event.value_a = change.new_owner_client_no;
            event.value_b = change.previous_owner_client_no;
            events.push_back(event);
            return;
        }
        case MSG_OBJECT_OWNERSHIP_REJECTED: {
            ObjectOwnershipRejectedMessage message;
            if (!deserialize_object_ownership_rejected(payload.data(), payload.size(), message)) {
                push_log("malformed object ownership rejected message");
                return;
            }
            OwnershipRejection rejection;
            objects_.handle_ownership_rejected(message, rejection);
            Event event;
            event.type = EventType::ObjectOwnershipRejected;
            event.object_id = rejection.object_id;
            event.value_a = rejection.current_owner_client_no;
            event.value_b = rejection.reason_code;
            events.push_back(event);
            return;
        }
        default:
            // Reserved and server-only message types are ignored, as upstream does.
            return;
    }
}

void NetSyncClient::process_room_pose(const std::vector<std::uint8_t> &payload,
                                      std::vector<Event> &events) {
    RoomPoseMessage message;
    if (!deserialize_room_pose(payload.data(), payload.size(), message)) {
        push_log("malformed room pose message");
        return;
    }
    if (message.room_id != config_.room_id) {
        return;
    }

    last_room_broadcast_time_ = message.broadcast_time;

    const int local = mapping_.local_client_no();
    std::vector<int> alive;
    alive.reserve(message.clients.size());
    std::map<int, RemoteClientPose> next_poses;

    for (const RoomPoseClient &client : message.clients) {
        const int client_no = static_cast<int>(client.client_no);
        if (client_no == local && local != 0) {
            continue;  // The local avatar is driven locally, never echoed back.
        }
        alive.push_back(client_no);

        RemoteClientPose pose;
        pose.client_no = client_no;
        pose.pose_time = client.pose_time;
        pose.broadcast_time = message.broadcast_time;
        pose.body = client.body;
        next_poses[client_no] = std::move(pose);
    }
    remote_poses_.swap(next_poses);

    std::vector<int> connected;
    std::vector<int> disconnected;
    mapping_.update_presence(alive, connected, disconnected);

    for (int client_no : connected) {
        Event event;
        event.type = EventType::AvatarConnected;
        event.client_no = client_no;
        event.name = mapping_.device_id_for(client_no);
        events.push_back(event);
    }
    for (int client_no : disconnected) {
        Event event;
        event.type = EventType::AvatarDisconnected;
        event.client_no = client_no;
        events.push_back(event);
    }
}

void NetSyncClient::process_room_objects(const std::vector<std::uint8_t> &payload,
                                         std::vector<Event> &events) {
    RoomObjectsMessage message;
    if (!deserialize_room_objects(payload.data(), payload.size(), message)) {
        push_log("malformed room objects message");
        return;
    }
    std::vector<OwnershipChange> changes;
    objects_.handle_room_objects(message, changes);
    for (const OwnershipChange &change : changes) {
        Event event;
        event.type = EventType::ObjectOwnershipChanged;
        event.object_id = change.object_id;
        event.value_a = change.new_owner_client_no;
        event.value_b = change.previous_owner_client_no;
        events.push_back(event);
    }
}

void NetSyncClient::set_local_pose(const ClientPoseBody &body) {
    local_pose_ = body;
    has_local_pose_ = true;
}

void NetSyncClient::clear_local_pose() {
    has_local_pose_ = false;
    has_last_pose_signature_ = false;
    last_pose_signature_.clear();
}

void NetSyncClient::send_local_pose_if_due(double now_seconds) {
    if (!transport_.is_running() || transport_.has_error()) {
        return;
    }

    ClientPoseBody body;
    if (config_.stealth_mode) {
        // A stealth client publishes only the handshake heartbeat, so the
        // server keeps its room entry alive without exposing an avatar.
        body.flags = POSE_FLAG_STEALTH;
    } else if (has_local_pose_) {
        body = local_pose_;
    } else {
        return;
    }

    const double send_interval = 1.0 / std::max(0.5, config_.transform_send_rate);
    if (now_seconds - last_pose_send_time_ < send_interval) {
        return;
    }

    const std::vector<std::uint8_t> signature = pose_signature(body);
    const bool unchanged = has_last_pose_signature_ && signature == last_pose_signature_;
    if (unchanged && now_seconds - last_pose_change_send_time_ < kPoseHeartbeatSeconds) {
        // Nothing moved and the heartbeat is not due: stay quiet.
        last_pose_send_time_ = now_seconds;
        return;
    }

    ++local_pose_seq_;
    body.pose_seq = local_pose_seq_;

    ClientPoseMessage message;
    message.device_id = config_.device_id;
    message.body = body;
    transport_.set_latest_transform(serialize_client_pose(message));

    last_pose_send_time_ = now_seconds;
    last_pose_change_send_time_ = now_seconds;
    last_pose_signature_ = signature;
    has_last_pose_signature_ = true;
}

bool NetSyncClient::get_remote_pose(int client_no, RemoteClientPose &out) const {
    const auto it = remote_poses_.find(client_no);
    if (it == remote_poses_.end()) {
        return false;
    }
    out = it->second;
    return true;
}

std::vector<int> NetSyncClient::remote_client_numbers() const {
    std::vector<int> out;
    out.reserve(remote_poses_.size());
    for (const auto &entry : remote_poses_) {
        out.push_back(entry.first);
    }
    return out;
}

// --- RPC ---------------------------------------------------------------------

void NetSyncClient::rpc(const std::string &function_name,
                        const std::vector<std::string> &args) {
    rpc_to_many(std::vector<int>(), function_name, args);
}

void NetSyncClient::rpc_to(int target_client_no, const std::string &function_name,
                           const std::vector<std::string> &args) {
    rpc_to_many(std::vector<int>{target_client_no}, function_name, args);
}

void NetSyncClient::rpc_to_many(const std::vector<int> &target_client_nos,
                                const std::string &function_name,
                                const std::vector<std::string> &args) {
    // `now` is taken from the transport's clock domain so the rate limiter and
    // the TTL agree with the queue timestamps even if the host's clock differs.
    using clock = std::chrono::steady_clock;
    const double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    rpc_manager_.send(now, is_ready(), mapping_.local_client_no(), config_.device_id,
                      target_client_nos, function_name, args);
}

void NetSyncClient::configure_rpc_rate_limit(int rpc_limit, double window_seconds,
                                             double warn_cooldown) {
    rpc_manager_.configure_rate_limit(rpc_limit, window_seconds, warn_cooldown);
}

// --- Network variables --------------------------------------------------------

bool NetSyncClient::set_global_variable(const std::string &name, const std::string &value) {
    using clock = std::chrono::steady_clock;
    const double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    return variables_.set_global(now, name, value);
}

std::string NetSyncClient::get_global_variable(const std::string &name,
                                               const std::string &fallback) const {
    std::string value;
    return variables_.get_global(name, value) ? value : fallback;
}

std::map<std::string, std::string> NetSyncClient::get_all_global_variables() const {
    return variables_.all_globals();
}

bool NetSyncClient::set_client_variable(const std::string &name, const std::string &value) {
    return set_client_variable_for(mapping_.local_client_no(), name, value);
}

bool NetSyncClient::set_client_variable_for(int target_client_no, const std::string &name,
                                            const std::string &value) {
    using clock = std::chrono::steady_clock;
    const double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    return variables_.set_client(now, target_client_no, name, value);
}

std::string NetSyncClient::get_client_variable(int client_no, const std::string &name,
                                               const std::string &fallback) const {
    std::string value;
    return variables_.get_client(client_no, name, value) ? value : fallback;
}

std::map<std::string, std::string> NetSyncClient::get_all_client_variables(int client_no) const {
    return variables_.all_client_variables(client_no);
}

bool NetSyncClient::clear_my_client_variables() {
    std::vector<VariableChange> changes;
    return variables_.clear_my_client_variables(changes);
}

// --- Objects ------------------------------------------------------------------

bool NetSyncClient::register_object(std::uint32_t object_id) {
    return objects_.register_object(object_id);
}

void NetSyncClient::unregister_object(std::uint32_t object_id) {
    objects_.unregister_object(object_id);
}

void NetSyncClient::submit_object_pose(std::uint32_t object_id, const PoseTransform &pose) {
    objects_.submit_local_pose(object_id, pose);
}

bool NetSyncClient::get_object_state(std::uint32_t object_id, ObjectState &out) const {
    return objects_.get_state(object_id, out);
}

bool NetSyncClient::request_object_ownership(std::uint32_t object_id) {
    return objects_.request_ownership(object_id);
}

bool NetSyncClient::release_object_ownership(std::uint32_t object_id) {
    return objects_.release_ownership(object_id);
}

}  // namespace netsync
}  // namespace styly
