// SPDX-License-Identifier: Apache-2.0
#include "rpc_manager.hpp"

#include <algorithm>

#include "json_util.hpp"

namespace styly {
namespace netsync {

void RpcManager::configure_rate_limit(int rpc_limit, double window_seconds, double warn_cooldown) {
    rpc_limit_ = std::max(0, rpc_limit);
    window_seconds_ = std::max(0.01, window_seconds);
    warn_cooldown_ = std::max(0.0, warn_cooldown);
}

void RpcManager::log(const std::string &message) {
    if (on_log_) {
        on_log_(message);
    }
}

bool RpcManager::try_consume_quota(double now_seconds, double &retry_after, int &current_count) {
    if (rpc_limit_ <= 0) {
        retry_after = 0.0;
        current_count = 0;
        return true;
    }
    while (!rate_hits_.empty() && now_seconds - rate_hits_.front() > window_seconds_) {
        rate_hits_.pop_front();
    }
    if (static_cast<int>(rate_hits_.size()) >= rpc_limit_) {
        retry_after = std::max(0.0, window_seconds_ - (now_seconds - rate_hits_.front()));
        current_count = static_cast<int>(rate_hits_.size());
        return false;
    }
    rate_hits_.push_back(now_seconds);
    retry_after = 0.0;
    current_count = static_cast<int>(rate_hits_.size());
    return true;
}

bool RpcManager::try_send_now(double now_seconds, int local_client_no,
                              const std::string &device_id,
                              const std::vector<int> &target_client_nos,
                              const std::string &function_name,
                              const std::vector<std::string> &arguments) {
    double retry_after = 0.0;
    int current_count = 0;
    if (!try_consume_quota(now_seconds, retry_after, current_count)) {
        ++dropped_rate_limited_;
        if (now_seconds - last_warn_at_ >= warn_cooldown_) {
            last_warn_at_ = now_seconds;
            log("RPC rate limited: dropped '" + function_name + "' (" +
                std::to_string(current_count) + "/" + std::to_string(rpc_limit_) + " per " +
                std::to_string(window_seconds_) + "s)");
        }
        return false;
    }

    if (!send_) {
        return false;
    }

    RpcMessage message;
    message.sender_client_no = static_cast<std::uint16_t>(local_client_no < 0 ? 0 : local_client_no);
    message.device_id = device_id;
    message.function_name = function_name;
    message.arguments_json = encode_json_string_array(arguments);
    for (int target : target_client_nos) {
        if (target < 0 || target > 0xFFFF) {
            log("RPC target client number out of range: " + std::to_string(target));
            return false;
        }
        message.target_client_nos.push_back(static_cast<std::uint16_t>(target));
    }
    if (message.target_client_nos.size() > 255) {
        log("RPC target list too long (" + std::to_string(message.target_client_nos.size()) +
            "); maximum is 255");
        return false;
    }

    return send_(serialize_rpc(message));
}

void RpcManager::send(double now_seconds, bool is_ready, int local_client_no,
                      const std::string &device_id, const std::vector<int> &target_client_nos,
                      const std::string &function_name,
                      const std::vector<std::string> &arguments) {
    if (!is_ready) {
        PendingRpc entry;
        entry.function_name = function_name;
        entry.arguments = arguments;
        entry.target_client_nos = target_client_nos;
        entry.enqueued_at = now_seconds;
        pending_.push_back(std::move(entry));
        while (pending_.size() > kMaxPendingRpc) {
            log("RPC pending queue overflow: dropped '" + pending_.front().function_name + "'");
            pending_.pop_front();
            ++dropped_overflow_;
        }
        return;
    }
    try_send_now(now_seconds, local_client_no, device_id, target_client_nos, function_name,
                 arguments);
}

bool RpcManager::flush_pending(double now_seconds, bool is_ready, int local_client_no,
                               const std::string &device_id) {
    if (!is_ready) {
        return true;  // Nothing to do yet; not backpressure.
    }

    int sent_this_tick = 0;
    while (sent_this_tick < kMaxFlushPerTick && !pending_.empty()) {
        const PendingRpc &head = pending_.front();
        if (now_seconds - head.enqueued_at > kRpcTtlSeconds) {
            log("dropped expired pending RPC '" + head.function_name + "'");
            pending_.pop_front();
            ++dropped_expired_;
            continue;
        }
        if (try_send_now(now_seconds, local_client_no, device_id, head.target_client_nos,
                         head.function_name, head.arguments)) {
            pending_.pop_front();
            ++sent_this_tick;
            continue;
        }
        // Rate limited or backpressured: keep the entry and retry next tick.
        return false;
    }
    return pending_.empty();
}

void RpcManager::clear_pending() {
    pending_.clear();
    rate_hits_.clear();
}

}  // namespace netsync
}  // namespace styly
