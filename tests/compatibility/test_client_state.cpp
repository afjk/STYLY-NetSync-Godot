// SPDX-License-Identifier: Apache-2.0
//
// NetSyncClient lifecycle and message routing, driven against an in-process
// stand-in server that speaks the real protocol.
//
// This is where the "ready is more than a connected socket" rule, the hello
// ordering, presence tracking, RPC/NV/object plumbing and the send-side
// throttles are pinned down.

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../fake_server.hpp"
#include "../test_support.hpp"
#include "core/device_id.hpp"
#include "core/netsync_client.hpp"

using namespace styly::netsync;
using namespace styly::netsync::test;

namespace {

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

/// Drives poll() until `predicate` holds or the deadline passes, accumulating
/// every event produced along the way.
bool pump_until(NetSyncClient &client, std::vector<Event> &events,
                const std::function<bool()> &predicate, int timeout_ms = 4000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::vector<Event> batch = client.poll(now_seconds());
        events.insert(events.end(), batch.begin(), batch.end());
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // One last poll so a predicate satisfied on the final iteration is seen.
    const std::vector<Event> batch = client.poll(now_seconds());
    events.insert(events.end(), batch.begin(), batch.end());
    return predicate();
}

void pump_for(NetSyncClient &client, std::vector<Event> &events, int milliseconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const std::vector<Event> batch = client.poll(now_seconds());
        events.insert(events.end(), batch.begin(), batch.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::size_t count_events(const std::vector<Event> &events, EventType type) {
    return static_cast<std::size_t>(
        std::count_if(events.begin(), events.end(),
                      [type](const Event &event) { return event.type == type; }));
}

const Event *find_event(const std::vector<Event> &events, EventType type) {
    for (const Event &event : events) {
        if (event.type == type) {
            return &event;
        }
    }
    return nullptr;
}

ClientConfig make_config(const FakeServer &server, const std::string &room,
                         const std::string &device_id) {
    ClientConfig config;
    config.server_address = "127.0.0.1";
    config.control_port = server.control_port();
    config.transform_port = server.transform_port();
    config.sub_port = server.pub_port();
    config.room_id = room;
    config.device_id = device_id;
    config.enable_discovery = false;
    config.transform_send_rate = 30.0;
    return config;
}

/// Read the client's hello off the control ROUTER and return its identity, so
/// the fake server can unicast back to it.
bool accept_hello(FakeServer &server, std::vector<std::uint8_t> &identity,
                  const std::string &expected_room, const std::string &expected_device_id) {
    std::string room;
    std::vector<std::uint8_t> payload;
    if (!server.receive(server.control(), identity, room, payload)) {
        return false;
    }
    if (room != expected_room) {
        return false;
    }
    if (peek_message_type(payload.data(), payload.size()) != MSG_CLIENT_HELLO) {
        return false;
    }
    ClientHelloMessage hello;
    if (!deserialize_client_hello(payload.data(), payload.size(), hello)) {
        return false;
    }
    return hello.device_id == expected_device_id;
}

std::vector<std::uint8_t> mapping_bytes(
    const std::vector<std::tuple<int, std::string, bool>> &entries) {
    DeviceIdMappingMessage message;
    message.server_version_major = 0;
    message.server_version_minor = 17;
    message.server_version_patch = 4;
    for (const auto &entry : entries) {
        DeviceIdMappingEntry item;
        item.client_no = static_cast<std::uint16_t>(std::get<0>(entry));
        item.device_id = std::get<1>(entry);
        item.is_stealth = std::get<2>(entry);
        message.mappings.push_back(item);
    }
    return serialize_device_id_mapping(message);
}

// --- Tests -------------------------------------------------------------------

void test_hello_is_first_and_ready_requires_handshake_and_sync() {
    FakeServer server;
    NetSyncClient client;
    const std::string room = "lifecycle";
    const std::string device = "godot-device-lifecycle";

    CHECK_EQ(static_cast<int>(client.state()), static_cast<int>(ConnectionState::Disconnected));
    CHECK(client.connect(make_config(server, room, device)));

    std::vector<Event> events;
    // The very first control message must be the hello.
    std::vector<std::uint8_t> identity;
    CHECK(pump_until(client, events,
                     [&] { return accept_hello(server, identity, room, device); }));

    // Sockets are up, but readiness needs the handshake and the NV sync too.
    CHECK(client.is_connected());
    CHECK(!client.has_handshake());
    CHECK(!client.is_ready());
    CHECK_MSG(client.state() == ConnectionState::Synchronizing ||
                  client.state() == ConnectionState::Connected,
              std::string("unexpected state ") + connection_state_name(client.state()));
    CHECK_EQ(count_events(events, EventType::Ready), static_cast<std::size_t>(0));

    // Device id mapping assigns the client number.
    server.send_control(identity, room, mapping_bytes({{5, device, false}}));
    CHECK(pump_until(client, events, [&] { return client.has_handshake(); }));
    CHECK_EQ(client.client_no(), 5);
    const Event *assigned = find_event(events, EventType::ClientNoAssigned);
    CHECK(assigned != nullptr);
    if (assigned != nullptr) {
        CHECK_EQ(assigned->client_no, 5);
    }

    // Still not ready: no network-variable sync yet.
    CHECK(!client.has_network_variable_sync());
    CHECK(!client.is_ready());
    CHECK_EQ(count_events(events, EventType::Ready), static_cast<std::size_t>(0));

    // The global sync completes readiness.
    GlobalVarSyncMessage sync;
    NetworkVariableEntry entry;
    entry.name = "phase";
    entry.value = "lobby";
    entry.last_writer_client_no = 1;
    sync.variables.push_back(entry);
    server.send_control(identity, room, serialize_global_var_sync(sync));

    CHECK(pump_until(client, events, [&] { return client.is_ready(); }));
    CHECK_EQ(static_cast<int>(client.state()), static_cast<int>(ConnectionState::Ready));
    CHECK_EQ(count_events(events, EventType::Ready), static_cast<std::size_t>(1));
    CHECK_EQ(client.get_global_variable("phase", "?"), std::string("lobby"));

    const Event *changed = find_event(events, EventType::GlobalVariableChanged);
    CHECK(changed != nullptr);
    if (changed != nullptr) {
        CHECK_EQ(changed->name, std::string("phase"));
        CHECK_EQ(changed->new_value, std::string("lobby"));
        CHECK(!changed->had_old_value);
    }

    // Ready fires once, not on every subsequent poll.
    std::vector<Event> later;
    pump_for(client, later, 100);
    CHECK_EQ(count_events(later, EventType::Ready), static_cast<std::size_t>(0));

    client.disconnect();
    CHECK_EQ(static_cast<int>(client.state()), static_cast<int>(ConnectionState::Disconnected));
}

void test_ready_without_variables_uses_the_timeout() {
    // A room with no network variables never produces a sync message; upstream
    // falls back to a 2 s timeout so such a room still becomes ready.
    FakeServer server;
    NetSyncClient client;
    const std::string room = "empty";
    const std::string device = "godot-device-empty";
    CHECK(client.connect(make_config(server, room, device)));

    std::vector<Event> events;
    std::vector<std::uint8_t> identity;
    CHECK(pump_until(client, events,
                     [&] { return accept_hello(server, identity, room, device); }));
    server.send_control(identity, room, mapping_bytes({{1, device, false}}));

    CHECK(pump_until(client, events, [&] { return client.has_handshake(); }));
    CHECK(!client.is_ready());

    // Nothing else is sent; readiness must arrive on the timeout alone.
    CHECK(pump_until(client, events, [&] { return client.is_ready(); },
                     static_cast<int>(NetworkVariableManager::kInitialSyncTimeout * 1000) + 2000));
    CHECK_EQ(count_events(events, EventType::Ready), static_cast<std::size_t>(1));

    client.disconnect();
}

void test_pose_send_rate_change_gating_and_heartbeat() {
    FakeServer server;
    NetSyncClient client;
    const std::string room = "pose";
    const std::string device = "godot-device-pose";
    ClientConfig config = make_config(server, room, device);
    config.transform_send_rate = 20.0;
    CHECK(client.connect(config));

    std::vector<Event> events;
    std::vector<std::uint8_t> identity;
    CHECK(pump_until(client, events,
                     [&] { return accept_hello(server, identity, room, device); }));

    ClientPoseBody body;
    body.flags = POSE_FLAG_HEAD_VALID | POSE_FLAG_PHYSICAL_VALID;
    body.head.position = Vec3(1.0, 1.6, -2.0);
    client.set_local_pose(body);

    // First pose goes out.
    std::string received_room;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> transform_identity;
    CHECK(pump_until(client, events, [&] {
        return server.receive(server.transform(), transform_identity, received_room, payload, 10);
    }));
    CHECK_EQ(received_room, room);
    ClientPoseMessage decoded;
    CHECK(deserialize_client_pose(payload.data(), payload.size(), decoded));
    CHECK_EQ(decoded.device_id, device);
    CHECK_EQ(decoded.body.flags, sanitize_pose_flags(body.flags));
    const std::uint16_t first_seq = decoded.body.pose_seq;
    CHECK(first_seq > 0);

    // An unchanged pose is suppressed until the 1 s heartbeat. Over 400 ms at
    // 20 Hz an unthrottled client would send eight more frames.
    pump_for(client, events, 400);
    CHECK(!server.receive(server.transform(), transform_identity, received_room, payload, 10));

    // Moving the head resumes sending, with an incremented sequence number.
    body.head.position = Vec3(1.5, 1.6, -2.0);
    client.set_local_pose(body);
    CHECK(pump_until(client, events, [&] {
        return server.receive(server.transform(), transform_identity, received_room, payload, 10);
    }));
    CHECK(deserialize_client_pose(payload.data(), payload.size(), decoded));
    CHECK(decoded.body.pose_seq > first_seq);
    CHECK_NEAR(decoded.body.head.position.x, 1.5, kAbsPosScale);

    client.disconnect();
}

void test_stealth_client_sends_only_the_stealth_pose() {
    FakeServer server;
    NetSyncClient client;
    const std::string room = "stealth";
    const std::string device = "godot-device-stealth";
    ClientConfig config = make_config(server, room, device);
    config.stealth_mode = true;
    CHECK(client.connect(config));

    std::vector<Event> events;
    std::vector<std::uint8_t> identity;
    std::string received_room;
    std::vector<std::uint8_t> payload;

    // The hello carries the stealth flag.
    CHECK(pump_until(client, events, [&] {
        std::string room_out;
        std::vector<std::uint8_t> bytes;
        if (!server.receive(server.control(), identity, room_out, bytes, 10)) {
            return false;
        }
        ClientHelloMessage hello;
        return deserialize_client_hello(bytes.data(), bytes.size(), hello) && hello.is_stealth();
    }));

    // And the pose it publishes carries only the stealth flag, no transforms —
    // even though a pose was submitted.
    ClientPoseBody body;
    body.flags = POSE_FLAG_HEAD_VALID;
    body.head.position = Vec3(9.0, 9.0, 9.0);
    client.set_local_pose(body);

    std::vector<std::uint8_t> transform_identity;
    CHECK(pump_until(client, events, [&] {
        return server.receive(server.transform(), transform_identity, received_room, payload, 10);
    }));
    ClientPoseMessage decoded;
    CHECK(deserialize_client_pose(payload.data(), payload.size(), decoded));
    CHECK_EQ(decoded.body.flags, static_cast<std::uint8_t>(POSE_FLAG_STEALTH));
    CHECK_EQ(decoded.body.head.position.x, 0.0);

    client.disconnect();
}

void test_rpc_round_trip_and_pre_ready_queueing() {
    FakeServer server;
    NetSyncClient client;
    const std::string room = "rpc";
    const std::string device = "godot-device-rpc";
    CHECK(client.connect(make_config(server, room, device)));

    std::vector<Event> events;
    std::vector<std::uint8_t> identity;
    CHECK(pump_until(client, events,
                     [&] { return accept_hello(server, identity, room, device); }));

    // Fired before ready: must be queued, not dropped and not sent yet.
    client.rpc("EarlyCall", {"a", "b"});
    pump_for(client, events, 100);
    std::string received_room;
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> ignored;
    CHECK(!server.receive(server.control(), ignored, received_room, payload, 50));

    // Become ready.
    server.send_control(identity, room, mapping_bytes({{4, device, false}}));
    server.send_control(identity, room, serialize_global_var_sync(GlobalVarSyncMessage()));
    CHECK(pump_until(client, events, [&] { return client.is_ready(); }));

    // The queued RPC is flushed with the now-known sender client number.
    CHECK(pump_until(client, events, [&] {
        return server.receive(server.control(), ignored, received_room, payload, 10);
    }));
    CHECK_EQ(peek_message_type(payload.data(), payload.size()),
             static_cast<std::uint8_t>(MSG_RPC));
    RpcMessage rpc;
    CHECK(deserialize_rpc(payload.data(), payload.size(), rpc));
    CHECK_EQ(rpc.function_name, std::string("EarlyCall"));
    CHECK_EQ(rpc.sender_client_no, static_cast<std::uint16_t>(4));
    CHECK_EQ(rpc.device_id, device);
    CHECK_EQ(rpc.target_client_nos.size(), static_cast<std::size_t>(0));
    CHECK_EQ(rpc.arguments_json, std::string("[\"a\",\"b\"]"));

    // Targeted RPC.
    client.rpc_to_many({2, 3}, "Targeted", {"x"});
    CHECK(pump_until(client, events, [&] {
        return server.receive(server.control(), ignored, received_room, payload, 10);
    }));
    CHECK(deserialize_rpc(payload.data(), payload.size(), rpc));
    CHECK_EQ(rpc.function_name, std::string("Targeted"));
    CHECK_EQ(rpc.target_client_nos.size(), static_cast<std::size_t>(2));
    CHECK_EQ(rpc.target_client_nos[0], static_cast<std::uint16_t>(2));
    CHECK_EQ(rpc.target_client_nos[1], static_cast<std::uint16_t>(3));

    // Inbound RPC becomes an event with decoded arguments.
    RpcMessage inbound;
    inbound.sender_client_no = 9;
    inbound.device_id = "peer";
    inbound.function_name = "ChangeColor";
    inbound.arguments_json = "[\"red\",\"1.5\"]";
    server.send_control(identity, room, serialize_rpc(inbound));

    std::vector<Event> rpc_events;
    CHECK(pump_until(client, rpc_events, [&] {
        return count_events(rpc_events, EventType::RpcReceived) > 0;
    }));
    const Event *received = find_event(rpc_events, EventType::RpcReceived);
    CHECK(received != nullptr);
    if (received != nullptr) {
        CHECK_EQ(received->client_no, 9);
        CHECK_EQ(received->name, std::string("ChangeColor"));
        CHECK_EQ(received->args.size(), static_cast<std::size_t>(2));
        CHECK_EQ(received->args[0], std::string("red"));
        CHECK_EQ(received->args[1], std::string("1.5"));
    }

    client.disconnect();
}

void test_network_variables_round_trip() {
    FakeServer server;
    NetSyncClient client;
    const std::string room = "nv";
    const std::string device = "godot-device-nv";
    CHECK(client.connect(make_config(server, room, device)));

    std::vector<Event> events;
    std::vector<std::uint8_t> identity;
    CHECK(pump_until(client, events,
                     [&] { return accept_hello(server, identity, room, device); }));
    server.send_control(identity, room, mapping_bytes({{6, device, false}}));
    server.send_control(identity, room, serialize_global_var_sync(GlobalVarSyncMessage()));
    CHECK(pump_until(client, events, [&] { return client.is_ready(); }));

    // Global set goes out on the leading edge.
    CHECK(client.set_global_variable("score", "100"));
    std::vector<std::uint8_t> ignored;
    std::string received_room;
    std::vector<std::uint8_t> payload;
    CHECK(pump_until(client, events, [&] {
        return server.receive(server.control(), ignored, received_room, payload, 10) &&
               peek_message_type(payload.data(), payload.size()) == MSG_GLOBAL_VAR_SET;
    }));
    GlobalVarSetMessage global_set;
    CHECK(deserialize_global_var_set(payload.data(), payload.size(), global_set));
    CHECK_EQ(global_set.variable_name, std::string("score"));
    CHECK_EQ(global_set.variable_value, std::string("100"));
    CHECK_EQ(global_set.sender_client_no, static_cast<std::uint16_t>(6));
    CHECK_EQ(global_set.device_id, device);

    // Client variable, targeted at self by default.
    CHECK(client.set_client_variable("nickname", "godot"));
    CHECK(pump_until(client, events, [&] {
        return server.receive(server.control(), ignored, received_room, payload, 10) &&
               peek_message_type(payload.data(), payload.size()) == MSG_CLIENT_VAR_SET;
    }));
    ClientVarSetMessage client_set;
    CHECK(deserialize_client_var_set(payload.data(), payload.size(), client_set));
    CHECK_EQ(client_set.target_client_no, static_cast<std::uint16_t>(6));
    CHECK_EQ(client_set.variable_name, std::string("nickname"));

    // Client-variable sync is an authoritative snapshot: a name that disappears
    // is reported as removed.
    ClientVarSyncMessage sync;
    NetworkVariableEntry a;
    a.name = "nickname";
    a.value = "godot";
    a.last_writer_client_no = 6;
    NetworkVariableEntry b;
    b.name = "team";
    b.value = "red";
    b.last_writer_client_no = 6;
    sync.clients.emplace_back(6, std::vector<NetworkVariableEntry>{a, b});
    server.send_control(identity, room, serialize_client_var_sync(sync));

    std::vector<Event> sync_events;
    CHECK(pump_until(client, sync_events, [&] {
        return client.get_client_variable(6, "team", "?") == "red";
    }));

    ClientVarSyncMessage shrunk;
    shrunk.clients.emplace_back(6, std::vector<NetworkVariableEntry>{a});
    server.send_control(identity, room, serialize_client_var_sync(shrunk));

    std::vector<Event> removal_events;
    CHECK(pump_until(client, removal_events, [&] {
        for (const Event &event : removal_events) {
            if (event.type == EventType::ClientVariableChanged && event.name == "team" &&
                event.removed) {
                return true;
            }
        }
        return false;
    }));
    CHECK_EQ(client.get_client_variable(6, "team", "gone"), std::string("gone"));
    CHECK_EQ(client.get_client_variable(6, "nickname", "?"), std::string("godot"));

    // Rejected writes: an over-long name and an over-long value.
    CHECK(!client.set_global_variable(std::string(65, 'n'), "v"));
    CHECK(!client.set_global_variable("ok", std::string(1025, 'v')));
    CHECK(!client.set_global_variable("", "v"));

    client.disconnect();
}

void test_presence_tracking_from_room_pose() {
    FakeServer server;
    NetSyncClient client;
    const std::string room = "presence";
    const std::string device = "godot-device-presence";
    CHECK(client.connect(make_config(server, room, device)));

    std::vector<Event> events;
    std::vector<std::uint8_t> identity;
    CHECK(pump_until(client, events,
                     [&] { return accept_hello(server, identity, room, device); }));
    server.send_control(identity, room,
                        mapping_bytes({{1, device, false}, {2, "peer-two", false}}));
    CHECK(pump_until(client, events, [&] { return client.has_handshake(); }));
    CHECK_EQ(client.client_no(), 1);

    // Let the SUB subscription settle before publishing.
    pump_for(client, events, 300);

    const auto publish_room_pose = [&](const std::vector<int> &client_numbers) {
        RoomPoseMessage message;
        message.room_id = room;
        message.broadcast_time = now_seconds();
        for (int client_no : client_numbers) {
            RoomPoseClient entry;
            entry.client_no = static_cast<std::uint16_t>(client_no);
            entry.pose_time = message.broadcast_time;
            entry.body.flags = POSE_FLAG_HEAD_VALID;
            entry.body.head.position = Vec3(client_no, 1.6, -client_no);
            message.clients.push_back(entry);
        }
        server.publish(room, serialize_room_pose(message));
    };

    // Both the local client and a peer are present; only the peer is announced.
    std::vector<Event> presence;
    CHECK(pump_until(client, presence,
                     [&] {
                         publish_room_pose({1, 2});
                         return count_events(presence, EventType::AvatarConnected) > 0;
                     }));
    const Event *connected = find_event(presence, EventType::AvatarConnected);
    CHECK(connected != nullptr);
    if (connected != nullptr) {
        CHECK_EQ(connected->client_no, 2);
        CHECK_EQ(connected->name, std::string("peer-two"));
    }
    // The local avatar is never reported as a remote one.
    for (const Event &event : presence) {
        if (event.type == EventType::AvatarConnected) {
            CHECK(event.client_no != 1);
        }
    }

    RemoteClientPose pose;
    CHECK(client.get_remote_pose(2, pose));
    CHECK_NEAR(pose.body.head.position.x, 2.0, kAbsPosScale);
    CHECK(!client.get_remote_pose(1, pose));  // local client excluded

    // Connection is announced once, not on every frame.
    std::vector<Event> repeats;
    for (int i = 0; i < 10; ++i) {
        publish_room_pose({1, 2});
        pump_for(client, repeats, 30);
    }
    CHECK_EQ(count_events(repeats, EventType::AvatarConnected), static_cast<std::size_t>(0));

    // The peer leaves.
    std::vector<Event> departure;
    CHECK(pump_until(client, departure,
                     [&] {
                         publish_room_pose({1});
                         return count_events(departure, EventType::AvatarDisconnected) > 0;
                     }));
    const Event *disconnected = find_event(departure, EventType::AvatarDisconnected);
    CHECK(disconnected != nullptr);
    if (disconnected != nullptr) {
        CHECK_EQ(disconnected->client_no, 2);
    }
    CHECK(!client.get_remote_pose(2, pose));

    client.disconnect();
}

void test_object_ownership_and_pose_flow() {
    FakeServer server;
    NetSyncClient client;
    const std::string room = "objects";
    const std::string device = "godot-device-objects";
    CHECK(client.connect(make_config(server, room, device)));

    std::vector<Event> events;
    std::vector<std::uint8_t> identity;
    CHECK(pump_until(client, events,
                     [&] { return accept_hello(server, identity, room, device); }));
    server.send_control(identity, room, mapping_bytes({{8, device, false}}));
    server.send_control(identity, room, serialize_global_var_sync(GlobalVarSyncMessage()));
    CHECK(pump_until(client, events, [&] { return client.is_ready(); }));

    const std::uint32_t object_id = 0x1234ABCD;
    CHECK(client.register_object(object_id));
    CHECK(!client.register_object(0));  // id 0 is invalid

    // Requesting ownership emits the control message with operation type 2.
    CHECK(client.request_object_ownership(object_id));
    std::vector<std::uint8_t> ignored;
    std::string received_room;
    std::vector<std::uint8_t> payload;
    CHECK(pump_until(client, events, [&] {
        return server.receive(server.control(), ignored, received_room, payload, 10) &&
               peek_message_type(payload.data(), payload.size()) ==
                   MSG_OBJECT_OWNERSHIP_REQUEST;
    }));
    ObjectOwnershipRequestMessage request;
    CHECK(deserialize_object_ownership_request(payload.data(), payload.size(), request));
    CHECK_EQ(request.operation_type, static_cast<std::uint8_t>(OWNERSHIP_OP_REQUEST));
    CHECK_EQ(request.object_id, object_id);
    CHECK_EQ(request.device_id, device);

    // The server grants it.
    ObjectOwnershipChangedMessage changed;
    changed.object_id = object_id;
    changed.new_owner_client_no = 8;
    changed.previous_owner_client_no = 0;
    server.send_control(identity, room, serialize_object_ownership_changed(changed));

    std::vector<Event> ownership_events;
    CHECK(pump_until(client, ownership_events, [&] {
        return count_events(ownership_events, EventType::ObjectOwnershipChanged) > 0;
    }));
    const Event *ownership = find_event(ownership_events, EventType::ObjectOwnershipChanged);
    CHECK(ownership != nullptr);
    if (ownership != nullptr) {
        CHECK_EQ(ownership->object_id, object_id);
        CHECK_EQ(ownership->value_a, 8);
        CHECK_EQ(ownership->value_b, 0);
    }

    ObjectState state;
    CHECK(client.get_object_state(object_id, state));
    CHECK_EQ(state.owner_client_no, 8);

    // Now that this client owns it, submitted poses are published.
    PoseTransform pose;
    pose.position = Vec3(3.0, 1.0, -4.0);
    pose.rotation = yaw_degrees_to_quaternion(90.0);
    client.submit_object_pose(object_id, pose);

    std::vector<std::uint8_t> transform_identity;
    CHECK(pump_until(client, events, [&] {
        return server.receive(server.transform(), transform_identity, received_room, payload,
                              10) &&
               peek_message_type(payload.data(), payload.size()) == MSG_OBJECT_POSE;
    }));
    ObjectPoseMessage object_pose;
    CHECK(deserialize_object_pose(payload.data(), payload.size(), object_pose));
    CHECK_EQ(object_pose.object_id, object_id);
    CHECK_EQ(object_pose.device_id, device);
    CHECK_NEAR(object_pose.position.x, 3.0, kAbsPosScale);
    CHECK(object_pose.pose_seq > 0);

    // Ownership rejection surfaces the reason code.
    ObjectOwnershipRejectedMessage rejected;
    rejected.object_id = object_id;
    rejected.current_owner_client_no = 12;
    rejected.reason_code = OWNERSHIP_REJECT_NOT_OWNER;
    server.send_control(identity, room, serialize_object_ownership_rejected(rejected));

    std::vector<Event> rejection_events;
    CHECK(pump_until(client, rejection_events, [&] {
        return count_events(rejection_events, EventType::ObjectOwnershipRejected) > 0;
    }));
    const Event *rejection = find_event(rejection_events, EventType::ObjectOwnershipRejected);
    CHECK(rejection != nullptr);
    if (rejection != nullptr) {
        CHECK_EQ(rejection->object_id, object_id);
        CHECK_EQ(rejection->value_a, 12);
        CHECK_EQ(rejection->value_b, static_cast<int>(OWNERSHIP_REJECT_NOT_OWNER));
    }

    // Releasing emits operation type 1.
    CHECK(client.release_object_ownership(object_id));
    CHECK(pump_until(client, events, [&] {
        return server.receive(server.control(), ignored, received_room, payload, 10) &&
               peek_message_type(payload.data(), payload.size()) ==
                   MSG_OBJECT_OWNERSHIP_REQUEST;
    }));
    CHECK(deserialize_object_ownership_request(payload.data(), payload.size(), request));
    CHECK_EQ(request.operation_type, static_cast<std::uint8_t>(OWNERSHIP_OP_RELEASE));

    // A room-objects broadcast drives non-owned object poses.
    pump_for(client, events, 200);
    RoomObjectsMessage room_objects;
    room_objects.broadcast_time = now_seconds();
    RoomObjectState remote_state;
    remote_state.object_id = object_id;
    remote_state.owner_client_no = 99;
    remote_state.pose_seq = 42;
    remote_state.pose_time = room_objects.broadcast_time;
    remote_state.position = Vec3(-1.0, 2.0, 3.0);
    remote_state.rotation = yaw_degrees_to_quaternion(45.0);
    room_objects.objects.push_back(remote_state);

    std::vector<std::uint8_t> object_topic(room.begin(), room.end());
    object_topic.push_back(0x00);
    object_topic.push_back('o');
    object_topic.push_back('b');
    object_topic.push_back('j');

    std::vector<Event> broadcast_events;
    CHECK(pump_until(client, broadcast_events, [&] {
        server.publish(object_topic, serialize_room_objects(room_objects));
        ObjectState current;
        return client.get_object_state(object_id, current) && current.owner_client_no == 99 &&
               current.has_pose;
    }));
    CHECK(client.get_object_state(object_id, state));
    CHECK_EQ(state.owner_client_no, 99);
    CHECK_EQ(state.pose_seq, static_cast<std::uint16_t>(42));
    CHECK_NEAR(state.pose.position.x, -1.0, kAbsPosScale);

    client.unregister_object(object_id);
    CHECK(!client.get_object_state(object_id, state));

    client.disconnect();
}

void test_disconnect_clears_state_and_reconnect_works() {
    FakeServer server;
    NetSyncClient client;
    const std::string room = "reconnect";
    const std::string device = "godot-device-reconnect";

    for (int round = 0; round < 2; ++round) {
        CHECK(client.connect(make_config(server, room, device)));
        std::vector<Event> events;
        std::vector<std::uint8_t> identity;
        CHECK(pump_until(client, events,
                         [&] { return accept_hello(server, identity, room, device); }));
        server.send_control(identity, room, mapping_bytes({{11, device, false}}));
        server.send_control(identity, room, serialize_global_var_sync(GlobalVarSyncMessage()));
        CHECK(pump_until(client, events, [&] { return client.is_ready(); }));
        CHECK_EQ(client.client_no(), 11);

        client.disconnect();
        // Every piece of room-scoped state is dropped.
        CHECK_EQ(client.client_no(), 0);
        CHECK(!client.is_ready());
        CHECK(!client.has_network_variable_sync());
        CHECK_EQ(client.remote_client_numbers().size(), static_cast<std::size_t>(0));
        CHECK_EQ(client.get_global_variable("anything", "default"), std::string("default"));
    }
}

void test_device_id_persistence() {
    const std::string path = "/tmp/styly_netsync_device_id_test.txt";
    std::remove(path.c_str());

    const std::string first = load_or_create_device_id(path);
    CHECK(!first.empty());
    CHECK_EQ(first.size(), static_cast<std::size_t>(36));  // UUID v4, hyphenated
    CHECK_EQ(first[14], '4');                              // version nibble

    // A second call returns the same value: the id must survive a restart.
    const std::string second = load_or_create_device_id(path);
    CHECK_EQ(second, first);
    CHECK_EQ(read_device_id(path), first);

    // Two freshly generated ids differ.
    CHECK(generate_uuid_v4() != generate_uuid_v4());

    std::remove(path.c_str());
    CHECK_EQ(read_device_id(path), std::string());
}

}  // namespace

int main() {
    test_device_id_persistence();
    test_hello_is_first_and_ready_requires_handshake_and_sync();
    test_ready_without_variables_uses_the_timeout();
    test_pose_send_rate_change_gating_and_heartbeat();
    test_stealth_client_sends_only_the_stealth_pose();
    test_rpc_round_trip_and_pre_ready_queueing();
    test_network_variables_round_trip();
    test_presence_tracking_from_room_pose();
    test_object_ownership_and_pose_flow();
    test_disconnect_clears_state_and_reconnect_works();
    return summary("client lifecycle and routing");
}
