// SPDX-License-Identifier: Apache-2.0
// The STYLY NetSync client: lifecycle, message routing and state.
//
// Everything here is expressed in wire (NetSync/Unity) coordinates and plain
// C++ types. The Godot layer in src/godot wraps this and is the only place that
// converts coordinates or touches Godot objects.
//
// Threading contract
// ------------------
// The transport's network thread only ever *enqueues* raw payloads and
// connection notifications. Decoding, state mutation and event production all
// happen inside poll(), on whatever thread the host calls it from (for Godot,
// the main thread). Nothing in this class calls back into the host from the
// network thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/device_mapping.hpp"
#include "core/network_variable_manager.hpp"
#include "core/object_sync_manager.hpp"
#include "core/rpc_manager.hpp"
#include "protocol/protocol_v8.hpp"
#include "transport/server_discovery.hpp"
#include "transport/zmq_transport.hpp"

namespace styly {
namespace netsync {

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Synchronizing,
    Ready,
    Error,
};

const char *connection_state_name(ConnectionState state);

enum class EventType {
    ConnectionStateChanged,
    Ready,
    ConnectionError,
    ServerDiscovered,
    ClientNoAssigned,
    AvatarConnected,
    AvatarDisconnected,
    RpcReceived,
    GlobalVariableChanged,
    ClientVariableChanged,
    ObjectOwnershipChanged,
    ObjectOwnershipRejected,
    VersionMismatch,
    Log,
};

/// A single thing that happened, produced by poll() for the host to dispatch.
struct Event {
    EventType type = EventType::Log;
    /// AvatarConnected/Disconnected, ClientNoAssigned, RpcReceived (sender),
    /// ClientVariableChanged (owner).
    int client_no = 0;
    /// ObjectOwnershipChanged: new owner. ObjectOwnershipRejected: current owner.
    /// VersionMismatch: server major. ConnectionStateChanged: the new state.
    int value_a = 0;
    /// ObjectOwnershipChanged: previous owner. ObjectOwnershipRejected: reason.
    /// VersionMismatch: server minor.
    int value_b = 0;
    /// VersionMismatch: server patch.
    int value_c = 0;
    std::uint32_t object_id = 0;
    /// Variable name, RPC function name, or log/error text.
    std::string name;
    std::string old_value;
    std::string new_value;
    /// True when the variable had a previous value (so `old_value` is meaningful).
    bool had_old_value = false;
    /// True when a variable was removed by an authoritative snapshot.
    bool removed = false;
    std::vector<std::string> args;
};

struct ClientConfig {
    /// Empty enables LAN discovery; otherwise "host" or "tcp://host".
    std::string server_address;
    int control_port = kDefaultControlPort;
    int transform_port = kDefaultTransformPort;
    int sub_port = kDefaultSubPort;
    int discovery_port = kDefaultDiscoveryPort;
    std::string room_id = "default_room";
    std::string device_id;
    /// A stealth client has no avatar: it registers on the control lane and
    /// sends a stealth pose heartbeat, but no transform-valid pose data.
    bool stealth_mode = false;
    /// Pose sends per second. Clamped to [0.5, 60] like upstream.
    double transform_send_rate = 10.0;
    /// When discovery is disabled and no address is set, connect() fails.
    bool enable_discovery = true;
};

/// Latest known pose of one remote client.
struct RemoteClientPose {
    int client_no = 0;
    double pose_time = 0.0;
    /// Server broadcast time of the frame that carried this pose.
    double broadcast_time = 0.0;
    ClientPoseBody body;
};

class NetSyncClient {
public:
    NetSyncClient();
    ~NetSyncClient();

    NetSyncClient(const NetSyncClient &) = delete;
    NetSyncClient &operator=(const NetSyncClient &) = delete;

    /// Begin connecting. When `config.server_address` is empty and discovery is
    /// enabled, discovery runs first and the transport starts once a server
    /// answers. Returns false when already connecting/connected or when neither
    /// an address nor discovery is available.
    bool connect(const ClientConfig &config);

    /// Tear everything down: stop discovery, stop the network thread, close the
    /// sockets, clear room-scoped state. Safe to call when disconnected.
    void disconnect();

    /// Drive the client. Call once per frame with a monotonically increasing
    /// clock in seconds. Returns the events produced by this call.
    std::vector<Event> poll(double now_seconds);

    // --- State ---------------------------------------------------------------

    ConnectionState state() const { return state_; }
    bool is_connected() const;
    bool is_ready() const;
    bool has_handshake() const { return mapping_.local_client_no() > 0; }
    bool has_network_variable_sync() const { return variables_.has_received_initial_sync(); }
    int client_no() const { return mapping_.local_client_no(); }
    const std::string &device_id() const { return config_.device_id; }
    const std::string &room_id() const { return config_.room_id; }
    /// Resolved server address; empty until discovery completes.
    const std::string &server_address() const { return resolved_address_; }
    int discovered_rest_api_port() const { return discovered_rest_api_port_; }
    const ClientConfig &config() const { return config_; }

    void set_transform_send_rate(double rate);
    double transform_send_rate() const { return config_.transform_send_rate; }

    TransportStats transport_stats() const { return transport_.stats(); }

    // --- Local pose ----------------------------------------------------------

