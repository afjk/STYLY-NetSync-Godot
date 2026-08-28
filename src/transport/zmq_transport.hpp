// SPDX-License-Identifier: Apache-2.0
// ZeroMQ transport for STYLY NetSync.
//
// Owns the three sockets and the single network thread. Reproduces the queueing,
// priority, latest-wins and backpressure behaviour of the upstream Unity
// ConnectionManager (see docs/PROTOCOL_V8.md §1.5).
//
// No Godot dependency: callbacks hand raw payloads to the caller, which is
// responsible for decoding and for marshalling to whatever main thread it has.
// Every callback fires on the network thread.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace styly {
namespace netsync {

/// Which socket a payload arrived on. Lets the caller enforce lane rules.
enum class TransportLane {
    Control,   ///< control DEALER (ROUTER unicast from the server)
    RoomPose,  ///< SUB, topic == roomId
    RoomObjects,  ///< SUB, topic == roomId + "\0obj"
};

struct TransportConfig {
    std::string server_address = "tcp://127.0.0.1";
    int control_port = 5555;
    int transform_port = 5557;
    int sub_port = 5556;
    std::string room_id = "default_room";
};

/// Counters mirroring the diagnostics upstream exposes.
struct TransportStats {
    std::uint64_t dropped_transform_frames = 0;
    std::uint64_t would_block_count = 0;
    std::uint64_t control_dropped_full = 0;
    std::uint64_t control_dropped_expired = 0;
    std::uint64_t messages_sent = 0;
    std::uint64_t messages_received = 0;
    int control_queue_length = 0;
};

class ZmqTransport {
public:
    /// Called on the network thread for every accepted inbound payload.
    using PayloadCallback = std::function<void(TransportLane, const std::uint8_t *, std::size_t)>;
    /// Called on the network thread once the sockets are up.
    using ConnectedCallback = std::function<void()>;
    /// Called on the network thread when the loop aborts.
    using ErrorCallback = std::function<void(const std::string &)>;
    /// Called on the network thread for diagnostics the caller may want to surface.
    using LogCallback = std::function<void(const std::string &)>;

    ZmqTransport();
    ~ZmqTransport();

    ZmqTransport(const ZmqTransport &) = delete;
    ZmqTransport &operator=(const ZmqTransport &) = delete;

    void set_payload_callback(PayloadCallback callback) { on_payload_ = std::move(callback); }
    void set_connected_callback(ConnectedCallback callback) { on_connected_ = std::move(callback); }
    void set_error_callback(ErrorCallback callback) { on_error_ = std::move(callback); }
    void set_log_callback(LogCallback callback) { on_log_ = std::move(callback); }

    /// Start the network thread. Returns false when already running.
    bool start(const TransportConfig &config);

    /// Stop the network thread, close the sockets and clear the outbound queues.
    /// Safe to call when not running. Blocks until the thread has exited.
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }
    bool has_error() const { return connection_error_.load(std::memory_order_acquire); }

    /// Queue a control-lane message. Returns false when the outbox is full or
    /// the transport is not usable, matching upstream's drop-newest policy.
    bool try_enqueue_control(std::vector<std::uint8_t> payload);

    /// Overwrite the single avatar-pose slot (latest-wins).
    void set_latest_transform(std::vector<std::uint8_t> payload);

    /// Overwrite one object's pose slot (per-object latest-wins).
    void set_latest_object_transform(std::uint32_t object_id, std::vector<std::uint8_t> payload);

    /// Drop everything queued for sending. Used on room switch and shutdown.
    void clear_outgoing();

    TransportStats stats() const;

    /// The room id the transport was started with.
    const std::string &room_id() const { return room_id_; }

    // --- Topic classification (exposed for testing) -------------------------

    /// True iff `topic` is byte-exactly the room topic.
    static bool is_avatar_topic(const std::uint8_t *topic, std::size_t topic_size,
                                const std::string &room_id);

    /// True iff `topic` is byte-exactly `room_id + "\0obj"`.
    static bool is_object_topic(const std::uint8_t *topic, std::size_t topic_size,
                                const std::string &room_id);

    /// Build a `tcp://host:port` endpoint, tolerating an input that already
    /// carries the `tcp://` scheme.
    static std::string build_endpoint(const std::string &server_address, int port);

    // --- Tunables, matching upstream ---------------------------------------

    static constexpr int kControlSendHwm = 1024;
    static constexpr int kControlReceiveHwm = 1024;
    static constexpr int kTransformSendHwm = 2;
    static constexpr int kTransformReceiveHwm = 2;
    static constexpr std::size_t kControlOutboxMax = 256;
    static constexpr double kControlTtlSeconds = 5.0;
    static constexpr int kControlDrainBatch = 64;
    static constexpr int kIdleSleepMilliseconds = 1;

private:
    struct OutboundPacket {
        std::vector<std::uint8_t> payload;
        double enqueued_at = 0.0;
    };

    void network_loop(TransportConfig config);
    bool flush_outgoing(void *control_socket, void *transform_socket);
    bool drain_control_sends(void *socket);
    bool try_send_latest_transform(void *socket);
    bool try_send_latest_object_transforms(void *socket);
    bool try_send_multipart(void *socket, const std::vector<std::uint8_t> &payload);
    void receive_sub(void *socket);
    void receive_control(void *socket);
    void log(const std::string &message);

    static double monotonic_seconds();

    void *context_ = nullptr;
    std::thread thread_;
    std::atomic<bool> should_stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> connection_error_{false};
    std::atomic<bool> sockets_ready_{false};

    std::string room_id_;
    std::vector<std::uint8_t> room_id_bytes_;

    PayloadCallback on_payload_;
    ConnectedCallback on_connected_;
    ErrorCallback on_error_;
    LogCallback on_log_;

    mutable std::mutex outbound_mutex_;
    std::deque<OutboundPacket> control_outbox_;
    bool has_latest_transform_ = false;
    OutboundPacket latest_transform_;
    std::map<std::uint32_t, OutboundPacket> latest_object_transforms_;

    /// Head-of-line packet held across iterations while backpressured.
    bool has_pending_control_ = false;
    OutboundPacket pending_control_;

    std::atomic<std::uint64_t> dropped_transform_frames_{0};
    std::atomic<std::uint64_t> would_block_count_{0};
    std::atomic<std::uint64_t> control_dropped_full_{0};
    std::atomic<std::uint64_t> control_dropped_expired_{0};
    std::atomic<std::uint64_t> messages_sent_{0};
    std::atomic<std::uint64_t> messages_received_{0};
    std::atomic<double> last_queue_full_warn_{0.0};
};

}  // namespace netsync
}  // namespace styly
