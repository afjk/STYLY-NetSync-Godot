// SPDX-License-Identifier: Apache-2.0
#include "device_mapping.hpp"

#include <algorithm>

namespace styly {
namespace netsync {

DeviceMapping::ApplyResult DeviceMapping::apply(const DeviceIdMappingMessage &message) {
    ApplyResult result;
    result.server_version_seen = true;
    result.server_version_major = message.server_version_major;
    result.server_version_minor = message.server_version_minor;
    result.server_version_patch = message.server_version_patch;

    client_no_to_device_id_.clear();
    device_id_to_client_no_.clear();
    client_no_to_stealth_.clear();

    for (const DeviceIdMappingEntry &entry : message.mappings) {
        const int client_no = static_cast<int>(entry.client_no);
        client_no_to_device_id_[client_no] = entry.device_id;
        device_id_to_client_no_[entry.device_id] = client_no;
        client_no_to_stealth_[client_no] = entry.is_stealth;

        if (!local_device_id_.empty() && entry.device_id == local_device_id_) {
            if (local_client_no_ != client_no) {
                local_client_no_ = client_no;
                result.local_client_no_changed = true;
            }
        }
    }
    result.local_client_no = local_client_no_;
    return result;
}

std::string DeviceMapping::device_id_for(int client_no) const {
    const auto it = client_no_to_device_id_.find(client_no);
    return it == client_no_to_device_id_.end() ? std::string() : it->second;
}

int DeviceMapping::client_no_for(const std::string &device_id) const {
    const auto it = device_id_to_client_no_.find(device_id);
    return it == device_id_to_client_no_.end() ? 0 : it->second;
}

bool DeviceMapping::is_stealth(int client_no) const {
    const auto it = client_no_to_stealth_.find(client_no);
    return it != client_no_to_stealth_.end() && it->second;
}

std::vector<int> DeviceMapping::known_client_numbers() const {
    std::vector<int> out;
    out.reserve(client_no_to_device_id_.size());
    for (const auto &entry : client_no_to_device_id_) {
        out.push_back(entry.first);
    }
    return out;
}

void DeviceMapping::update_presence(const std::vector<int> &alive, std::vector<int> &connected,
                                    std::vector<int> &disconnected) {
    connected.clear();
    disconnected.clear();

    const std::set<int> alive_set(alive.begin(), alive.end());

    // A client is announced only once its device id is known, so a listener can
    // rely on avatar_connected carrying one. Until then it waits in
    // `pending_clients_`, which is how upstream handles a pose frame that beats
    // its ID-mapping message.
    for (int client_no : alive_set) {
        if (announced_clients_.count(client_no) != 0) {
            continue;
        }
        if (client_no_to_device_id_.count(client_no) != 0) {
            announced_clients_.insert(client_no);
            pending_clients_.erase(client_no);
            connected.push_back(client_no);
        } else {
            pending_clients_.insert(client_no);
        }
    }

    // Forget anything pending that has since left the room.
    for (auto it = pending_clients_.begin(); it != pending_clients_.end();) {
        it = alive_set.count(*it) == 0 ? pending_clients_.erase(it) : std::next(it);
    }

    for (auto it = announced_clients_.begin(); it != announced_clients_.end();) {
        if (alive_set.count(*it) == 0) {
            disconnected.push_back(*it);
            it = announced_clients_.erase(it);
        } else {
            ++it;
        }
    }
}

void DeviceMapping::clear() {
    client_no_to_device_id_.clear();
    device_id_to_client_no_.clear();
    client_no_to_stealth_.clear();
    pending_clients_.clear();
    announced_clients_.clear();
    local_client_no_ = 0;
}

}  // namespace netsync
}  // namespace styly