    /// Provide the local avatar pose in wire coordinates. Sending is rate
    /// limited and change-gated inside poll(); the pose sequence number is
    /// managed here, so callers must leave `body.pose_seq` alone.
    void set_local_pose(const ClientPoseBody &body);

    /// Drop the local pose so nothing further is transmitted for this avatar.
    void clear_local_pose();

    // --- Remote poses --------------------------------------------------------

    bool get_remote_pose(int client_no, RemoteClientPose &out) const;
    std::vector<int> remote_client_numbers() const;
    /// Server broadcast time of the most recent room-pose frame.
    double last_room_broadcast_time() const { return last_room_broadcast_time_; }

    // --- Device mapping ------------------------------------------------------

    std::string device_id_for(int client_no) const { return mapping_.device_id_for(client_no); }
    int client_no_for(const std::string &device_id) const {
        return mapping_.client_no_for(device_id);
    }
    bool is_client_stealth(int client_no) const { return mapping_.is_stealth(client_no); }
    std::vector<int> known_client_numbers() const { return mapping_.known_client_numbers(); }

    // --- RPC -----------------------------------------------------------------

    void rpc(const std::string &function_name, const std::vector<std::string> &args);
    void rpc_to(int target_client_no, const std::string &function_name,
                const std::vector<std::string> &args);
    void rpc_to_many(const std::vector<int> &target_client_nos, const std::string &function_name,
                     const std::vector<std::string> &args);
    void configure_rpc_rate_limit(int rpc_limit, double window_seconds, double warn_cooldown);

    // --- Network variables ---------------------------------------------------

    bool set_global_variable(const std::string &name, const std::string &value);
    std::string get_global_variable(const std::string &name, const std::string &fallback) const;
    std::map<std::string, std::string> get_all_global_variables() const;

    bool set_client_variable(const std::string &name, const std::string &value);
    bool set_client_variable_for(int target_client_no, const std::string &name,
                                 const std::string &value);
    std::string get_client_variable(int client_no, const std::string &name,
                                    const std::string &fallback) const;
    std::map<std::string, std::string> get_all_client_variables(int client_no) const;
    bool clear_my_client_variables();

    // --- Object sync ---------------------------------------------------------

    bool register_object(std::uint32_t object_id);
    void unregister_object(std::uint32_t object_id);
    void submit_object_pose(std::uint32_t object_id, const PoseTransform &pose);
    bool get_object_state(std::uint32_t object_id, ObjectState &out) const;
    bool request_object_ownership(std::uint32_t object_id);
    bool release_object_ownership(std::uint32_t object_id);

private:
    void set_state(ConnectionState state, std::vector<Event> &events);
    void start_transport(const std::string &address, int control_port, int transform_port,
                         int sub_port);
    void enqueue_hello();
    void handle_payload(TransportLane lane, const std::uint8_t *data, std::size_t size);
    void process_control_payload(const std::vector<std::uint8_t> &payload,
                                 std::vector<Event> &events);
    void process_room_pose(const std::vector<std::uint8_t> &payload, std::vector<Event> &events);
    void process_room_objects(const std::vector<std::uint8_t> &payload,
                              std::vector<Event> &events);
    void send_local_pose_if_due(double now_seconds);
    void push_log(const std::string &message);
    void drain_internal_events(std::vector<Event> &events);
    void update_ready(std::vector<Event> &events);
    void reset_session_state();

    ClientConfig config_;
    std::string resolved_address_;
    int discovered_rest_api_port_ = 0;

    ZmqTransport transport_;
    ServerDiscovery discovery_;

    DeviceMapping mapping_;
    RpcManager rpc_manager_;
    NetworkVariableManager variables_;
    ObjectSyncManager objects_;

    ConnectionState state_ = ConnectionState::Disconnected;
    bool ready_fired_ = false;
    bool version_checked_ = false;
    bool hello_enqueued_ = false;

    // --- Cross-thread inboxes ------------------------------------------------
    // Written by the network/discovery threads, drained by poll().

    static constexpr std::size_t kControlInboxMax = 4096;

    mutable std::mutex inbox_mutex_;
    std::deque<std::vector<std::uint8_t>> control_inbox_;
    std::vector<std::uint8_t> latest_room_pose_;
    bool has_room_pose_ = false;
    std::vector<std::uint8_t> latest_room_objects_;
    bool has_room_objects_ = false;
    std::uint64_t control_inbox_dropped_ = 0;

    struct PendingNotification {
        enum class Kind { Connected, Error, Log, Discovered } kind = Kind::Log;
        std::string text;
        DiscoveredServer server;
    };
    std::deque<PendingNotification> notifications_;

    // --- Local pose ----------------------------------------------------------

    bool has_local_pose_ = false;
    ClientPoseBody local_pose_;
    std::uint16_t local_pose_seq_ = 0;
    double last_pose_send_time_ = -1e9;
    double last_pose_change_send_time_ = -1e9;
    bool has_last_pose_signature_ = false;
    std::vector<std::uint8_t> last_pose_signature_;

    // --- Remote poses --------------------------------------------------------

    std::map<int, RemoteClientPose> remote_poses_;
    double last_room_broadcast_time_ = 0.0;
};

}  // namespace netsync
}  // namespace styly
