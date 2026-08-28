// SPDX-License-Identifier: Apache-2.0
// Outgoing RPC handling: rate limiting, pre-ready queueing, TTL and per-tick
// flush budget. Mirrors the upstream Unity RPCManager.
//
// Main-thread only. Sending goes through an injected callback so this class has
// no transport dependency.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "protocol/protocol_v8.hpp"

namespace styly {
namespace netsync {

class RpcManager {
public:
    /// Returns true when the payload was accepted by the transport.
    using ControlSender = std::function<bool(std::vector<std::uint8_t>)>;
    using LogCallback = std::function<void(const std::string &)>;

    /// Upstream defaults (RPCManager.cs).
    static constexpr int kDefaultRpcLimit = 30;
    static constexpr double kDefaultWindowSeconds = 1.0;
    static constexpr double kDefaultWarnCooldown = 0.5;
    static constexpr std::size_t kMaxPendingRpc = 100;
    static constexpr double kRpcTtlSeconds = 5.0;
    static constexpr int kMaxFlushPerTick = 10;

    void set_control_sender(ControlSender sender) { send_ = std::move(sender); }
    void set_log_callback(LogCallback callback) { on_log_ = std::move(callback); }

    /// `rpc_limit <= 0` disables rate limiting.
    void configure_rate_limit(int rpc_limit, double window_seconds, double warn_cooldown);

    /// Queue or send an RPC. `target_client_nos` empty means broadcast.
    ///
    /// Before the client is ready the call is queued (bounded, oldest dropped),
    /// exactly as upstream does, so application code can fire RPCs during
    /// start-up without losing them.
    void send(double now_seconds, bool is_ready, int local_client_no,
              const std::string &device_id, const std::vector<int> &target_client_nos,
              const std::string &function_name, const std::vector<std::string> &arguments);

    /// Drain the pending queue once ready. Returns true when the queue is empty
    /// and nothing was backpressured.
    bool flush_pending(double now_seconds, bool is_ready, int local_client_no,
                       const std::string &device_id);

    void clear_pending();

    std::size_t pending_count() const { return pending_.size(); }
    std::uint64_t dropped_rate_limited() const { return dropped_rate_limited_; }
    std::uint64_t dropped_expired() const { return dropped_expired_; }
    std::uint64_t dropped_overflow() const { return dropped_overflow_; }

private:
    struct PendingRpc {
        std::string function_name;
        std::vector<std::string> arguments;
        std::vector<int> target_client_nos;
        double enqueued_at = 0.0;
    };

    bool try_send_now(double now_seconds, int local_client_no, const std::string &device_id,
                      const std::vector<int> &target_client_nos, const std::string &function_name,
                      const std::vector<std::string> &arguments);
    bool try_consume_quota(double now_seconds, double &retry_after, int &current_count);
    void log(const std::string &message);

    ControlSender send_;
    LogCallback on_log_;

    std::deque<double> rate_hits_;
    int rpc_limit_ = kDefaultRpcLimit;
    double window_seconds_ = kDefaultWindowSeconds;
    double warn_cooldown_ = kDefaultWarnCooldown;
    double last_warn_at_ = -1e9;

    std::deque<PendingRpc> pending_;

    std::uint64_t dropped_rate_limited_ = 0;
    std::uint64_t dropped_expired_ = 0;
    std::uint64_t dropped_overflow_ = 0;
};

}  // namespace netsync
}  // namespace styly
