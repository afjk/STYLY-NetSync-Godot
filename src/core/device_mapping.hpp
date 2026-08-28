// SPDX-License-Identifier: Apache-2.0
// Client-number ↔ device-id bookkeeping.
//
// The server never sends a dedicated "welcome" message: a client learns its own
// number by finding its device id in MSG_DEVICE_ID_MAPPING. This class owns that
// table plus the presence tracking derived from MSG_ROOM_POSE.
//
// Main-thread only.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "protocol/protocol_v8.hpp"

namespace styly {
namespace netsync {

class DeviceMapping {
public:
    /// Result of applying a mapping message.
    struct ApplyResult {
        bool local_client_no_changed = false;
        int local_client_no = 0;
        bool server_version_seen = false;
        int server_version_major = 0;
        int server_version_minor = 0;
        int server_version_patch = 0;
    };

    void set_local_device_id(std::string device_id) { local_device_id_ = std::move(device_id); }
    const std::string &local_device_id() const { return local_device_id_; }

    int local_client_no() const { return local_client_no_; }
    void set_local_client_no(int client_no) { local_client_no_ = client_no; }

    /// Replace the whole table, as upstream does on every mapping message.
    ApplyResult apply(const DeviceIdMappingMessage &message);

    std::string device_id_for(int client_no) const;
    int client_no_for(const std::string &device_id) const;
    bool is_stealth(int client_no) const;
    std::vector<int> known_client_numbers() const;

    /// Update presence from a room-pose snapshot.
    ///
    /// `alive` is every client number present in the snapshot except the local
    /// one. Newly seen numbers land in `connected`, numbers that vanished land
    /// in `disconnected`. A client is only announced once it has a device-id
    /// mapping, matching upstream's pending-client behaviour.
    void update_presence(const std::vector<int> &alive, std::vector<int> &connected,
                         std::vector<int> &disconnected);

    /// Clear every room-scoped table. Called on room switch and disconnect.
    void clear();

    const std::set<int> &connected_clients() const { return announced_clients_; }

private:
    std::string local_device_id_;
    int local_client_no_ = 0;

    std::map<int, std::string> client_no_to_device_id_;
    std::map<std::string, int> device_id_to_client_no_;
    std::map<int, bool> client_no_to_stealth_;

    /// Seen in a room snapshot but not yet announced (no mapping yet).
    std::set<int> pending_clients_;
    /// Announced through an avatar_connected event and not yet disconnected.
    std::set<int> announced_clients_;
};

}  // namespace netsync
}  // namespace styly
