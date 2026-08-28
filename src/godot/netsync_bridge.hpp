// SPDX-License-Identifier: Apache-2.0
// GDExtension binding for the STYLY NetSync client.
//
// This is the only file that knows about both Godot and the client core. It:
//   * converts Godot types (Transform3D, Vector3, Quaternion) to and from wire
//     coordinates through CoordinateConverter, and nowhere else;
//   * turns core events into Godot signals, on the thread that calls poll();
//   * keeps every Godot API call on the caller's thread — the network thread
//     never touches an Object.
//
// The GDScript addon in addons/styly_netsync wraps this in a Node with a
// Godot-idiomatic API. Nothing here creates or drives a Node itself.
#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <memory>

#include "core/netsync_client.hpp"

namespace styly {
namespace netsync {

class NetSyncBridge : public godot::RefCounted {
    GDCLASS(NetSyncBridge, godot::RefCounted)

public:
    /// Mirrors styly::netsync::ConnectionState so GDScript can compare against
    /// named constants instead of bare integers.
    enum State {
        STATE_DISCONNECTED = 0,
        STATE_CONNECTING = 1,
        STATE_CONNECTED = 2,
        STATE_SYNCHRONIZING = 3,
        STATE_READY = 4,
        STATE_ERROR = 5,
    };

    NetSyncBridge();
    ~NetSyncBridge() override;

    // --- Lifecycle -----------------------------------------------------------

    /// Connect using a configuration dictionary. Recognised keys:
    ///   server_address (String, empty enables discovery), control_port,
    ///   transform_port, sub_port, discovery_port (int), room_id (String),
    ///   device_id (String), stealth_mode (bool), transform_send_rate (float),
    ///   enable_discovery (bool).
    bool connect_to_server(const godot::Dictionary &config);
    void disconnect_from_server();

    /// Drain the client and emit the resulting signals. Call once per frame
    /// from the main thread.
    void poll();

    bool is_connected_to_server() const;
    bool is_ready() const;
    bool has_handshake() const;
    bool has_network_variable_sync() const;
    int get_connection_state() const;
    godot::String get_connection_state_name() const;

    int get_client_no() const;
    godot::String get_device_id() const;
    godot::String get_room_id() const;
    godot::String get_server_address() const;
    int get_discovered_rest_api_port() const;

    void set_transform_send_rate(double rate);
    double get_transform_send_rate() const;

    godot::Dictionary get_transport_stats() const;

    // --- Local pose ----------------------------------------------------------

    /// Set the local avatar pose from Godot-space transforms. Recognised keys:
    ///   head, right_hand, left_hand, physical (Transform3D)
    ///   virtuals (Array of Transform3D)
    ///   xr_origin_delta_position (Vector3), xr_origin_delta_yaw (float, degrees)
    ///   moving_floor_local (bool), physical_valid (bool)
    /// A body part is transmitted only when its key is present.
    void set_local_pose(const godot::Dictionary &pose);
    void clear_local_pose();

    // --- Remote state --------------------------------------------------------

    /// Latest pose of a remote client, in Godot space. Empty when unknown.
    /// Keys: client_no, pose_time, broadcast_time, flags, head, right_hand,
    /// left_hand, physical (Transform3D), virtuals (Array), has_head,
    /// has_right_hand, has_left_hand, has_virtuals, is_stealth.
    godot::Dictionary get_remote_pose(int client_no) const;
    godot::PackedInt32Array get_remote_client_numbers() const;
    godot::PackedInt32Array get_known_client_numbers() const;
    godot::String get_device_id_for(int client_no) const;
    int get_client_no_for(const godot::String &device_id) const;
    bool is_client_stealth(int client_no) const;
    double get_last_room_broadcast_time() const;

    // --- RPC -----------------------------------------------------------------

    void send_rpc(const godot::String &function_name, const godot::PackedStringArray &args);
    void send_rpc_to(int target_client_no, const godot::String &function_name,
                     const godot::PackedStringArray &args);
    void send_rpc_to_many(const godot::PackedInt32Array &target_client_nos,
                          const godot::String &function_name,
                          const godot::PackedStringArray &args);
    void configure_rpc_rate_limit(int rpc_limit, double window_seconds, double warn_cooldown);

    // --- Network variables ---------------------------------------------------

    bool set_global_variable(const godot::String &name, const godot::String &value);
    godot::String get_global_variable(const godot::String &name,
                                      const godot::String &fallback) const;
    godot::Dictionary get_all_global_variables() const;

    bool set_client_variable(const godot::String &name, const godot::String &value);
    bool set_client_variable_for(int target_client_no, const godot::String &name,
                                 const godot::String &value);
    godot::String get_client_variable(int client_no, const godot::String &name,
                                      const godot::String &fallback) const;
    godot::Dictionary get_all_client_variables(int client_no) const;
    bool clear_my_client_variables();

    // --- Object sync ---------------------------------------------------------

    bool register_object(int64_t object_id);
    void unregister_object(int64_t object_id);
    /// Submit the owner's current transform, in Godot space.
    void submit_object_pose(int64_t object_id, const godot::Transform3D &transform);
    /// Keys: found, object_id, owner_client_no, pose_seq, pose_time,
    /// broadcast_time, has_pose, transform (Godot space), is_owned_by_me.
    godot::Dictionary get_object_state(int64_t object_id) const;
    bool request_object_ownership(int64_t object_id);
    bool release_object_ownership(int64_t object_id);

    // --- Utilities -----------------------------------------------------------

    /// Random UUID v4, for use as a device id.
    static godot::String generate_device_id();
    /// Read a persisted device id from an absolute filesystem path, creating one
    /// when absent. The GDScript layer resolves `user://` before calling.
    static godot::String load_or_create_device_id(const godot::String &path);
    /// 32-bit FNV-1a of a name, for deriving a stable object id in Godot-only
    /// scenes. Never use this to match a Unity object: set the same explicit id
    /// on both sides instead.
    static int64_t hash_object_id(const godot::String &name);

    /// Convert a Godot transform to the wire convention and back, exposed so a
    /// project can verify the boundary or convert a value it holds itself.
    static godot::Transform3D godot_to_netsync_transform(const godot::Transform3D &transform);
    static godot::Transform3D netsync_to_godot_transform(const godot::Transform3D &transform);

protected:
    static void _bind_methods();

private:
    void emit_event(const Event &event);

    std::unique_ptr<NetSyncClient> client_;
};

}  // namespace netsync
}  // namespace styly

VARIANT_ENUM_CAST(styly::netsync::NetSyncBridge::State);
