// SPDX-License-Identifier: Apache-2.0
#include "network_variable_manager.hpp"

#include <algorithm>
#include <vector>

namespace styly {
namespace netsync {

namespace {

/// Count Unicode code points, matching the upstream length checks that operate
/// on characters rather than bytes.
std::size_t code_point_length(const std::string &value) {
    std::size_t count = 0;
    for (unsigned char c : value) {
        if ((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

}  // namespace

void NetworkVariableManager::log(const std::string &message) {
    if (on_log_) {
        on_log_(message);
    }
}

void NetworkVariableManager::reset_initial_sync() {
    has_received_initial_sync_ = false;
    connection_established_ = false;
    connection_established_at_ = 0.0;
}

void NetworkVariableManager::on_connection_established(double now_seconds) {
    connection_established_ = true;
    connection_established_at_ = now_seconds;
}

bool NetworkVariableManager::check_initial_sync_timeout(double now_seconds) {
    if (has_received_initial_sync_ || !connection_established_) {
        return false;
    }
    if (now_seconds - connection_established_at_ < kInitialSyncTimeout) {
        return false;
    }
    has_received_initial_sync_ = true;
    log("network variable initial sync timed out after " + std::to_string(kInitialSyncTimeout) +
        "s — proceeding without variables");
    return true;
}

bool NetworkVariableManager::validate_name(const std::string &name) const {
    if (name.empty() || code_point_length(name) > kMaxVariableNameChars) {
        return false;
    }
    return true;
}

bool NetworkVariableManager::validate_value(const std::string &value) const {
    return code_point_length(value) <= kMaxVariableValueChars;
}

// --- Global ------------------------------------------------------------------

bool NetworkVariableManager::try_send_global_now(const std::string &name,
                                                 const std::string &value) {
    if (!send_) {
        return false;
    }
    GlobalVarSetMessage message;
    message.sender_client_no =
        static_cast<std::uint16_t>(local_client_no_ < 0 ? 0 : local_client_no_);
    message.device_id = local_device_id_;
    message.variable_name = name;
    message.variable_value = value;
    if (!send_(serialize_global_var_set(message))) {
        return false;
    }
    last_sent_global_[name] = value;
    return true;
}

bool NetworkVariableManager::set_global(double now_seconds, const std::string &name,
                                        const std::string &value) {
    if (!validate_name(name)) {
        log("invalid global variable name: must be 1-" + std::to_string(kMaxVariableNameChars) +
            " characters");
        return false;
    }
    if (!validate_value(value)) {
        log("invalid global variable value: must be at most " +
            std::to_string(kMaxVariableValueChars) + " characters");
        return false;
    }
    if (globals_.size() >= kMaxGlobalVars && globals_.find(name) == globals_.end()) {
        log("global variable limit (" + std::to_string(kMaxGlobalVars) + ") reached");
        return false;
    }

    // Dedupe against what is actually on the wire, and cancel a now-stale
    // trailing value: the incoming value is the most recent intent, so any
    // surviving pending entry is an older one that would wrongly overwrite it.
    const auto last_sent = last_sent_global_.find(name);
    if (last_sent != last_sent_global_.end() && last_sent->second == value) {
        pending_global_.erase(name);
        due_global_.erase(name);
        return true;
    }

    const auto next_allowed = next_allowed_global_.find(name);
    const bool allow_immediate =
        next_allowed == next_allowed_global_.end() || now_seconds >= next_allowed->second;

    if (allow_immediate) {
        if (try_send_global_now(name, value)) {
            next_allowed_global_[name] = now_seconds + kDebounceInterval;
        }
        pending_global_[name] = value;
        if (due_global_.find(name) == due_global_.end()) {
            due_global_[name] = now_seconds + kDebounceInterval;
        }
        return true;
    }

    // Inside the cooldown: update the pending value, keep the original deadline.
    pending_global_[name] = value;
    if (due_global_.find(name) == due_global_.end()) {
        due_global_[name] = next_allowed->second;
    }
    return true;
}

bool NetworkVariableManager::get_global(const std::string &name, std::string &out) const {
    const auto it = globals_.find(name);
    if (it == globals_.end()) {
        return false;
    }
    out = it->second;
    return true;
}

// --- Client ------------------------------------------------------------------

bool NetworkVariableManager::try_send_client_now(int target_client_no, const std::string &name,
                                                 const std::string &value) {
    if (!send_) {
        return false;
    }
    ClientVarSetMessage message;
    message.sender_client_no =
        static_cast<std::uint16_t>(local_client_no_ < 0 ? 0 : local_client_no_);
    message.device_id = local_device_id_;
    message.target_client_no =
        static_cast<std::uint16_t>(target_client_no < 0 ? 0 : target_client_no);
    message.variable_name = name;
    message.variable_value = value;
    if (!send_(serialize_client_var_set(message))) {
        return false;
    }
    last_sent_client_[ClientKey(target_client_no, name)] = value;
    return true;
}

bool NetworkVariableManager::set_client(double now_seconds, int target_client_no,
                                        const std::string &name, const std::string &value) {
    if (!validate_name(name)) {
        log("invalid client variable name: must be 1-" + std::to_string(kMaxVariableNameChars) +
            " characters");
        return false;
    }
    if (!validate_value(value)) {
        log("invalid client variable value: must be at most " +
            std::to_string(kMaxVariableValueChars) + " characters");
        return false;
    }

    std::map<std::string, std::string> &variables = client_variables_[target_client_no];
    if (variables.size() >= kMaxClientVars && variables.find(name) == variables.end()) {
        log("client variable limit (" + std::to_string(kMaxClientVars) + ") reached for client " +
            std::to_string(target_client_no));
        return false;
    }

    const ClientKey key(target_client_no, name);
    const auto last_sent = last_sent_client_.find(key);
    if (last_sent != last_sent_client_.end() && last_sent->second == value) {
        pending_client_.erase(key);
        due_client_.erase(key);
        return true;
    }

    const auto next_allowed = next_allowed_client_.find(key);
    const bool allow_immediate =
        next_allowed == next_allowed_client_.end() || now_seconds >= next_allowed->second;

    if (allow_immediate) {
        if (try_send_client_now(target_client_no, name, value)) {
            next_allowed_client_[key] = now_seconds + kDebounceInterval;
        }
        pending_client_[key] = value;
        if (due_client_.find(key) == due_client_.end()) {
            due_client_[key] = now_seconds + kDebounceInterval;
        }
        return true;
    }

    pending_client_[key] = value;
    if (due_client_.find(key) == due_client_.end()) {
        due_client_[key] = next_allowed->second;
    }
    return true;
}

bool NetworkVariableManager::get_client(int client_no, const std::string &name,
                                        std::string &out) const {
    const auto client = client_variables_.find(client_no);
    if (client == client_variables_.end()) {
        return false;
    }
    const auto variable = client->second.find(name);
    if (variable == client->second.end()) {
        return false;
    }
    out = variable->second;
    return true;
}

std::map<std::string, std::string> NetworkVariableManager::all_client_variables(
    int client_no) const {
    const auto it = client_variables_.find(client_no);
    return it == client_variables_.end() ? std::map<std::string, std::string>() : it->second;
}

bool NetworkVariableManager::clear_my_client_variables(std::vector<VariableChange> &changes) {
    if (local_client_no_ <= 0 || !send_) {
        return false;
    }
    ClientVarClearMessage message;
    message.sender_client_no = static_cast<std::uint16_t>(local_client_no_);
    message.device_id = local_device_id_;
    if (!send_(serialize_client_var_clear(message))) {
        return false;
    }

    // Optimistically clear locally; the server's next snapshot is authoritative.
    const auto it = client_variables_.find(local_client_no_);
    if (it != client_variables_.end()) {
        for (const auto &entry : it->second) {
            VariableChange change;
            change.client_no = local_client_no_;
            change.name = entry.first;
            change.old_value = entry.second;
            change.had_old_value = true;
            change.removed = true;
            changes.push_back(change);
        }
        it->second.clear();
    }
    clear_pending_for_client(local_client_no_);
    return true;
}

// --- Incoming ----------------------------------------------------------------

void NetworkVariableManager::handle_global_sync(const GlobalVarSyncMessage &message,
                                                std::vector<VariableChange> &changes) {
    has_received_initial_sync_ = true;

    for (const NetworkVariableEntry &entry : message.variables) {
        if (entry.name.empty()) {
            continue;
        }
        // Reconcile send-side dedupe with the authoritative value: drop the
        // redundant trailing copy of our own last send, and forget the dedupe
        // cache entirely when someone else changed the value, so restoring that
        // value later is not silently swallowed.
        const auto last_sent = last_sent_global_.find(entry.name);
        if (last_sent != last_sent_global_.end()) {
            const auto pending = pending_global_.find(entry.name);
            if (pending != pending_global_.end() && pending->second == last_sent->second) {
                pending_global_.erase(pending);
                due_global_.erase(entry.name);
            }
            if (last_sent->second != entry.value) {
                last_sent_global_.erase(last_sent);
            }
        }

        const auto existing = globals_.find(entry.name);
        const bool had_old = existing != globals_.end();
        if (had_old && existing->second == entry.value) {
            continue;
        }

        VariableChange change;
        change.client_no = 0;
        change.name = entry.name;
        change.had_old_value = had_old;
        change.old_value = had_old ? existing->second : std::string();
        change.new_value = entry.value;
        globals_[entry.name] = entry.value;
        changes.push_back(change);
    }
}

void NetworkVariableManager::handle_client_sync(const ClientVarSyncMessage &message,
                                                std::vector<VariableChange> &changes) {
    has_received_initial_sync_ = true;

    for (const auto &client : message.clients) {
        const int client_no = static_cast<int>(client.first);

        std::map<std::string, std::string> next;
        for (const NetworkVariableEntry &entry : client.second) {
            if (entry.name.empty()) {
                continue;
            }
            next[entry.name] = entry.value;
        }

        std::map<std::string, std::string> old;
        const auto existing = client_variables_.find(client_no);
        if (existing != client_variables_.end()) {
            old = existing->second;
        }

        // Each block is a full authoritative snapshot: anything missing was removed.
        for (const auto &entry : old) {
            if (next.find(entry.first) != next.end()) {
                continue;
            }
            const ClientKey key(client_no, entry.first);
            const auto last_sent = last_sent_client_.find(key);
            if (last_sent != last_sent_client_.end()) {
                const auto pending = pending_client_.find(key);
                if (pending != pending_client_.end() && pending->second == last_sent->second) {
                    pending_client_.erase(pending);
                    due_client_.erase(key);
                }
                last_sent_client_.erase(last_sent);
            }
            VariableChange change;
            change.client_no = client_no;
            change.name = entry.first;
            change.old_value = entry.second;
            change.had_old_value = true;
            change.removed = true;
            changes.push_back(change);
        }

        for (const auto &entry : next) {
            const ClientKey key(client_no, entry.first);
            last_sent_client_[key] = entry.second;
            const auto pending = pending_client_.find(key);
            if (pending != pending_client_.end() && pending->second == entry.second) {
                pending_client_.erase(pending);
                due_client_.erase(key);
            }
            const auto old_entry = old.find(entry.first);
            const bool had_old = old_entry != old.end();
            if (had_old && old_entry->second == entry.second) {
                continue;
            }
            VariableChange change;
            change.client_no = client_no;
            change.name = entry.first;
            change.had_old_value = had_old;
            change.old_value = had_old ? old_entry->second : std::string();
            change.new_value = entry.second;
            changes.push_back(change);
        }

        client_variables_[client_no] = std::move(next);
    }
}

// --- Flushing ----------------------------------------------------------------

bool NetworkVariableManager::flush_pending_global(double now_seconds) {
    std::vector<std::pair<std::string, std::string>> to_flush;
    for (const auto &entry : due_global_) {
        if (entry.second > now_seconds) {
            continue;
        }
        const auto pending = pending_global_.find(entry.first);
        if (pending != pending_global_.end()) {
            to_flush.emplace_back(entry.first, pending->second);
        }
    }

    bool all_flushed = true;
    for (const auto &entry : to_flush) {
        const auto last_sent = last_sent_global_.find(entry.first);
        if (last_sent != last_sent_global_.end() && last_sent->second == entry.second) {
            pending_global_.erase(entry.first);
            due_global_.erase(entry.first);
            continue;
        }
        if (try_send_global_now(entry.first, entry.second)) {
            pending_global_.erase(entry.first);
            due_global_.erase(entry.first);
            next_allowed_global_[entry.first] = now_seconds + kDebounceInterval;
        } else {
            all_flushed = false;  // Keep the entry so a later tick retries.
        }
    }
    return all_flushed;
}

bool NetworkVariableManager::flush_pending_client(double now_seconds) {
    std::vector<std::pair<ClientKey, std::string>> to_flush;
    for (const auto &entry : due_client_) {
        if (entry.second > now_seconds) {
            continue;
        }
        const auto pending = pending_client_.find(entry.first);
        if (pending != pending_client_.end()) {
            to_flush.emplace_back(entry.first, pending->second);
        }
    }

    bool all_flushed = true;
    for (const auto &entry : to_flush) {
        const auto last_sent = last_sent_client_.find(entry.first);
        if (last_sent != last_sent_client_.end() && last_sent->second == entry.second) {
            pending_client_.erase(entry.first);
            due_client_.erase(entry.first);
            continue;
        }
        if (try_send_client_now(entry.first.first, entry.first.second, entry.second)) {
            pending_client_.erase(entry.first);
            due_client_.erase(entry.first);
            next_allowed_client_[entry.first] = now_seconds + kDebounceInterval;
        } else {
            all_flushed = false;
        }
    }
    return all_flushed;
}

bool NetworkVariableManager::tick(double now_seconds) {
    const bool global_ok = flush_pending_global(now_seconds);
    const bool client_ok = flush_pending_client(now_seconds);
    return global_ok && client_ok;
}

void NetworkVariableManager::clear_pending_for_client(int client_no) {
    for (auto it = pending_client_.begin(); it != pending_client_.end();) {
        it = it->first.first == client_no ? pending_client_.erase(it) : std::next(it);
    }
    for (auto it = due_client_.begin(); it != due_client_.end();) {
        it = it->first.first == client_no ? due_client_.erase(it) : std::next(it);
    }
    for (auto it = last_sent_client_.begin(); it != last_sent_client_.end();) {
        it = it->first.first == client_no ? last_sent_client_.erase(it) : std::next(it);
    }
    for (auto it = next_allowed_client_.begin(); it != next_allowed_client_.end();) {
        it = it->first.first == client_no ? next_allowed_client_.erase(it) : std::next(it);
    }
}

void NetworkVariableManager::clear_pending_sends() {
    last_sent_global_.clear();
    last_sent_client_.clear();
    next_allowed_global_.clear();
    next_allowed_client_.clear();
    pending_global_.clear();
    due_global_.clear();
    pending_client_.clear();
    due_client_.clear();
}

void NetworkVariableManager::clear_all() {
    clear_pending_sends();
    globals_.clear();
    client_variables_.clear();
    reset_initial_sync();
}

}  // namespace netsync
}  // namespace styly
