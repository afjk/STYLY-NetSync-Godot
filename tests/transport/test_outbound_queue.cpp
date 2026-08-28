// SPDX-License-Identifier: Apache-2.0
//
// Transport behaviour against a real in-process ZeroMQ peer: multipart framing,
// lane separation, strict SUB routing, and the outbox capacity/backpressure
// rules copied from upstream's ConnectionManager.

#include <zmq.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../fake_server.hpp"
#include "../test_support.hpp"
#include "protocol/protocol_v8.hpp"
#include "transport/zmq_transport.hpp"

using namespace styly::netsync;
using namespace styly::netsync::test;

namespace {

/// Collects payloads delivered by the transport, tagged by lane.
struct Collector {
    std::mutex mutex;
    std::vector<std::pair<TransportLane, std::vector<std::uint8_t>>> payloads;
    std::atomic<bool> connected{false};
    std::atomic<int> errors{0};

    void attach(ZmqTransport &transport) {
        transport.set_payload_callback(
            [this](TransportLane lane, const std::uint8_t *data, std::size_t size) {
                std::lock_guard<std::mutex> lock(mutex);
                payloads.emplace_back(lane, std::vector<std::uint8_t>(data, data + size));
            });
        transport.set_connected_callback([this]() { connected.store(true); });
        transport.set_error_callback([this](const std::string &) { errors.fetch_add(1); });
    }

    std::size_t count(TransportLane lane) {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t total = 0;
        for (const auto &entry : payloads) {
            if (entry.first == lane) {
                ++total;
            }
        }
        return total;
    }

