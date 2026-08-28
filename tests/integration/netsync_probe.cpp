// SPDX-License-Identifier: Apache-2.0
//
// Headless NetSync client driven over stdin, used by the integration tests to
// talk to a real STYLY NetSync server without Godot in the loop.
//
// Commands arrive one per line on stdin; every event and command result is
// written to stdout as a single JSON object per line, so the Python harness can
// assert on it. See tests/integration/README.md for the command reference.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/device_id.hpp"
#include "core/json_util.hpp"
#include "core/netsync_client.hpp"

using namespace styly::netsync;

namespace {

std::mutex g_output_mutex;

void emit(const std::string &json_object) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::fputs(json_object.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

std::string json_field(const std::string &key, const std::string &value) {
    return encode_json_string(key) + ":" + encode_json_string(value);
}

std::string json_field(const std::string &key, long long value) {
    return encode_json_string(key) + ":" + std::to_string(value);
}

std::string json_field(const std::string &key, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return encode_json_string(key) + ":" + buffer;
}

std::string json_field(const std::string &key, bool value) {
    return encode_json_string(key) + ":" + (value ? "true" : "false");
}

std::string json_object(const std::vector<std::string> &fields) {
    std::string out = "{";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out += fields[i];
    }
    out.push_back('}');
    return out;
}

const char *event_name(EventType type) {
    switch (type) {
        case EventType::ConnectionStateChanged:
            return "connection_state_changed";
        case EventType::Ready:
            return "ready";
        case EventType::ConnectionError:
            return "connection_error";
        case EventType::ServerDiscovered:
            return "server_discovered";
        case EventType::ClientNoAssigned:
            return "client_no_assigned";
        case EventType::AvatarConnected:
            return "avatar_connected";
        case EventType::AvatarDisconnected:
            return "avatar_disconnected";
        case EventType::RpcReceived:
            return "rpc_received";
        case EventType::GlobalVariableChanged:
            return "global_variable_changed";
        case EventType::ClientVariableChanged:
            return "client_variable_changed";
        case EventType::ObjectOwnershipChanged:
            return "object_ownership_changed";
        case EventType::ObjectOwnershipRejected:
            return "object_ownership_rejected";
        case EventType::ServerVersion:
            return "server_version";
        case EventType::Log:
            return "log";
    }
    return "unknown";
}

void emit_event(const Event &event) {
    std::vector<std::string> fields;
    fields.push_back(json_field("event", std::string(event_name(event.type))));
    fields.push_back(json_field("client_no", static_cast<long long>(event.client_no)));
    fields.push_back(json_field("value_a", static_cast<long long>(event.value_a)));
    fields.push_back(json_field("value_b", static_cast<long long>(event.value_b)));
    fields.push_back(json_field("value_c", static_cast<long long>(event.value_c)));
    fields.push_back(json_field("object_id", static_cast<long long>(event.object_id)));
    fields.push_back(json_field("name", event.name));
    fields.push_back(json_field("old_value", event.old_value));
    fields.push_back(json_field("new_value", event.new_value));
    fields.push_back(json_field("had_old_value", event.had_old_value));
    fields.push_back(json_field("removed", event.removed));
    fields.push_back(encode_json_string("args") + ":" + encode_json_string_array(event.args));
    emit(json_object(fields));
}

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::vector<std::string> tokenize(const std::string &line) {
    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string token;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

double to_double(const std::string &text) { return std::strtod(text.c_str(), nullptr); }
long long to_int(const std::string &text) { return std::strtoll(text.c_str(), nullptr, 0); }

/// Reads stdin on its own thread so the poll loop never blocks.
class LineReader {
public:
    LineReader() : thread_([this] { run(); }) {}
    ~LineReader() {
        stopped_.store(true);
        if (thread_.joinable()) {
            thread_.detach();  // stdin read cannot be interrupted portably
        }
    }

    bool next(std::string &line) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lines_.empty()) {
            return false;
        }
        line = std::move(lines_.front());
        lines_.pop_front();
        return true;
    }

