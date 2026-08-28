// SPDX-License-Identifier: Apache-2.0
//
// SUB topic classification. ZeroMQ's ZMQ_SUBSCRIBE is a *prefix* filter, so
// subscribing to "room" also delivers "room\0obj" and, critically, every other
// room whose id starts with "room". Upstream therefore classifies topics
// byte-exactly in user space; anything less would cross rooms.

#include <string>
#include <vector>

#include "../test_support.hpp"
#include "transport/zmq_transport.hpp"

using namespace styly::netsync;
using namespace styly::netsync::test;

namespace {

std::vector<std::uint8_t> topic(const std::string &text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> object_topic(const std::string &room) {
    std::vector<std::uint8_t> out = topic(room);
    out.push_back(0x00);
    out.push_back('o');
    out.push_back('b');
    out.push_back('j');
    return out;
}

bool is_avatar(const std::vector<std::uint8_t> &bytes, const std::string &room) {
    return ZmqTransport::is_avatar_topic(bytes.data(), bytes.size(), room);
}

bool is_object(const std::vector<std::uint8_t> &bytes, const std::string &room) {
    return ZmqTransport::is_object_topic(bytes.data(), bytes.size(), room);
}

void test_exact_room_topic() {
    const std::string room = "default_room";
    CHECK(is_avatar(topic(room), room));
    CHECK(!is_object(topic(room), room));
}

void test_exact_object_topic() {
    const std::string room = "default_room";
    CHECK(is_object(object_topic(room), room));
    CHECK(!is_avatar(object_topic(room), room));
}

void test_prefix_sharing_rooms_are_rejected() {
    // This is the case a prefix match would get wrong: "room" is a prefix of
    // "room2", so a naive check would accept another room's traffic.
    const std::string room = "room";
    CHECK(!is_avatar(topic("room2"), room));
    CHECK(!is_object(topic("room2"), room));
    CHECK(!is_avatar(object_topic("room2"), room));
    CHECK(!is_object(object_topic("room2"), room));

    // A longer room must not match a shorter subscription either way round.
    CHECK(!is_avatar(topic("roo"), room));
    CHECK(!is_object(topic("roo"), room));
}

void test_near_miss_suffixes() {
    const std::string room = "r";
    // Right length, wrong suffix bytes.
    std::vector<std::uint8_t> wrong = topic("r");
    wrong.push_back(0x00);
    wrong.push_back('o');
    wrong.push_back('b');
    wrong.push_back('k');
    CHECK(!is_object(wrong, room));

    // Suffix without the leading NUL.
    CHECK(!is_object(topic("robj"), room));

    // Correct suffix, one byte short.
    std::vector<std::uint8_t> short_suffix = topic("r");
    short_suffix.push_back(0x00);
    short_suffix.push_back('o');
    short_suffix.push_back('b');
    CHECK(!is_object(short_suffix, room));

    // Correct suffix plus trailing junk.
    std::vector<std::uint8_t> long_suffix = object_topic("r");
    long_suffix.push_back('x');
    CHECK(!is_object(long_suffix, room));
}

void test_empty_and_null_inputs() {
    CHECK(!ZmqTransport::is_avatar_topic(nullptr, 0, "room"));
    CHECK(!ZmqTransport::is_object_topic(nullptr, 0, "room"));
    CHECK(!is_avatar(std::vector<std::uint8_t>(), "room"));
    // An empty room id is degenerate but must still classify consistently.
    CHECK(is_avatar(std::vector<std::uint8_t>(), ""));
    CHECK(is_object(object_topic(""), ""));
}

void test_room_ids_with_unusual_bytes() {
    // Room ids are opaque UTF-8; multi-byte and embedded-NUL ids must compare
    // by bytes, not by C-string semantics.
    const std::string unicode_room = "部屋-01";
    CHECK(is_avatar(topic(unicode_room), unicode_room));
    CHECK(is_object(object_topic(unicode_room), unicode_room));
    CHECK(!is_avatar(topic("部屋-02"), unicode_room));

    std::string embedded_nul = "a";
    embedded_nul.push_back('\0');
    embedded_nul += "b";
    CHECK_EQ(embedded_nul.size(), static_cast<std::size_t>(3));
    CHECK(is_avatar(topic(embedded_nul), embedded_nul));
    CHECK(is_object(object_topic(embedded_nul), embedded_nul));
    CHECK(!is_avatar(topic("a"), embedded_nul));
}

void test_endpoint_building() {
    CHECK_EQ(ZmqTransport::build_endpoint("192.168.1.10", 5555),
             std::string("tcp://192.168.1.10:5555"));
    CHECK_EQ(ZmqTransport::build_endpoint("tcp://192.168.1.10", 5557),
             std::string("tcp://192.168.1.10:5557"));
    CHECK_EQ(ZmqTransport::build_endpoint("localhost", 5556), std::string("tcp://localhost:5556"));
    // A port already present in the configured address is replaced, not appended.
    CHECK_EQ(ZmqTransport::build_endpoint("tcp://10.0.0.5:9999", 5555),
             std::string("tcp://10.0.0.5:5555"));
    // Hostnames containing digits after a colon-free name are untouched.
    CHECK_EQ(ZmqTransport::build_endpoint("server-01.local", 5555),
             std::string("tcp://server-01.local:5555"));
}

}  // namespace

int main() {
    test_exact_room_topic();
    test_exact_object_topic();
    test_prefix_sharing_rooms_are_rejected();
    test_near_miss_suffixes();
    test_empty_and_null_inputs();
    test_room_ids_with_unusual_bytes();
    test_endpoint_building();
    return summary("SUB topic routing");
}
