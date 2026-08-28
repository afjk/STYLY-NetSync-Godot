// SPDX-License-Identifier: Apache-2.0
#include "object_sync_manager.hpp"

#include <algorithm>
#include <cmath>

namespace styly {
namespace netsync {

namespace {

bool poses_equal(const PoseTransform &a, const PoseTransform &b) {
    return a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z && a.rotation.x == b.rotation.x &&
           a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z &&
           a.rotation.w == b.rotation.w;
}

}  // namespace

void ObjectSyncManager::log(const std::string &message) {
    if (on_log_) {
        on_log_(message);
    }
}

bool ObjectSyncManager::register_object(std::uint32_t object_id) {
    if (object_id == 0u) {
        log("NetSyncObject has no object id assigned; skipping registration");
        return false;
    }
    Entry &entry = objects_[object_id];
    entry.state.object_id = object_id;
    return true;
}

void ObjectSyncManager::unregister_object(std::uint32_t object_id) { objects_.erase(object_id); }

bool ObjectSyncManager::is_registered(std::uint32_t object_id) const {
    return objects_.find(object_id) != objects_.end();
}

std::vector<std::uint32_t> ObjectSyncManager::registered_object_ids() const {
    std::vector<std::uint32_t> out;
    out.reserve(objects_.size());
    for (const auto &entry : objects_) {
        out.push_back(entry.first);
    }
    return out;
}

void ObjectSyncManager::submit_local_pose(std::uint32_t object_id, const PoseTransform &pose) {
    const auto it = objects_.find(object_id);
    if (it == objects_.end()) {
        return;
    }
    it->second.local_pose = pose;
    it->second.has_local_pose = true;
}

void ObjectSyncManager::tick(double now_seconds, double transform_send_rate) {
    if (local_client_no_ == 0 || !send_object_transform_) {
        return;
    }
    const double send_interval = 1.0 / std::max(0.5, transform_send_rate);

    for (auto &pair : objects_) {
        Entry &entry = pair.second;
        if (entry.state.owner_client_no != local_client_no_) {
            continue;
        }
        if (!entry.has_local_pose) {
            continue;
        }
        if (now_seconds - entry.last_send_time < send_interval) {
            continue;
        }
        // Only-on-change, with a heartbeat so a late joiner still learns the pose.
        if (entry.has_last_sent_pose && poses_equal(entry.local_pose, entry.last_sent_pose) &&
            now_seconds - entry.last_send_time < kHeartbeatIntervalSeconds) {
            continue;
        }

        ++entry.send_pose_seq;
        entry.last_send_time = now_seconds;
        entry.last_sent_pose = entry.local_pose;
        entry.has_last_sent_pose = true;

        ObjectPoseMessage message;
        message.device_id = local_device_id_;
        message.object_id = pair.first;
        message.pose_seq = entry.send_pose_seq;
        message.position = entry.local_pose.position;
        message.rotation = entry.local_pose.rotation;
        send_object_transform_(pair.first, serialize_object_pose(message));
    }
}

bool ObjectSyncManager::send_ownership_request(std::uint32_t object_id,
                                               std::uint8_t operation_type) {
    if (object_id == 0u || !send_control_) {
        return false;
    }
    ObjectOwnershipRequestMessage message;
    message.device_id = local_device_id_;
    message.operation_type = operation_type;
    message.object_id = object_id;
    return send_control_(serialize_object_ownership_request(message));
}

bool ObjectSyncManager::request_ownership(std::uint32_t object_id) {
    return send_ownership_request(object_id, OWNERSHIP_OP_REQUEST);
}

bool ObjectSyncManager::release_ownership(std::uint32_t object_id) {
    return send_ownership_request(object_id, OWNERSHIP_OP_RELEASE);
}

void ObjectSyncManager::handle_room_objects(const RoomObjectsMessage &message,
                                            std::vector<OwnershipChange> &changes) {
    for (const RoomObjectState &state : message.objects) {
        const auto it = objects_.find(state.object_id);
        if (it == objects_.end()) {
            continue;
        }
        Entry &entry = it->second;

        const int previous_owner = entry.state.owner_client_no;
        const int new_owner = static_cast<int>(state.owner_client_no);
        entry.state.owner_client_no = new_owner;
        if (previous_owner != new_owner) {
            OwnershipChange change;
            change.object_id = state.object_id;
            change.new_owner_client_no = new_owner;
            change.previous_owner_client_no = previous_owner;
            changes.push_back(change);
        }

        // Only non-owned objects take their pose from the broadcast; the owner
        // is authoritative for its own transform.
        if (new_owner != local_client_no_ || local_client_no_ == 0) {
            entry.state.pose_seq = state.pose_seq;
            entry.state.pose_time = state.pose_time;
            entry.state.broadcast_time = message.broadcast_time;
            entry.state.pose.position = state.position;
            entry.state.pose.rotation = state.rotation;
            entry.state.has_pose = true;
        }
    }
}

void ObjectSyncManager::handle_ownership_changed(const ObjectOwnershipChangedMessage &message,
                                                 OwnershipChange &change) {
    change.object_id = message.object_id;
    change.new_owner_client_no = static_cast<int>(message.new_owner_client_no);
    change.previous_owner_client_no = static_cast<int>(message.previous_owner_client_no);

    const auto it = objects_.find(message.object_id);
    if (it == objects_.end()) {
        return;
    }
    it->second.state.owner_client_no = change.new_owner_client_no;
    if (change.new_owner_client_no == local_client_no_ && local_client_no_ != 0) {
        // Taking ownership: forget the previous send state so the first pose
        // this client publishes is not suppressed by the only-on-change check.
        it->second.has_last_sent_pose = false;
        it->second.last_send_time = -1e9;
    }
}

void ObjectSyncManager::handle_ownership_rejected(const ObjectOwnershipRejectedMessage &message,
                                                  OwnershipRejection &rejection) {
    rejection.object_id = message.object_id;
    rejection.current_owner_client_no = static_cast<int>(message.current_owner_client_no);
    rejection.reason_code = static_cast<int>(message.reason_code);
}

bool ObjectSyncManager::get_state(std::uint32_t object_id, ObjectState &out) const {
    const auto it = objects_.find(object_id);
    if (it == objects_.end()) {
        return false;
    }
    out = it->second.state;
    return true;
}

int ObjectSyncManager::owner_of(std::uint32_t object_id) const {
    const auto it = objects_.find(object_id);
    return it == objects_.end() ? 0 : it->second.state.owner_client_no;
}

bool ObjectSyncManager::is_owned_by_local(std::uint32_t object_id) const {
    const int owner = owner_of(object_id);
    return owner != 0 && owner == local_client_no_;
}

void ObjectSyncManager::clear_room_scoped_state() {
    for (auto &pair : objects_) {
        Entry &entry = pair.second;
        entry.state.owner_client_no = 0;
        entry.state.has_pose = false;
        entry.state.pose_seq = 0;
        entry.state.pose_time = 0.0;
        entry.state.broadcast_time = 0.0;
        entry.has_last_sent_pose = false;
        entry.last_send_time = -1e9;
        entry.send_pose_seq = 0;
    }
}

}  // namespace netsync
}  // namespace styly
