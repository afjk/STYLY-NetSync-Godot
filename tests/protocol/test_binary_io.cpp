// SPDX-License-Identifier: Apache-2.0
// BinaryWriter / BinaryReader behaviour, including the bounds checking that
// keeps a malformed payload from reading past the end of the buffer.

#include <string>
#include <vector>

#include "../test_support.hpp"
#include "protocol/binary_reader.hpp"
#include "protocol/binary_writer.hpp"
#include "protocol/protocol_v8.hpp"

using namespace styly::netsync;
using namespace styly::netsync::test;

namespace {

void test_little_endian_integers() {
    BinaryWriter writer;
    writer.write_u8(0x12);
    writer.write_u16(0x3456);
    writer.write_u32(0x789ABCDE);
    writer.write_i16(-2);
    writer.write_i32(-2);

    const std::vector<std::uint8_t> expected = {0x12, 0x56, 0x34, 0xDE, 0xBC, 0x9A,
                                                0x78, 0xFE, 0xFF, 0xFE, 0xFF, 0xFF, 0xFF};
    CHECK_BYTES(writer.buffer(), expected);

    BinaryReader reader(writer.buffer());
    CHECK_EQ(reader.read_u8(), static_cast<std::uint8_t>(0x12));
    CHECK_EQ(reader.read_u16(), static_cast<std::uint16_t>(0x3456));
    CHECK_EQ(reader.read_u32(), 0x789ABCDEu);
    CHECK_EQ(reader.read_i16(), static_cast<std::int16_t>(-2));
    CHECK_EQ(reader.read_i32(), -2);
    CHECK(reader.ok());
    CHECK_EQ(reader.remaining(), static_cast<std::size_t>(0));
}

void test_int24_round_trip() {
    const std::int32_t values[] = {0,        1,        -1,       127,     -128,
                                   0x7FFFFF, -0x800000, 0x123456, -0x123456};
    for (std::int32_t value : values) {
        BinaryWriter writer;
        writer.write_i24(value);
        CHECK_EQ(writer.size(), static_cast<std::size_t>(3));
        BinaryReader reader(writer.buffer());
        CHECK_EQ(reader.read_i24(), value);
    }

    // Out-of-range values clamp rather than wrap.
    BinaryWriter high;
    high.write_i24(0x7FFFFFF);
    BinaryReader high_reader(high.buffer());
    CHECK_EQ(high_reader.read_i24(), 0x7FFFFF);

    BinaryWriter low;
    low.write_i24(-0x7FFFFFF);
    BinaryReader low_reader(low.buffer());
    CHECK_EQ(low_reader.read_i24(), -0x800000);
}

void test_double_round_trip() {
    const double values[] = {0.0, -0.0, 1.0, -1.0, 3.141592653589793, 1e300, -1e-300};
    for (double value : values) {
        BinaryWriter writer;
        writer.write_f64(value);
        CHECK_EQ(writer.size(), static_cast<std::size_t>(8));
        BinaryReader reader(writer.buffer());
        CHECK_EQ(reader.read_f64(), value);
    }
    // Little-endian layout of 1.0.
    BinaryWriter one;
    one.write_f64(1.0);
    const std::vector<std::uint8_t> expected = {0, 0, 0, 0, 0, 0, 0xF0, 0x3F};
    CHECK_BYTES(one.buffer(), expected);
}

void test_strings() {
    BinaryWriter writer;
    writer.write_string8("hello");
    writer.write_string16("world");
    const std::vector<std::uint8_t> expected = {5,   'h', 'e', 'l', 'l', 'o', 5,
                                                0,   'w', 'o', 'r', 'l', 'd'};
    CHECK_BYTES(writer.buffer(), expected);

    BinaryReader reader(writer.buffer());
    CHECK_EQ(reader.read_string8(), std::string("hello"));
    CHECK_EQ(reader.read_string16(), std::string("world"));

    // UTF-8 length prefixes count bytes, not characters.
    BinaryWriter unicode;
    unicode.write_string8("日本語");
    CHECK_EQ(unicode.buffer()[0], static_cast<std::uint8_t>(9));
    BinaryReader unicode_reader(unicode.buffer());
    CHECK_EQ(unicode_reader.read_string8(), std::string("日本語"));
}

void test_string8_truncation_is_utf8_safe() {
    // 200 three-byte characters is 600 bytes: the length prefix caps at 255, and
    // the cut must land on a character boundary rather than splitting one.
    const std::string long_value = [] {
        std::string out;
        for (int i = 0; i < 200; ++i) {
            out += "あ";
        }
        return out;
    }();

    BinaryWriter writer;
    writer.write_string8(long_value);
    const std::size_t length = writer.buffer()[0];
    CHECK(length <= 255);
    CHECK_EQ(length % 3, static_cast<std::size_t>(0));  // whole characters only
    CHECK_EQ(length, static_cast<std::size_t>(255));    // 85 characters exactly

    BinaryReader reader(writer.buffer());
    const std::string decoded = reader.read_string8();
    CHECK_EQ(decoded.size(), static_cast<std::size_t>(255));
    CHECK(reader.ok());

    // An ASCII device id at exactly 255 bytes is untouched.
    const std::string ascii(255, 'd');
    BinaryWriter ascii_writer;
    ascii_writer.write_string8(ascii);
    BinaryReader ascii_reader(ascii_writer.buffer());
    CHECK_EQ(ascii_reader.read_string8(), ascii);
}

void test_code_point_truncation() {
    CHECK_EQ(truncate_utf8_code_points("abcdef", 3), std::string("abc"));
    CHECK_EQ(truncate_utf8_code_points("あいうえお", 2), std::string("あい"));
    CHECK_EQ(truncate_utf8_code_points("abc", 10), std::string("abc"));
    CHECK_EQ(truncate_utf8_code_points("", 4), std::string(""));
}

void test_reader_bounds_checking() {
    const std::vector<std::uint8_t> payload = {1, 2, 3};
    BinaryReader reader(payload);
    CHECK_EQ(reader.read_u8(), static_cast<std::uint8_t>(1));
    CHECK(reader.ok());
    // Two bytes left, four requested.
    CHECK_EQ(reader.read_u32(), 0u);
    CHECK(!reader.ok());
    // Once failed, the reader stays failed and keeps returning zeroes.
    CHECK_EQ(reader.read_u8(), static_cast<std::uint8_t>(0));
    CHECK(!reader.ok());
    CHECK_EQ(reader.remaining(), static_cast<std::size_t>(0));

    // A string whose declared length runs past the end must fail, not read on.
    const std::vector<std::uint8_t> lying = {200, 'a', 'b'};
    BinaryReader lying_reader(lying);
    CHECK_EQ(lying_reader.read_string8(), std::string());
    CHECK(!lying_reader.ok());

    // Empty input.
    BinaryReader empty(nullptr, 0);
    CHECK_EQ(empty.read_u8(), static_cast<std::uint8_t>(0));
    CHECK(!empty.ok());
}

void test_truncated_messages_are_rejected() {
    // Every prefix of a valid message shorter than the whole must be rejected
    // rather than yielding a partially populated struct.
    ClientPoseMessage pose;
    pose.device_id = "dev";
    pose.body.flags = POSE_FLAG_HEAD_VALID | POSE_FLAG_RIGHT_VALID;
    pose.body.head.position = Vec3(1, 2, 3);
    pose.body.right_hand.position = Vec3(1.1, 2, 3);
    const std::vector<std::uint8_t> full = serialize_client_pose(pose);

    for (std::size_t length = 0; length < full.size(); ++length) {
        ClientPoseMessage decoded;
        CHECK(!deserialize_client_pose(full.data(), length, decoded));
    }
    ClientPoseMessage decoded;
    CHECK(deserialize_client_pose(full.data(), full.size(), decoded));

    RoomPoseMessage room;
    room.room_id = "r";
    room.broadcast_time = 1.0;
    RoomPoseClient client;
    client.client_no = 3;
    client.body.flags = POSE_FLAG_HEAD_VALID;
    room.clients.push_back(client);
    const std::vector<std::uint8_t> room_bytes = serialize_room_pose(room);
    for (std::size_t length = 0; length < room_bytes.size(); ++length) {
        RoomPoseMessage room_decoded;
        CHECK(!deserialize_room_pose(room_bytes.data(), length, room_decoded));
    }
}

void test_wrong_type_byte_is_rejected() {
    std::vector<std::uint8_t> hello = serialize_client_hello("dev", false);
    ClientHelloMessage decoded;
    CHECK(deserialize_client_hello(hello.data(), hello.size(), decoded));

    hello[0] = MSG_RPC;
    CHECK(!deserialize_client_hello(hello.data(), hello.size(), decoded));

    // A wrong protocol version is rejected on every versioned message.
    std::vector<std::uint8_t> pose = serialize_client_pose(ClientPoseMessage());
    pose[1] = 7;
    ClientPoseMessage pose_decoded;
    CHECK(!deserialize_client_pose(pose.data(), pose.size(), pose_decoded));
}

}  // namespace

int main() {
    test_little_endian_integers();
    test_int24_round_trip();
    test_double_round_trip();
    test_strings();
    test_string8_truncation_is_utf8_safe();
    test_code_point_truncation();
    test_reader_bounds_checking();
    test_truncated_messages_are_rejected();
    test_wrong_type_byte_is_rejected();
    return summary("binary reader/writer");
}
