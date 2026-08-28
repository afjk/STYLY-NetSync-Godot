// SPDX-License-Identifier: Apache-2.0
// A stand-in STYLY NetSync server for tests: two ROUTER sockets (control and
// transform) plus a PUB socket, all bound on ephemeral loopback ports.
//
// It speaks the real wire protocol, so tests exercise the same framing, lanes
// and topics a real server would.
#pragma once

#include <zmq.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace styly {
namespace netsync {
namespace test {

/// A stand-in server: ROUTER for control, ROUTER for transform, PUB for
/// broadcasts, all bound on ephemeral ports.
class FakeServer {
public:
    FakeServer() {
        context_ = zmq_ctx_new();
        control_ = zmq_socket(context_, ZMQ_ROUTER);
        transform_ = zmq_socket(context_, ZMQ_ROUTER);
        publisher_ = zmq_socket(context_, ZMQ_PUB);

        const int linger = 0;
        for (void *socket : {control_, transform_, publisher_}) {
            zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger));
        }

        control_port_ = bind_ephemeral(control_);
        transform_port_ = bind_ephemeral(transform_);
        pub_port_ = bind_ephemeral(publisher_);
    }

    ~FakeServer() {
        zmq_close(control_);
        zmq_close(transform_);
        zmq_close(publisher_);
        zmq_ctx_term(context_);
    }

    int control_port() const { return control_port_; }
    int transform_port() const { return transform_port_; }
    int pub_port() const { return pub_port_; }

    /// Receive one client message from a ROUTER, as [identity, room, payload].
    /// Returns false on timeout.
    bool receive(void *socket, std::vector<std::uint8_t> &identity, std::string &room,
                 std::vector<std::uint8_t> &payload, int timeout_ms = 3000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::vector<std::vector<std::uint8_t>> parts;
            if (receive_multipart(socket, parts) && parts.size() >= 3) {
                identity = parts[0];
                room.assign(parts[1].begin(), parts[1].end());
                payload = parts[2];
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    void *control() { return control_; }
    void *transform() { return transform_; }

    /// Send a control message back to a specific client identity.
    void send_control(const std::vector<std::uint8_t> &identity, const std::string &room,
                      const std::vector<std::uint8_t> &payload) {
        zmq_send(control_, identity.data(), identity.size(), ZMQ_SNDMORE);
        zmq_send(control_, room.data(), room.size(), ZMQ_SNDMORE);
        zmq_send(control_, payload.data(), payload.size(), 0);
    }

    void publish(const std::vector<std::uint8_t> &topic,
                 const std::vector<std::uint8_t> &payload) {
        zmq_send(publisher_, topic.data(), topic.size(), ZMQ_SNDMORE);
        zmq_send(publisher_, payload.data(), payload.size(), 0);
    }

    void publish(const std::string &topic, const std::vector<std::uint8_t> &payload) {
        publish(std::vector<std::uint8_t>(topic.begin(), topic.end()), payload);
    }

private:
    static int bind_ephemeral(void *socket) {
        if (zmq_bind(socket, "tcp://127.0.0.1:*") != 0) {
            return 0;
        }
        char endpoint[256] = {0};
        std::size_t size = sizeof(endpoint);
        if (zmq_getsockopt(socket, ZMQ_LAST_ENDPOINT, endpoint, &size) != 0) {
            return 0;
        }
        const std::string text(endpoint);
        const std::size_t colon = text.rfind(':');
        return colon == std::string::npos ? 0 : std::atoi(text.c_str() + colon + 1);
    }

    static bool receive_multipart(void *socket, std::vector<std::vector<std::uint8_t>> &parts) {
        parts.clear();
        while (true) {
            zmq_msg_t message;
            zmq_msg_init(&message);
            if (zmq_msg_recv(&message, socket, ZMQ_DONTWAIT) < 0) {
                zmq_msg_close(&message);
                return !parts.empty();
            }
            const auto *data = static_cast<const std::uint8_t *>(zmq_msg_data(&message));
            parts.emplace_back(data, data + zmq_msg_size(&message));
            const bool more = zmq_msg_more(&message) != 0;
            zmq_msg_close(&message);
            if (!more) {
                return true;
            }
        }
    }

    void *context_ = nullptr;
    void *control_ = nullptr;
    void *transform_ = nullptr;
    void *publisher_ = nullptr;
    int control_port_ = 0;
    int transform_port_ = 0;
    int pub_port_ = 0;
};

}  // namespace test
}  // namespace netsync
}  // namespace styly
