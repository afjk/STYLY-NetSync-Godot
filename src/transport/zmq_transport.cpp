// SPDX-License-Identifier: Apache-2.0
#include "zmq_transport.hpp"

#include <zmq.h>

#include <cerrno>
#include <cstring>

#include "protocol/message_types.hpp"

namespace styly {
namespace netsync {

namespace {

/// RAII wrapper so a socket is closed exactly once when the network loop exits,
/// including on the exception/early-return paths.
class SocketHandle {
public:
    SocketHandle(void *context, int type) : socket_(zmq_socket(context, type)) {}
    ~SocketHandle() {
        if (socket_ != nullptr) {
            zmq_close(socket_);
        }
    }

    SocketHandle(const SocketHandle &) = delete;
    SocketHandle &operator=(const SocketHandle &) = delete;

    void *get() const { return socket_; }
    explicit operator bool() const { return socket_ != nullptr; }

private:
    void *socket_ = nullptr;
};

std::string zmq_error_text() {
    const int code = zmq_errno();
    return std::string(zmq_strerror(code)) + " (errno " + std::to_string(code) + ")";
}

bool set_int_option(void *socket, int option, int value) {
    return zmq_setsockopt(socket, option, &value, sizeof(value)) == 0;
}

}  // namespace

ZmqTransport::ZmqTransport() = default;

ZmqTransport::~ZmqTransport() { stop(); }

double ZmqTransport::monotonic_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::string ZmqTransport::build_endpoint(const std::string &server_address, int port) {
    std::string host = server_address;
    const std::string scheme = "tcp://";
    if (host.rfind(scheme, 0) == 0) {
        host = host.substr(scheme.size());
    }
    // A trailing ":port" in the configured address would produce a malformed
    // endpoint; strip it so "tcp://host:5555" behaves like "host".
    const std::size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(']') == std::string::npos &&
        host.find(':') == colon) {
        bool all_digits = colon + 1 < host.size();
        for (std::size_t i = colon + 1; i < host.size() && all_digits; ++i) {
            all_digits = host[i] >= '0' && host[i] <= '9';
        }
        if (all_digits) {
            host = host.substr(0, colon);
        }
    }
    return scheme + host + ":" + std::to_string(port);
}

bool ZmqTransport::is_avatar_topic(const std::uint8_t *topic, std::size_t topic_size,
                                   const std::string &room_id) {
    // Length first: a differing length settles it without touching the bytes,
    // and it lets a zero-length topic match a (degenerate) empty room id even
    // though an empty frame has no data pointer to compare.
    if (topic_size != room_id.size()) {
        return false;
    }
    if (topic_size == 0) {
        return true;
    }
    if (topic == nullptr) {
        return false;
    }
    return std::memcmp(topic, room_id.data(), topic_size) == 0;
}

bool ZmqTransport::is_object_topic(const std::uint8_t *topic, std::size_t topic_size,
                                   const std::string &room_id) {
    if (topic_size != room_id.size() + kObjectTopicSuffixLength) {
        return false;
    }
    if (topic == nullptr) {
        return false;
    }
    if (!room_id.empty() && std::memcmp(topic, room_id.data(), room_id.size()) != 0) {
        return false;
    }
    return std::memcmp(topic + room_id.size(), kObjectTopicSuffix,
                       kObjectTopicSuffixLength) == 0;
}

bool ZmqTransport::start(const TransportConfig &config) {
    if (running_.load(std::memory_order_acquire) || thread_.joinable()) {
        return false;
    }
    should_stop_.store(false, std::memory_order_release);
    connection_error_.store(false, std::memory_order_release);
    sockets_ready_.store(false, std::memory_order_release);
    room_id_ = config.room_id;
    room_id_bytes_.assign(room_id_.begin(), room_id_.end());

    context_ = zmq_ctx_new();
    if (context_ == nullptr) {
        if (on_error_) {
            on_error_("failed to create ZeroMQ context: " + zmq_error_text());
        }
        return false;
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ZmqTransport::network_loop, this, config);
    return true;
}

void ZmqTransport::stop() {
    if (!thread_.joinable()) {
        if (context_ != nullptr) {
            zmq_ctx_term(context_);
            context_ = nullptr;
        }
        running_.store(false, std::memory_order_release);
        return;
    }

    should_stop_.store(true, std::memory_order_release);
    clear_outgoing();
    thread_.join();

    // Every socket lives on the network thread and is closed as it unwinds, so
    // by the time the join returns zmq_ctx_term cannot block.
    if (context_ != nullptr) {
        zmq_ctx_term(context_);
        context_ = nullptr;
    }
    running_.store(false, std::memory_order_release);
    sockets_ready_.store(false, std::memory_order_release);
}

bool ZmqTransport::try_enqueue_control(std::vector<std::uint8_t> payload) {
    if (should_stop_.load(std::memory_order_acquire) ||
        connection_error_.load(std::memory_order_acquire) ||
        !running_.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(outbound_mutex_);
    if (control_outbox_.size() >= kControlOutboxMax) {
        control_dropped_full_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    OutboundPacket packet;
    packet.payload = std::move(payload);
    packet.enqueued_at = monotonic_seconds();
    control_outbox_.push_back(std::move(packet));
    return true;
}

void ZmqTransport::set_latest_transform(std::vector<std::uint8_t> payload) {
    if (should_stop_.load(std::memory_order_acquire) ||
        connection_error_.load(std::memory_order_acquire) ||
        !running_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    latest_transform_.payload = std::move(payload);
    latest_transform_.enqueued_at = monotonic_seconds();
    has_latest_transform_ = true;
}

void ZmqTransport::set_latest_object_transform(std::uint32_t object_id,
                                               std::vector<std::uint8_t> payload) {
    if (object_id == 0u) {
        return;
    }
    if (should_stop_.load(std::memory_order_acquire) ||
        connection_error_.load(std::memory_order_acquire) ||
        !running_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    OutboundPacket packet;
    packet.payload = std::move(payload);
    packet.enqueued_at = monotonic_seconds();
    latest_object_transforms_[object_id] = std::move(packet);
}

void ZmqTransport::clear_outgoing() {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    control_outbox_.clear();
    has_latest_transform_ = false;
    latest_transform_ = OutboundPacket();
    latest_object_transforms_.clear();
    has_pending_control_ = false;
    pending_control_ = OutboundPacket();
}

TransportStats ZmqTransport::stats() const {
    TransportStats out;
    out.dropped_transform_frames = dropped_transform_frames_.load(std::memory_order_relaxed);
    out.would_block_count = would_block_count_.load(std::memory_order_relaxed);
    out.control_dropped_full = control_dropped_full_.load(std::memory_order_relaxed);
    out.control_dropped_expired = control_dropped_expired_.load(std::memory_order_relaxed);
    out.messages_sent = messages_sent_.load(std::memory_order_relaxed);
    out.messages_received = messages_received_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        out.control_queue_length = static_cast<int>(control_outbox_.size());
    }
    return out;
}

void ZmqTransport::log(const std::string &message) {
    if (on_log_) {
        on_log_(message);
    }
}

void ZmqTransport::network_loop(TransportConfig config) {
    SocketHandle control(context_, ZMQ_DEALER);
    SocketHandle transform(context_, ZMQ_DEALER);
    SocketHandle sub(context_, ZMQ_SUB);

    const auto fail = [&](const std::string &message) {
        connection_error_.store(true, std::memory_order_release);
        if (on_error_) {
            on_error_(message);
        }
    };

    if (!control || !transform || !sub) {
        fail("failed to create ZeroMQ sockets: " + zmq_error_text());
        return;
    }

    // Control DEALER.
    if (!set_int_option(control.get(), ZMQ_LINGER, 0) ||
        !set_int_option(control.get(), ZMQ_SNDHWM, kControlSendHwm) ||
        !set_int_option(control.get(), ZMQ_RCVHWM, kControlReceiveHwm)) {
        fail("failed to configure the control socket: " + zmq_error_text());
        return;
    }
    const std::string control_endpoint = build_endpoint(config.server_address, config.control_port);
    if (zmq_connect(control.get(), control_endpoint.c_str()) != 0) {
        fail("failed to connect control DEALER to " + control_endpoint + ": " + zmq_error_text());
        return;
    }
    log("control DEALER connected to " + control_endpoint);

    // Transform DEALER.
    if (!set_int_option(transform.get(), ZMQ_LINGER, 0) ||
        !set_int_option(transform.get(), ZMQ_SNDHWM, kTransformSendHwm)) {
        fail("failed to configure the transform socket: " + zmq_error_text());
        return;
    }
    const std::string transform_endpoint =
        build_endpoint(config.server_address, config.transform_port);
    if (zmq_connect(transform.get(), transform_endpoint.c_str()) != 0) {
        fail("failed to connect transform DEALER to " + transform_endpoint + ": " +
             zmq_error_text());
        return;
    }
    log("transform DEALER connected to " + transform_endpoint);

    // SUB. ZMQ_SUBSCRIBE is a prefix filter, so the room topic also delivers the
    // object topic; the strict classification below is what keeps the two apart
    // (and rejects other rooms that share this room's id as a prefix).
    if (!set_int_option(sub.get(), ZMQ_LINGER, 0) ||
        !set_int_option(sub.get(), ZMQ_RCVHWM, kTransformReceiveHwm)) {
        fail("failed to configure the subscriber socket: " + zmq_error_text());
        return;
    }
    const std::string sub_endpoint = build_endpoint(config.server_address, config.sub_port);
    if (zmq_connect(sub.get(), sub_endpoint.c_str()) != 0) {
        fail("failed to connect SUB to " + sub_endpoint + ": " + zmq_error_text());
        return;
    }
    if (zmq_setsockopt(sub.get(), ZMQ_SUBSCRIBE, config.room_id.data(), config.room_id.size()) !=
        0) {
        fail("failed to subscribe to room topic: " + zmq_error_text());
        return;
    }
    log("SUB connected to " + sub_endpoint);

    sockets_ready_.store(true, std::memory_order_release);
    if (on_connected_) {
        on_connected_();
    }

    while (!should_stop_.load(std::memory_order_acquire)) {
        const bool sent_any = flush_outgoing(control.get(), transform.get());

        const std::uint64_t received_before = messages_received_.load(std::memory_order_relaxed);
        receive_sub(sub.get());
        receive_control(control.get());
        const bool received_any =
            messages_received_.load(std::memory_order_relaxed) != received_before;

        if (!sent_any && !received_any) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kIdleSleepMilliseconds));
        }
    }

    sockets_ready_.store(false, std::memory_order_release);
}

bool ZmqTransport::flush_outgoing(void *control_socket, void *transform_socket) {
    // Control first: RPC and network variables must not be starved by the
    // high-frequency pose stream.
    bool did_work = drain_control_sends(control_socket);
    did_work |= try_send_latest_transform(transform_socket);
    did_work |= try_send_latest_object_transforms(transform_socket);
    return did_work;
}

bool ZmqTransport::drain_control_sends(void *socket) {
    bool did_work = false;
    int sent = 0;
    const double now = monotonic_seconds();

    while (sent < kControlDrainBatch) {
        if (!has_pending_control_) {
            std::lock_guard<std::mutex> lock(outbound_mutex_);
            if (control_outbox_.empty()) {
                break;
            }
            pending_control_ = std::move(control_outbox_.front());
            control_outbox_.pop_front();
            has_pending_control_ = true;
        }

        if (now - pending_control_.enqueued_at > kControlTtlSeconds) {
            control_dropped_expired_.fetch_add(1, std::memory_order_relaxed);
            log("control packet expired (TTL " + std::to_string(kControlTtlSeconds) +
                "s exceeded)");
            has_pending_control_ = false;
            continue;
        }

        if (try_send_multipart(socket, pending_control_.payload)) {
            has_pending_control_ = false;
            did_work = true;
            ++sent;
        } else {
            would_block_count_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    return did_work;
}

bool ZmqTransport::try_send_latest_transform(void *socket) {
    std::vector<std::uint8_t> payload;
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        if (!has_latest_transform_) {
            return false;
        }
        payload = latest_transform_.payload;
    }

    if (!try_send_multipart(socket, payload)) {
        would_block_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Clear only if the slot still holds what was just sent: a concurrent
    // set_latest_transform during the send must not be lost.
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    if (has_latest_transform_ && latest_transform_.payload == payload) {
        has_latest_transform_ = false;
        latest_transform_ = OutboundPacket();
    }
    return true;
}

bool ZmqTransport::try_send_latest_object_transforms(void *socket) {
    std::vector<std::uint32_t> object_ids;
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        if (latest_object_transforms_.empty()) {
            return false;
        }
        object_ids.reserve(latest_object_transforms_.size());
        for (const auto &entry : latest_object_transforms_) {
            object_ids.push_back(entry.first);
        }
    }

    bool did_work = false;
    for (std::uint32_t object_id : object_ids) {
        std::vector<std::uint8_t> payload;
        {
            std::lock_guard<std::mutex> lock(outbound_mutex_);
            const auto it = latest_object_transforms_.find(object_id);
            if (it == latest_object_transforms_.end()) {
                continue;
            }
            payload = it->second.payload;
        }

        if (!try_send_multipart(socket, payload)) {
            would_block_count_.fetch_add(1, std::memory_order_relaxed);
            break;  // Backpressure: stop draining objects this iteration.
        }

        std::lock_guard<std::mutex> lock(outbound_mutex_);
        const auto it = latest_object_transforms_.find(object_id);
        if (it != latest_object_transforms_.end() && it->second.payload == payload) {
            latest_object_transforms_.erase(it);
        }
        did_work = true;
    }
    return did_work;
}

bool ZmqTransport::try_send_multipart(void *socket, const std::vector<std::uint8_t> &payload) {
    // Frame 0: roomId. Frame 1: payload. Non-blocking; EAGAIN means backpressure.
    int rc = zmq_send(socket, room_id_bytes_.data(), room_id_bytes_.size(),
                      ZMQ_SNDMORE | ZMQ_DONTWAIT);
    if (rc < 0) {
        return false;
    }
    // The room frame is committed to the socket's queue at this point. libzmq
    // does not drop a partially queued multipart message on EAGAIN for the
    // final frame, but a failure here would leave the message unterminated, so
    // retry the tail briefly rather than abandoning it.
    for (int attempt = 0; attempt < 64; ++attempt) {
        rc = zmq_send(socket, payload.data(), payload.size(), ZMQ_DONTWAIT);
        if (rc >= 0) {
            messages_sent_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (zmq_errno() != EAGAIN) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    // Send the (empty) tail so the socket is not left mid-message.
    zmq_send(socket, "", 0, 0);
    return false;
}

void ZmqTransport::receive_sub(void *socket) {
    std::vector<std::uint8_t> last_avatar;
    std::vector<std::uint8_t> last_object;
    bool got_avatar = false;
    bool got_object = false;
    int avatar_frames = 0;

    while (true) {
        zmq_msg_t topic_msg;
        zmq_msg_init(&topic_msg);
        if (zmq_msg_recv(&topic_msg, socket, ZMQ_DONTWAIT) < 0) {
            zmq_msg_close(&topic_msg);
            break;
        }

        const std::uint8_t *topic_data = static_cast<const std::uint8_t *>(zmq_msg_data(&topic_msg));
        const std::size_t topic_size = zmq_msg_size(&topic_msg);
        const bool avatar = is_avatar_topic(topic_data, topic_size, room_id_);
        const bool object = !avatar && is_object_topic(topic_data, topic_size, room_id_);
        const bool more = zmq_msg_more(&topic_msg) != 0;
        zmq_msg_close(&topic_msg);

        if (!more) {
            continue;  // Topic without a payload frame: nothing to do.
        }

        zmq_msg_t payload_msg;
        zmq_msg_init(&payload_msg);
        if (zmq_msg_recv(&payload_msg, socket, ZMQ_DONTWAIT) < 0) {
            zmq_msg_close(&payload_msg);
            break;
        }
        const std::uint8_t *payload_data =
            static_cast<const std::uint8_t *>(zmq_msg_data(&payload_msg));
        const std::size_t payload_size = zmq_msg_size(&payload_msg);

        // Latest-wins: within one drain only the newest frame of each kind is
        // decoded; the rest are counted as intentionally dropped.
        if (avatar) {
            last_avatar.assign(payload_data, payload_data + payload_size);
            got_avatar = true;
            ++avatar_frames;
        } else if (object) {
            last_object.assign(payload_data, payload_data + payload_size);
            got_object = true;
        }
        // Any other topic is a different room sharing our prefix: ignored.

        zmq_msg_close(&payload_msg);
    }

    if (avatar_frames > 1) {
        dropped_transform_frames_.fetch_add(static_cast<std::uint64_t>(avatar_frames - 1),
                                            std::memory_order_relaxed);
    }

    if (got_avatar) {
        messages_received_.fetch_add(1, std::memory_order_relaxed);
        if (on_payload_) {
            on_payload_(TransportLane::RoomPose, last_avatar.data(), last_avatar.size());
        }
    }
    if (got_object) {
        messages_received_.fetch_add(1, std::memory_order_relaxed);
        if (on_payload_) {
            on_payload_(TransportLane::RoomObjects, last_object.data(), last_object.size());
        }
    }
}

void ZmqTransport::receive_control(void *socket) {
    while (true) {
        zmq_msg_t room_msg;
        zmq_msg_init(&room_msg);
        if (zmq_msg_recv(&room_msg, socket, ZMQ_DONTWAIT) < 0) {
            zmq_msg_close(&room_msg);
            break;
        }
        const char *room_data = static_cast<const char *>(zmq_msg_data(&room_msg));
        const std::string frame_room(room_data, zmq_msg_size(&room_msg));
        const bool more = zmq_msg_more(&room_msg) != 0;
        zmq_msg_close(&room_msg);

        if (!more) {
            continue;
        }

        zmq_msg_t payload_msg;
        zmq_msg_init(&payload_msg);
        if (zmq_msg_recv(&payload_msg, socket, ZMQ_DONTWAIT) < 0) {
            zmq_msg_close(&payload_msg);
            break;
        }

        messages_received_.fetch_add(1, std::memory_order_relaxed);
        if (frame_room == room_id_ && on_payload_) {
            on_payload_(TransportLane::Control,
                        static_cast<const std::uint8_t *>(zmq_msg_data(&payload_msg)),
                        zmq_msg_size(&payload_msg));
        }
        zmq_msg_close(&payload_msg);
    }
}

}  // namespace netsync
}  // namespace styly