    bool wait_for(TransportLane lane, std::size_t expected, int timeout_ms = 3000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (count(lane) >= expected) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

void test_control_framing_and_ordering() {
    FakeServer server;
    ZmqTransport transport;
    Collector collector;
    collector.attach(transport);

    TransportConfig config;
    config.server_address = "tcp://127.0.0.1";
    config.control_port = server.control_port();
    config.transform_port = server.transform_port();
    config.sub_port = server.pub_port();
    config.room_id = "unit_test_room";
    CHECK(transport.start(config));

    // Three control messages, in order.
    CHECK(transport.try_enqueue_control(serialize_client_hello("device-a", false)));
    ClientVarClearMessage clear;
    clear.sender_client_no = 7;
    clear.device_id = "device-a";
    CHECK(transport.try_enqueue_control(serialize_client_var_clear(clear)));
    GlobalVarSetMessage set;
    set.sender_client_no = 7;
    set.device_id = "device-a";
    set.variable_name = "k";
    set.variable_value = "v";
    CHECK(transport.try_enqueue_control(serialize_global_var_set(set)));

    std::vector<std::uint8_t> identity;
    std::string room;
    std::vector<std::uint8_t> payload;

    CHECK(server.receive(server.control(), identity, room, payload));
    // Frame 0 is the room id, frame 1 the payload — after the ROUTER identity.
    CHECK_EQ(room, std::string("unit_test_room"));
    CHECK_EQ(peek_message_type(payload.data(), payload.size()),
             static_cast<std::uint8_t>(MSG_CLIENT_HELLO));
    ClientHelloMessage hello;
    CHECK(deserialize_client_hello(payload.data(), payload.size(), hello));
    CHECK_EQ(hello.device_id, std::string("device-a"));

    CHECK(server.receive(server.control(), identity, room, payload));
    CHECK_EQ(peek_message_type(payload.data(), payload.size()),
             static_cast<std::uint8_t>(MSG_CLIENT_VAR_CLEAR));

    CHECK(server.receive(server.control(), identity, room, payload));
    CHECK_EQ(peek_message_type(payload.data(), payload.size()),
             static_cast<std::uint8_t>(MSG_GLOBAL_VAR_SET));

    // Control downlink: the client must accept its own room and drop others.
    DeviceIdMappingMessage mapping;
    mapping.server_version_major = 0;
    mapping.server_version_minor = 17;
    mapping.server_version_patch = 4;
    DeviceIdMappingEntry entry;
    entry.client_no = 3;
    entry.device_id = "device-a";
    mapping.mappings.push_back(entry);
    const std::vector<std::uint8_t> mapping_bytes = serialize_device_id_mapping(mapping);

    server.send_control(identity, "some_other_room", mapping_bytes);
    server.send_control(identity, "unit_test_room", mapping_bytes);

    CHECK(collector.wait_for(TransportLane::Control, 1));
    // Give the wrong-room frame a chance to be (incorrectly) delivered.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_EQ(collector.count(TransportLane::Control), static_cast<std::size_t>(1));

    transport.stop();
    CHECK_EQ(collector.errors.load(), 0);
}

void test_transform_lane_is_separate() {
    FakeServer server;
    ZmqTransport transport;
    Collector collector;
    collector.attach(transport);

    TransportConfig config;
    config.server_address = "127.0.0.1";
    config.control_port = server.control_port();
    config.transform_port = server.transform_port();
    config.sub_port = server.pub_port();
    config.room_id = "lane_room";
    CHECK(transport.start(config));

    ClientPoseMessage pose;
    pose.device_id = "device-b";
    pose.body.flags = POSE_FLAG_HEAD_VALID;
    pose.body.head.position = Vec3(1.0, 1.6, -2.0);
    transport.set_latest_transform(serialize_client_pose(pose));

    std::vector<std::uint8_t> identity;
    std::string room;
    std::vector<std::uint8_t> payload;
    CHECK(server.receive(server.transform(), identity, room, payload));
    CHECK_EQ(room, std::string("lane_room"));
    CHECK_EQ(peek_message_type(payload.data(), payload.size()),
             static_cast<std::uint8_t>(MSG_CLIENT_POSE));

    // Object poses use per-object latest-wins slots on the same lane.
    ObjectPoseMessage object;
    object.device_id = "device-b";
    object.object_id = 0x1234;
    object.pose_seq = 1;
    transport.set_latest_object_transform(object.object_id, serialize_object_pose(object));
    CHECK(server.receive(server.transform(), identity, room, payload));
    CHECK_EQ(peek_message_type(payload.data(), payload.size()),
             static_cast<std::uint8_t>(MSG_OBJECT_POSE));

    // object_id 0 is rejected outright, so nothing further arrives.
    ObjectPoseMessage zero;
    zero.device_id = "device-b";
    zero.object_id = 0;
    transport.set_latest_object_transform(0, serialize_object_pose(zero));
    CHECK(!server.receive(server.transform(), identity, room, payload, 200));

    transport.stop();
}

void test_sub_topic_routing_end_to_end() {
    FakeServer server;
    ZmqTransport transport;
    Collector collector;
    collector.attach(transport);

    const std::string room = "route";
    TransportConfig config;
    config.server_address = "127.0.0.1";
    config.control_port = server.control_port();
    config.transform_port = server.transform_port();
    config.sub_port = server.pub_port();
    config.room_id = room;
    CHECK(transport.start(config));

    // PUB/SUB needs a moment to complete the subscription handshake, otherwise
    // early publishes are silently dropped by the publisher.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    RoomPoseMessage room_pose;
    room_pose.room_id = room;
    room_pose.broadcast_time = 12.5;
    const std::vector<std::uint8_t> room_pose_bytes = serialize_room_pose(room_pose);

    RoomObjectsMessage room_objects;
    room_objects.broadcast_time = 12.5;
    const std::vector<std::uint8_t> room_objects_bytes = serialize_room_objects(room_objects);

    // A different room sharing this room's id as a prefix. ZMQ_SUBSCRIBE is a
    // prefix filter, so this *is* delivered to the socket and must be rejected
    // by the transport's strict classification.
    server.publish(room + "2", room_pose_bytes);
    server.publish(room, room_pose_bytes);
    std::vector<std::uint8_t> object_topic(room.begin(), room.end());
    object_topic.push_back(0x00);
    object_topic.push_back('o');
    object_topic.push_back('b');
    object_topic.push_back('j');
    server.publish(object_topic, room_objects_bytes);

    CHECK(collector.wait_for(TransportLane::RoomPose, 1));
    CHECK(collector.wait_for(TransportLane::RoomObjects, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    // Exactly one of each: the prefix-sharing room contributed nothing.
    CHECK_EQ(collector.count(TransportLane::RoomPose), static_cast<std::size_t>(1));
    CHECK_EQ(collector.count(TransportLane::RoomObjects), static_cast<std::size_t>(1));

    transport.stop();
}

void test_control_outbox_capacity_and_backpressure() {
    // Point the transport at a port nobody is listening on. A DEALER with no
    // connected peer fails every non-blocking send, so the outbox fills and the
    // drop-newest rule becomes observable.
    ZmqTransport transport;
    Collector collector;
    collector.attach(transport);

    TransportConfig config;
    config.server_address = "127.0.0.1";
    config.control_port = 1;  // reserved; nothing will ever accept
    config.transform_port = 1;
    config.sub_port = 1;
    config.room_id = "dead";
    CHECK(transport.start(config));

    std::size_t accepted = 0;
    std::size_t refused = 0;
    for (int i = 0; i < 400; ++i) {
        if (transport.try_enqueue_control(serialize_client_hello("d", false))) {
            ++accepted;
        } else {
            ++refused;
        }
    }

    CHECK_MSG(refused > 0, "the outbox never refused a message");
    // 256 in the queue, plus at most one the network thread has taken as its
    // head-of-line packet while backpressured.
    CHECK_MSG(accepted >= ZmqTransport::kControlOutboxMax &&
                  accepted <= ZmqTransport::kControlOutboxMax + 1,
              "accepted " + std::to_string(accepted) + " messages, expected " +
                  std::to_string(ZmqTransport::kControlOutboxMax) + " or one more");

    const TransportStats stats = transport.stats();
    CHECK_EQ(stats.control_dropped_full, static_cast<std::uint64_t>(refused));
    CHECK_EQ(stats.messages_sent, static_cast<std::uint64_t>(0));

    // Clearing empties the queue so the next enqueue succeeds again.
    transport.clear_outgoing();
    CHECK(transport.try_enqueue_control(serialize_client_hello("d", false)));

    transport.stop();
}

void test_enqueue_is_refused_when_not_running() {
    ZmqTransport transport;
    CHECK(!transport.is_running());
    CHECK(!transport.try_enqueue_control(serialize_client_hello("d", false)));
    // Setting a transform while stopped is a no-op rather than an error.
    transport.set_latest_transform(serialize_client_pose(ClientPoseMessage()));
    CHECK_EQ(transport.stats().messages_sent, static_cast<std::uint64_t>(0));

    // Stopping a transport that never started is safe.
    transport.stop();
    CHECK(!transport.is_running());
}

void test_latest_wins_transform_slot() {
    // With no peer, sends fail, so the single slot keeps only the newest pose.
    ZmqTransport transport;
    TransportConfig config;
    config.server_address = "127.0.0.1";
    config.control_port = 1;
    config.transform_port = 1;
    config.sub_port = 1;
    config.room_id = "latest";
    CHECK(transport.start(config));

    for (int i = 1; i <= 100; ++i) {
        ClientPoseMessage pose;
        pose.device_id = "d";
        pose.body.pose_seq = static_cast<std::uint16_t>(i);
        pose.body.flags = POSE_FLAG_HEAD_VALID;
        transport.set_latest_transform(serialize_client_pose(pose));
    }
    // Overwriting never fails and never queues: nothing was sent, and no
    // "would block" was charged for the 99 superseded frames.
    CHECK_EQ(transport.stats().messages_sent, static_cast<std::uint64_t>(0));
    CHECK_EQ(transport.stats().control_queue_length, 0);

    transport.stop();
}

void test_restart_after_stop() {
    FakeServer server;
    ZmqTransport transport;
    Collector collector;
    collector.attach(transport);

    TransportConfig config;
    config.server_address = "127.0.0.1";
    config.control_port = server.control_port();
    config.transform_port = server.transform_port();
    config.sub_port = server.pub_port();
    config.room_id = "restart";

    for (int round = 0; round < 3; ++round) {
        CHECK(transport.start(config));
        CHECK(transport.try_enqueue_control(serialize_client_hello("restart-device", false)));
        std::vector<std::uint8_t> identity;
        std::string room;
        std::vector<std::uint8_t> payload;
        CHECK(server.receive(server.control(), identity, room, payload));
        CHECK_EQ(peek_message_type(payload.data(), payload.size()),
                 static_cast<std::uint8_t>(MSG_CLIENT_HELLO));
        transport.stop();
        CHECK(!transport.is_running());
    }
    CHECK_EQ(collector.errors.load(), 0);
}

}  // namespace

int main() {
    test_enqueue_is_refused_when_not_running();
    test_control_framing_and_ordering();
    test_transform_lane_is_separate();
    test_sub_topic_routing_end_to_end();
    test_control_outbox_capacity_and_backpressure();
    test_latest_wins_transform_slot();
    test_restart_after_stop();
    return summary("transport queues and framing");
}
