// SPDX-License-Identifier: Apache-2.0
// NetSyncObject transform sync and ownership.
//
// Mirrors the upstream Unity ObjectSyncManager: send-rate limiting with a 1 s
// heartbeat, only-on-change suppression, per-object pose sequence numbers, and
// per-object latest-wins delivery to the transport.
//
// Poses handled here are already in wire coordinates; the Godot bridge converts.
// Main-thread only.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "protocol/protocol_v8.hpp"

namespace styly {
namespace netsync {

/// Everything the caller needs to drive one synchronised object.
struct ObjectState {
    std::uint32_t object_id = 0;
    int owner_client_no = 0;
    std::uint16_t pose_seq = 0;
    double pose_time = 0.0;
    /// Server broadcast time of the frame that carried this pose.
    double broadcast_time = 0.0;
    PoseTransform pose;
    bool has_pose = false;
};

struct OwnershipChange {
    std::uint32_t object_id = 0;
    int new_owner_client_no = 0;
    int previous_owner_client_no = 0;
};

struct OwnershipRejection {
    std::uint32_t object_id = 0;
    int current_owner_client_no = 0;
    int reason_code = 0;
};

class ObjectSyncManager {
public:
    using ControlSender = std::function<bool(std::vector<std::uint8_t>)>;
    /// Per-object latest-wins hand-off to the transform lane.
    using ObjectTransformSender =
        std::function<void(std::uint32_t, std::vector<std::uint8_t>)>;
    using LogCallback = std::function<void(const std::string &)>;

    /// Upstream heartbeat: an unchanged owned object is still re-sent this often.
    static constexpr double kHeartbeatIntervalSeconds = 1.0;

    void set_control_sender(ControlSender sender) { send_control_ = std::move(sender); }
    void set_object_transform_sender(ObjectTransformSender sender) {
        send_object_transform_ = std::move(sender);
    }
    void set_log_callback(LogCallback callback) { on_log_ = std::move(callback); }

    void set_local_identity(int client_no, std::string device_id) {
        local_client_no_ = client_no;
        local_device_id_ = std::move(device_id);
    }

    /// Register an object so its state is tracked and its poses are relayed.
    /// `object_id == 0` is rejected, matching upstream.
    bool register_object(std::uint32_t object_id);
    void unregister_object(std::uint32_t object_id);
    bool is_registered(std::uint32_t object_id) const;
    std::vector<std::uint32_t> registered_object_ids() const;

    /// Record the owner's current pose. Sending is decided in `tick`.
    void submit_local_pose(std::uint32_t object_id, const PoseTransform &pose);

    /// Send due poses for objects this client owns.
    void tick(double now_seconds, double transform_send_rate);

    bool request_ownership(std::uint32_t object_id);
    bool release_ownership(std::uint32_t object_id);

    /// Apply a room-objects broadcast. Ownership transitions are appended to
    /// `changes` so the caller can surface them even when they arrive only via
    /// the periodic snapshot.
    void handle_room_objects(const RoomObjectsMessage &message,
                             std::vector<OwnershipChange> &changes);
    void handle_ownership_changed(const ObjectOwnershipChangedMessage &message,
                                  OwnershipChange &change);
    void handle_ownership_rejected(const ObjectOwnershipRejectedMessage &message,
                                   OwnershipRejection &rejection);

    bool get_state(std::uint32_t object_id, ObjectState &out) const;
    int owner_of(std::uint32_t object_id) const;
    bool is_owned_by_local(std::uint32_t object_id) const;

    /// Reset ownership and send state without forgetting the registrations.
    void clear_room_scoped_state();

private:
    struct Entry {
        ObjectState state;
        bool has_local_pose = false;
        PoseTransform local_pose;
        double last_send_time = -1e9;
        std::uint16_t send_pose_seq = 0;
        bool has_last_sent_pose = false;
        PoseTransform last_sent_pose;
    };

    bool send_ownership_request(std::uint32_t object_id, std::uint8_t operation_type);
    void log(const std::string &message);

    ControlSender send_control_;
    ObjectTransformSender send_object_transform_;
    LogCallback on_log_;

    int local_client_no_ = 0;
    std::string local_device_id_;

    std::map<std::uint32_t, Entry> objects_;
};

}  // namespace netsync
}  // namespace styly
