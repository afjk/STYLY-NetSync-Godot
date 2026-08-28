// SPDX-License-Identifier: Apache-2.0
// Global and per-client network variables.
//
// Reproduces the upstream Unity NetworkVariableManager: leading-edge send with
// a 100 ms cooldown, a trailing latest-wins flush on a fixed deadline, send-side
// dedupe against the last value actually put on the wire, and authoritative
// snapshot semantics on the receive side (a client's sync block replaces that
// client's whole variable set).
//
// Main-thread only.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "protocol/protocol_v8.hpp"

namespace styly {
namespace netsync {

/// Emitted when a variable's value changes. `had_old_value` is false when the
/// name is newly introduced; `removed` is true when it disappeared from an
/// authoritative snapshot.
struct VariableChange {
    int client_no = 0;  ///< 0 for a global variable
    std::string name;
    std::string old_value;
    std::string new_value;
    bool had_old_value = false;
    bool removed = false;
};

class NetworkVariableManager {
public:
    using ControlSender = std::function<bool(std::vector<std::uint8_t>)>;
    using LogCallback = std::function<void(const std::string &)>;

    /// Upstream limits (must match the server).
    static constexpr std::size_t kMaxGlobalVars = 100;
    static constexpr std::size_t kMaxClientVars = 100;
    static constexpr double kDebounceInterval = 0.1;
    /// A room with no variables never produces a sync message, so readiness
    /// falls back to this timeout.
    static constexpr double kInitialSyncTimeout = 2.0;

    void set_control_sender(ControlSender sender) { send_ = std::move(sender); }
    void set_log_callback(LogCallback callback) { on_log_ = std::move(callback); }

    void set_local_identity(int client_no, std::string device_id) {
        local_client_no_ = client_no;
        local_device_id_ = std::move(device_id);
    }

    bool has_received_initial_sync() const { return has_received_initial_sync_; }
    void mark_initial_sync_complete() { has_received_initial_sync_ = true; }
    void reset_initial_sync();
    void on_connection_established(double now_seconds);
    /// Returns true when this call flipped initial sync to complete.
    bool check_initial_sync_timeout(double now_seconds);

    // --- Global variables ---------------------------------------------------

    bool set_global(double now_seconds, const std::string &name, const std::string &value);
    bool get_global(const std::string &name, std::string &out) const;
    std::map<std::string, std::string> all_globals() const { return globals_; }

    // --- Client variables ---------------------------------------------------

    bool set_client(double now_seconds, int target_client_no, const std::string &name,
                    const std::string &value);
    bool get_client(int client_no, const std::string &name, std::string &out) const;
    std::map<std::string, std::string> all_client_variables(int client_no) const;

    /// Clear every variable this client owns on the server.
    bool clear_my_client_variables(std::vector<VariableChange> &changes);

    // --- Incoming syncs ------------------------------------------------------

    void handle_global_sync(const GlobalVarSyncMessage &message,
                            std::vector<VariableChange> &changes);
    void handle_client_sync(const ClientVarSyncMessage &message,
                            std::vector<VariableChange> &changes);

    /// Flush trailing debounced writes. Returns false when a send was refused.
    bool tick(double now_seconds);

    /// Drop send-side state that must not cross a connection session.
    void clear_pending_sends();
    /// Drop everything, including the cached values.
    void clear_all();

private:
    using ClientKey = std::pair<int, std::string>;

    bool validate_name(const std::string &name) const;
    bool validate_value(const std::string &value) const;
    bool try_send_global_now(const std::string &name, const std::string &value);
    bool try_send_client_now(int target_client_no, const std::string &name,
                             const std::string &value);
    bool flush_pending_global(double now_seconds);
    bool flush_pending_client(double now_seconds);
    void clear_pending_for_client(int client_no);
    void log(const std::string &message);

    ControlSender send_;
    LogCallback on_log_;

    int local_client_no_ = 0;
    std::string local_device_id_;

    std::map<std::string, std::string> globals_;
    std::map<int, std::map<std::string, std::string>> client_variables_;

    std::map<std::string, std::string> last_sent_global_;
    std::map<ClientKey, std::string> last_sent_client_;
    std::map<std::string, double> next_allowed_global_;
    std::map<ClientKey, double> next_allowed_client_;
    std::map<std::string, std::string> pending_global_;
    std::map<std::string, double> due_global_;
    std::map<ClientKey, std::string> pending_client_;
    std::map<ClientKey, double> due_client_;

    bool has_received_initial_sync_ = false;
    bool connection_established_ = false;
    double connection_established_at_ = 0.0;
};

}  // namespace netsync
}  // namespace styly