    bool finished() const { return finished_.load(); }

private:
    void run() {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.push_back(line);
        }
        finished_.store(true);
    }

    std::mutex mutex_;
    std::deque<std::string> lines_;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> finished_{false};
    std::thread thread_;
};

}  // namespace

int main(int argc, char **argv) {
    ClientConfig config;
    config.room_id = "probe_room";
    config.enable_discovery = false;
    config.transform_send_rate = 20.0;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto value = [&](const char *fallback) -> std::string {
            return i + 1 < argc ? argv[++i] : fallback;
        };
        if (argument == "--server") {
            config.server_address = value("");
        } else if (argument == "--control") {
            config.control_port = static_cast<int>(to_int(value("5555")));
        } else if (argument == "--transform") {
            config.transform_port = static_cast<int>(to_int(value("5557")));
        } else if (argument == "--sub") {
            config.sub_port = static_cast<int>(to_int(value("5556")));
        } else if (argument == "--discovery-port") {
            config.discovery_port = static_cast<int>(to_int(value("9999")));
        } else if (argument == "--room") {
            config.room_id = value("probe_room");
        } else if (argument == "--device") {
            config.device_id = value("");
        } else if (argument == "--stealth") {
            config.stealth_mode = true;
        } else if (argument == "--discover") {
            config.enable_discovery = true;
            config.server_address.clear();
        } else if (argument == "--send-rate") {
            config.transform_send_rate = to_double(value("20"));
        } else {
            emit(json_object({json_field("result", std::string("error")),
                              json_field("message", "unknown argument " + argument)}));
            return 2;
        }
    }

    if (config.device_id.empty()) {
        config.device_id = generate_uuid_v4();
    }

    NetSyncClient client;
    LineReader reader;

    emit(json_object({json_field("result", std::string("started")),
                      json_field("device_id", config.device_id),
                      json_field("room_id", config.room_id)}));

    bool running = true;
    double wait_ready_deadline = -1.0;
    double sleep_until = -1.0;

    while (running) {
        for (const Event &event : client.poll(now_seconds())) {
            emit_event(event);
        }

        if (sleep_until > 0.0) {
            if (now_seconds() < sleep_until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            sleep_until = -1.0;
            emit(json_object({json_field("result", std::string("slept"))}));
        }

        if (wait_ready_deadline > 0.0) {
            if (client.is_ready()) {
                wait_ready_deadline = -1.0;
                emit(json_object({json_field("result", std::string("ready")),
                                  json_field("client_no",
                                             static_cast<long long>(client.client_no()))}));
            } else if (now_seconds() > wait_ready_deadline) {
                wait_ready_deadline = -1.0;
                emit(json_object(
                    {json_field("result", std::string("timeout")),
                     json_field("state", std::string(connection_state_name(client.state()))),
                     json_field("connected", client.is_connected()),
                     json_field("handshake", client.has_handshake()),
                     json_field("nv_sync", client.has_network_variable_sync())}));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
        }

        std::string line;
        if (!reader.next(line)) {
            if (reader.finished()) {
                running = false;
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const std::vector<std::string> tokens = tokenize(line);
        if (tokens.empty()) {
            continue;
        }
        const std::string &command = tokens[0];

        if (command == "connect") {
            const bool ok = client.connect(config);
            emit(json_object({json_field("result", std::string("connect")),
                              json_field("ok", ok)}));
        } else if (command == "disconnect") {
            client.disconnect();
            emit(json_object({json_field("result", std::string("disconnect"))}));
        } else if (command == "wait_ready") {
            const double timeout = tokens.size() > 1 ? to_double(tokens[1]) : 10.0;
            wait_ready_deadline = now_seconds() + timeout;
        } else if (command == "sleep") {
            sleep_until = now_seconds() + (tokens.size() > 1 ? to_double(tokens[1]) : 0.1);
        } else if (command == "state") {
            emit(json_object(
                {json_field("result", std::string("state")),
                 json_field("state", std::string(connection_state_name(client.state()))),
                 json_field("connected", client.is_connected()),
                 json_field("ready", client.is_ready()),
                 json_field("handshake", client.has_handshake()),
                 json_field("nv_sync", client.has_network_variable_sync()),
                 json_field("client_no", static_cast<long long>(client.client_no())),
                 json_field("device_id", client.device_id()),
                 json_field("server_address", client.server_address())}));
        } else if (command == "pose" && tokens.size() >= 4) {
            // Wire-space head pose: x y z [yaw_degrees].
            ClientPoseBody body;
            body.flags = POSE_FLAG_HEAD_VALID | POSE_FLAG_PHYSICAL_VALID;
            body.head.position = Vec3(to_double(tokens[1]), to_double(tokens[2]),
                                      to_double(tokens[3]));
            if (tokens.size() >= 5) {
                body.head.rotation = yaw_degrees_to_quaternion(to_double(tokens[4]));
            }
            client.set_local_pose(body);
            emit(json_object({json_field("result", std::string("pose"))}));
        } else if (command == "hands" && tokens.size() >= 7) {
            ClientPoseBody body;
            body.flags = POSE_FLAG_HEAD_VALID | POSE_FLAG_PHYSICAL_VALID |
                         POSE_FLAG_RIGHT_VALID | POSE_FLAG_LEFT_VALID;
            body.head.position = Vec3(to_double(tokens[1]), to_double(tokens[2]),
                                      to_double(tokens[3]));
            body.right_hand.position =
                Vec3(to_double(tokens[1]) + to_double(tokens[4]), to_double(tokens[2]),
                     to_double(tokens[3]));
            body.left_hand.position =
                Vec3(to_double(tokens[1]) + to_double(tokens[5]), to_double(tokens[2]),
                     to_double(tokens[3]));
            body.xr_origin_delta_yaw = to_double(tokens[6]);
            client.set_local_pose(body);
            emit(json_object({json_field("result", std::string("hands"))}));
        } else if (command == "rpc" && tokens.size() >= 2) {
            const std::vector<std::string> args(tokens.begin() + 2, tokens.end());
            client.rpc(tokens[1], args);
            emit(json_object({json_field("result", std::string("rpc"))}));
        } else if (command == "rpc_to" && tokens.size() >= 3) {
            const std::vector<std::string> args(tokens.begin() + 3, tokens.end());
            client.rpc_to(static_cast<int>(to_int(tokens[1])), tokens[2], args);
            emit(json_object({json_field("result", std::string("rpc_to"))}));
        } else if (command == "set_global" && tokens.size() >= 3) {
            const bool ok = client.set_global_variable(tokens[1], tokens[2]);
            emit(json_object({json_field("result", std::string("set_global")),
                              json_field("ok", ok)}));
        } else if (command == "get_global" && tokens.size() >= 2) {
            emit(json_object({json_field("result", std::string("get_global")),
                              json_field("name", tokens[1]),
                              json_field("value", client.get_global_variable(tokens[1], ""))}));
        } else if (command == "set_client_var" && tokens.size() >= 3) {
            const bool ok = client.set_client_variable(tokens[1], tokens[2]);
            emit(json_object({json_field("result", std::string("set_client_var")),
                              json_field("ok", ok)}));
        } else if (command == "set_client_var_for" && tokens.size() >= 4) {
            const bool ok = client.set_client_variable_for(
                static_cast<int>(to_int(tokens[1])), tokens[2], tokens[3]);
            emit(json_object({json_field("result", std::string("set_client_var_for")),
                              json_field("ok", ok)}));
        } else if (command == "get_client_var" && tokens.size() >= 3) {
            emit(json_object(
                {json_field("result", std::string("get_client_var")),
                 json_field("client_no", to_int(tokens[1])), json_field("name", tokens[2]),
                 json_field("value", client.get_client_variable(
                                         static_cast<int>(to_int(tokens[1])), tokens[2], ""))}));
        } else if (command == "clear_client_vars") {
            emit(json_object({json_field("result", std::string("clear_client_vars")),
                              json_field("ok", client.clear_my_client_variables())}));
        } else if (command == "register_object" && tokens.size() >= 2) {
            const bool ok =
                client.register_object(static_cast<std::uint32_t>(to_int(tokens[1])));
            emit(json_object({json_field("result", std::string("register_object")),
                              json_field("ok", ok)}));
        } else if (command == "request_ownership" && tokens.size() >= 2) {
            const bool ok =
                client.request_object_ownership(static_cast<std::uint32_t>(to_int(tokens[1])));
            emit(json_object({json_field("result", std::string("request_ownership")),
                              json_field("ok", ok)}));
        } else if (command == "release_ownership" && tokens.size() >= 2) {
            const bool ok =
                client.release_object_ownership(static_cast<std::uint32_t>(to_int(tokens[1])));
            emit(json_object({json_field("result", std::string("release_ownership")),
                              json_field("ok", ok)}));
        } else if (command == "object_pose" && tokens.size() >= 5) {
            PoseTransform pose;
            pose.position = Vec3(to_double(tokens[2]), to_double(tokens[3]),
                                 to_double(tokens[4]));
            if (tokens.size() >= 6) {
                pose.rotation = yaw_degrees_to_quaternion(to_double(tokens[5]));
            }
            client.submit_object_pose(static_cast<std::uint32_t>(to_int(tokens[1])), pose);
            emit(json_object({json_field("result", std::string("object_pose"))}));
        } else if (command == "get_object" && tokens.size() >= 2) {
            ObjectState state;
            const bool found =
                client.get_object_state(static_cast<std::uint32_t>(to_int(tokens[1])), state);
            emit(json_object(
                {json_field("result", std::string("get_object")), json_field("found", found),
                 json_field("object_id", static_cast<long long>(state.object_id)),
                 json_field("owner_client_no", static_cast<long long>(state.owner_client_no)),
                 json_field("pose_seq", static_cast<long long>(state.pose_seq)),
                 json_field("has_pose", state.has_pose), json_field("x", state.pose.position.x),
                 json_field("y", state.pose.position.y),
                 json_field("z", state.pose.position.z)}));
        } else if (command == "remote_pose" && tokens.size() >= 2) {
            RemoteClientPose pose;
            const bool found =
                client.get_remote_pose(static_cast<int>(to_int(tokens[1])), pose);
            emit(json_object(
                {json_field("result", std::string("remote_pose")), json_field("found", found),
                 json_field("client_no", static_cast<long long>(pose.client_no)),
                 json_field("flags", static_cast<long long>(pose.body.flags)),
                 json_field("x", pose.body.head.position.x),
                 json_field("y", pose.body.head.position.y),
                 json_field("z", pose.body.head.position.z),
                 json_field("yaw", quaternion_to_yaw_degrees(pose.body.head.rotation))}));
        } else if (command == "remote_clients") {
            std::vector<std::string> numbers;
            for (int client_no : client.remote_client_numbers()) {
                numbers.push_back(std::to_string(client_no));
            }
            emit(json_object({json_field("result", std::string("remote_clients")),
                              encode_json_string("clients") + ":" +
                                  encode_json_string_array(numbers)}));
        } else if (command == "known_clients") {
            std::vector<std::string> numbers;
            for (int client_no : client.known_client_numbers()) {
                numbers.push_back(std::to_string(client_no));
            }
            emit(json_object({json_field("result", std::string("known_clients")),
                              encode_json_string("clients") + ":" +
                                  encode_json_string_array(numbers)}));
        } else if (command == "stats") {
            const TransportStats stats = client.transport_stats();
            emit(json_object(
                {json_field("result", std::string("stats")),
                 json_field("messages_sent", static_cast<long long>(stats.messages_sent)),
                 json_field("messages_received",
                            static_cast<long long>(stats.messages_received)),
                 json_field("dropped_transform_frames",
                            static_cast<long long>(stats.dropped_transform_frames)),
                 json_field("would_block", static_cast<long long>(stats.would_block_count)),
                 json_field("control_dropped_full",
                            static_cast<long long>(stats.control_dropped_full))}));
        } else if (command == "quit") {
            running = false;
        } else {
            emit(json_object({json_field("result", std::string("error")),
                              json_field("message", "unrecognised command: " + line)}));
        }
    }

    client.disconnect();
    emit(json_object({json_field("result", std::string("exited"))}));
    return 0;
}
