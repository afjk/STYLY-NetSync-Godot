// SPDX-License-Identifier: Apache-2.0
// STYLY NetSync LAN server discovery.
//
// Implements the same wire protocol and probe order as the upstream Unity
// ServerDiscoveryManager (see docs/PROTOCOL_V8.md §7):
//
//   request  : "STYLY-NETSYNC-DISCOVER"
//   response : "STYLY-NETSYNC3|control|transform|pub|rest|name"
//
// Probe order: TCP to 127.0.0.1, then TCP to the cached address, then repeated
// UDP broadcast on every local IPv4 interface. Only a current
// `STYLY-NETSYNC3` reply with at least six fields is accepted — older
// `STYLY-NETSYNC2`/`STYLY-NETSYNC` replies are rejected, exactly as upstream
// clients do.
//
// No Godot dependency; the cache is a plain callback pair so the caller decides
// where to persist it.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace styly {
namespace netsync {

/// Wire constants. Kept public so tests can assert against them directly.
inline constexpr const char *kDiscoveryRequest = "STYLY-NETSYNC-DISCOVER";
inline constexpr const char *kDiscoveryResponseVersion = "STYLY-NETSYNC3";

struct DiscoveredServer {
    std::string address;  ///< "tcp://<ip>"
    std::string ip;
    int control_port = 0;
    int transform_port = 0;
    int sub_port = 0;
    int rest_api_port = 0;
    std::string server_name;
};

/// Parse a discovery response body. Returns false for anything that is not a
/// current-format reply, so a caller probing by TCP keeps scanning.
bool parse_discovery_response(const std::string &message, const std::string &sender_ip,
                              DiscoveredServer &out);

/// Enumerate this host's non-loopback IPv4 addresses and their broadcast
/// addresses. Exposed for diagnostics and tests.
struct LocalInterface {
    std::string address;
    std::string broadcast;
};
std::vector<LocalInterface> enumerate_local_interfaces();

class ServerDiscovery {
public:
    using FoundCallback = std::function<void(const DiscoveredServer &)>;
    using LogCallback = std::function<void(const std::string &)>;
    /// Reads/writes the last known server IP. Both are optional.
    using CacheReader = std::function<std::string()>;
    using CacheWriter = std::function<void(const std::string &)>;

    ServerDiscovery();
    ~ServerDiscovery();

    ServerDiscovery(const ServerDiscovery &) = delete;
    ServerDiscovery &operator=(const ServerDiscovery &) = delete;

    void set_found_callback(FoundCallback callback) { on_found_ = std::move(callback); }
    void set_log_callback(LogCallback callback) { on_log_ = std::move(callback); }
    void set_cache(CacheReader reader, CacheWriter writer) {
        cache_reader_ = std::move(reader);
        cache_writer_ = std::move(writer);
    }

    void set_port(int port) { port_ = port; }
    int port() const { return port_; }

    /// Start discovery on a background thread. Returns false when already running.
    /// The found callback fires on that thread, once, and discovery then stops.
    bool start();
    void stop();
    bool is_discovering() const { return discovering_.load(std::memory_order_acquire); }

    /// One synchronous TCP probe. Public so a caller can verify a known address
    /// without running the full discovery loop.
    bool probe_tcp(const std::string &ip, int timeout_ms, DiscoveredServer &out);

    /// Interval between broadcast bursts, seconds.
    double broadcast_interval = 0.1;
    /// TCP connect timeout, milliseconds.
    int tcp_connect_timeout_ms = 300;
    /// UDP receive timeout per socket per iteration, milliseconds.
    int udp_receive_timeout_ms = 500;

private:
    void discovery_loop();
    bool broadcast_round(const std::vector<int> &sockets,
                         const std::vector<LocalInterface> &interfaces);
    void log(const std::string &message);
    void report(const DiscoveredServer &server, bool cache_ip);

    std::atomic<bool> discovering_{false};
    std::atomic<bool> should_stop_{false};
    std::thread thread_;
    int port_ = 9999;

    FoundCallback on_found_;
    LogCallback on_log_;
    CacheReader cache_reader_;
    CacheWriter cache_writer_;
};

}  // namespace netsync
}  // namespace styly
