// SPDX-License-Identifier: Apache-2.0
// Pose codec properties that the golden vectors alone do not pin down:
// rounding mode, clamping, the quaternion codec's accuracy and canonical form,
// and the flag-derivation rules.

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "../test_support.hpp"
#include "protocol/pose_codec.hpp"
#include "protocol/protocol_v8.hpp"

using namespace styly::netsync;
using namespace styly::netsync::test;

namespace {

void test_round_half_to_even() {
    // The distinguishing cases: exact halves round to the even neighbour, not
    // away from zero. std::round would fail every one of these.
    CHECK_EQ(round_half_to_even(0.5), 0.0);
    CHECK_EQ(round_half_to_even(1.5), 2.0);
    CHECK_EQ(round_half_to_even(2.5), 2.0);
    CHECK_EQ(round_half_to_even(3.5), 4.0);
    CHECK_EQ(round_half_to_even(-0.5), 0.0);
    CHECK_EQ(round_half_to_even(-1.5), -2.0);
    CHECK_EQ(round_half_to_even(-2.5), -2.0);
    CHECK_EQ(round_half_to_even(-3.5), -4.0);
    // Ordinary values are unaffected.
    CHECK_EQ(round_half_to_even(0.4), 0.0);
    CHECK_EQ(round_half_to_even(0.6), 1.0);
    CHECK_EQ(round_half_to_even(-0.6), -1.0);
    CHECK_EQ(round_half_to_even(1e17), 1e17);
}

void test_quantization_clamping() {
    CHECK_EQ(quantize_signed(1e9, kAbsPosScale), static_cast<std::int16_t>(kInt16Max));
    CHECK_EQ(quantize_signed(-1e9, kAbsPosScale), static_cast<std::int16_t>(kInt16Min));
    CHECK_EQ(quantize_signed_int24(1e9, kAbsPosScale), kInt24Max);
    CHECK_EQ(quantize_signed_int24(-1e9, kAbsPosScale), kInt24Min);

    // A non-positive scale yields zero rather than dividing by zero.
    CHECK_EQ(quantize_signed(1.0, 0.0), static_cast<std::int16_t>(0));
    CHECK_EQ(quantize_signed_int24(1.0, -1.0), 0);

    // Non-finite input saturates instead of producing an undefined cast.
    const double infinity = std::numeric_limits<double>::infinity();
    CHECK_EQ(quantize_signed(infinity, kAbsPosScale), static_cast<std::int16_t>(kInt16Max));
    CHECK_EQ(quantize_signed(-infinity, kAbsPosScale), static_cast<std::int16_t>(kInt16Min));
    CHECK_EQ(quantize_signed(std::nan(""), kAbsPosScale), static_cast<std::int16_t>(0));
    CHECK_EQ(quantize_signed_int24(std::nan(""), kAbsPosScale), 0);
}

void test_quantization_round_trip_accuracy() {
    std::mt19937 engine(4242);
    std::uniform_real_distribution<double> position(-80000.0, 80000.0);
    for (int i = 0; i < 5000; ++i) {
        const double value = position(engine);
        const double restored = dequantize_signed(quantize_signed_int24(value, kAbsPosScale),
                                                  kAbsPosScale);
        // Half a quantisation step, plus room for the scaling multiply.
        CHECK_NEAR(restored, value, kAbsPosScale * 0.5 + 1e-9);
    }

    std::uniform_real_distribution<double> relative(-160.0, 160.0);
    for (int i = 0; i < 5000; ++i) {
        const double value = relative(engine);
        const double restored =
            dequantize_signed(quantize_signed(value, kRelPosScale), kRelPosScale);
        CHECK_NEAR(restored, value, kRelPosScale * 0.5 + 1e-9);
    }
}

void test_quaternion_normalization() {
    // Degenerate inputs collapse to identity rather than producing NaNs.
    const Quat zero = normalize_quaternion(Quat(0, 0, 0, 0));
    CHECK_EQ(zero.w, 1.0);
    CHECK_EQ(zero.x, 0.0);

    const Quat tiny = normalize_quaternion(Quat(1e-9, 0, 0, 0));
    CHECK_EQ(tiny.w, 1.0);

    const Quat nan_quat = normalize_quaternion(Quat(std::nan(""), 0, 0, 1));
    CHECK_EQ(nan_quat.w, 1.0);

    // A scaled quaternion normalises to unit length.
    const Quat scaled = normalize_quaternion(Quat(2, 0, 0, 2));
    CHECK_NEAR(scaled.x * scaled.x + scaled.y * scaled.y + scaled.z * scaled.z +
                   scaled.w * scaled.w,
               1.0, 1e-12);
}

void test_quaternion_codec_accuracy() {
    // Uniformly random rotations (Shoemake). The smallest-three codec must keep
    // every one within the accuracy its 10-bit components allow.
    std::mt19937 engine(1337);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    double worst_angle_degrees = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const double u1 = unit(engine);
        const double u2 = unit(engine);
        const double u3 = unit(engine);
        const double s1 = std::sqrt(1.0 - u1);
        const double s2 = std::sqrt(u1);
        const double t1 = 2.0 * kPi * u2;
        const double t2 = 2.0 * kPi * u3;
        const Quat original(s1 * std::sin(t1), s1 * std::cos(t1), s2 * std::sin(t2),
                            s2 * std::cos(t2));

        const Quat restored =
            decompress_quaternion_smallest_three(compress_quaternion_smallest_three(original));

        // |dot| is 1 for identical rotations; q and -q are the same rotation.
        double dot = original.x * restored.x + original.y * restored.y +
                     original.z * restored.z + original.w * restored.w;
        dot = std::fabs(dot);
        if (dot > 1.0) {
            dot = 1.0;
        }
        const double angle_degrees = 2.0 * std::acos(dot) * (180.0 / kPi);
        if (angle_degrees > worst_angle_degrees) {
            worst_angle_degrees = angle_degrees;
        }
    }
    // 10 bits over [-1/√2, 1/√2] gives a step of ~0.00138 per component; the
    // resulting worst-case rotation error is well under a quarter degree.
    CHECK_MSG(worst_angle_degrees < 0.25,
              "worst quaternion error " + std::to_string(worst_angle_degrees) + " degrees");
}

void test_quaternion_sign_canonicalisation() {
    // q and -q are the same rotation and must pack identically.
    std::mt19937 engine(99);
    std::uniform_real_distribution<double> component(-1.0, 1.0);
    for (int i = 0; i < 2000; ++i) {
        const Quat q(component(engine), component(engine), component(engine), component(engine));
        if (q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w < 1e-6) {
            continue;
        }
        const Quat negated(-q.x, -q.y, -q.z, -q.w);
        CHECK_EQ(compress_quaternion_smallest_three(q),
                 compress_quaternion_smallest_three(negated));
    }
}

void test_quaternion_largest_index_layout() {
    // Identity: w is largest, so the index field must be 3 and the three stored
    // components must all decode to ~0.
    const std::uint32_t identity = compress_quaternion_smallest_three(Quat(0, 0, 0, 1));
    CHECK_EQ((identity >> 30) & 0x3u, 3u);

    const std::uint32_t about_x = compress_quaternion_smallest_three(Quat(1, 0, 0, 0));
    CHECK_EQ((about_x >> 30) & 0x3u, 0u);
    const std::uint32_t about_y = compress_quaternion_smallest_three(Quat(0, 1, 0, 0));
    CHECK_EQ((about_y >> 30) & 0x3u, 1u);
    const std::uint32_t about_z = compress_quaternion_smallest_three(Quat(0, 0, 1, 0));
    CHECK_EQ((about_z >> 30) & 0x3u, 2u);

    // Ties resolve to the lowest index, matching Python's max() and Unity's
    // strict-greater scan.
    const std::uint32_t tie = compress_quaternion_smallest_three(Quat(0.5, 0.5, 0.5, 0.5));
    CHECK_EQ((tie >> 30) & 0x3u, 0u);
}

void test_yaw_helpers() {
    CHECK_NEAR(normalize_yaw_degrees(0.0), 0.0, 1e-12);
    CHECK_NEAR(normalize_yaw_degrees(180.0), -180.0, 1e-12);
    CHECK_NEAR(normalize_yaw_degrees(-180.0), -180.0, 1e-12);
    CHECK_NEAR(normalize_yaw_degrees(190.0), -170.0, 1e-12);
    CHECK_NEAR(normalize_yaw_degrees(-190.0), 170.0, 1e-12);
    CHECK_NEAR(normalize_yaw_degrees(720.0 + 45.0), 45.0, 1e-12);

    for (double yaw = -179.0; yaw < 180.0; yaw += 7.0) {
        const Quat q = yaw_degrees_to_quaternion(yaw);
        CHECK_NEAR(quaternion_to_yaw_degrees(q), yaw, 1e-9);
    }

    // rotate_yaw_vector must agree with the yaw quaternion it corresponds to.
    const Vec3 v(1.0, 2.0, 3.0);
    const Vec3 rotated = rotate_yaw_vector(v, 90.0);
    CHECK_NEAR(rotated.x, 3.0, 1e-9);
    CHECK_NEAR(rotated.y, 2.0, 1e-9);
    CHECK_NEAR(rotated.z, -1.0, 1e-9);
}

void test_pose_flag_sanitisation() {
    // Stealth collapses to stealth alone.
    CHECK_EQ(sanitize_pose_flags(POSE_FLAG_STEALTH | POSE_FLAG_HEAD_VALID |
                                 POSE_FLAG_PHYSICAL_VALID),
             static_cast<std::uint8_t>(POSE_FLAG_STEALTH));

    // Head-relative bits require the head.
    CHECK_EQ(sanitize_pose_flags(POSE_FLAG_RIGHT_VALID | POSE_FLAG_LEFT_VALID |
                                 POSE_FLAG_VIRTUALS_VALID | POSE_FLAG_PHYSICAL_VALID),
             static_cast<std::uint8_t>(POSE_FLAG_PHYSICAL_VALID));

    // With a head they survive untouched.
    const std::uint8_t full = POSE_FLAG_HEAD_VALID | POSE_FLAG_RIGHT_VALID |
                              POSE_FLAG_LEFT_VALID | POSE_FLAG_VIRTUALS_VALID |
                              POSE_FLAG_PHYSICAL_VALID;
    CHECK_EQ(sanitize_pose_flags(full), full);
}

void test_encoding_flag_derivation() {
    CHECK_EQ(compute_encoding_flags(POSE_FLAG_HEAD_VALID),
             static_cast<std::uint8_t>(ENCODING_FLAGS_DEFAULT));
    CHECK_EQ(compute_encoding_flags(POSE_FLAG_HEAD_VALID | POSE_FLAG_MOVING_FLOOR_LOCAL),
             static_cast<std::uint8_t>(ENCODING_FLAGS_DEFAULT &
                                       ~ENCODING_PHYSICAL_IS_XRORIGIN_DELTA));
}

void test_pose_body_round_trip() {
    ClientPoseMessage message;
    message.device_id = "round-trip";
    message.body.pose_seq = 4242;
    message.body.flags = POSE_FLAG_HEAD_VALID | POSE_FLAG_RIGHT_VALID | POSE_FLAG_LEFT_VALID |
                         POSE_FLAG_VIRTUALS_VALID | POSE_FLAG_PHYSICAL_VALID;
    message.body.xr_origin_delta_position = Vec3(1.5, 0.0, -2.5);
    message.body.xr_origin_delta_yaw = 45.0;
    message.body.head.position = Vec3(1.0, 1.6, -2.0);
    message.body.head.rotation = yaw_degrees_to_quaternion(30.0);
    message.body.right_hand.position = Vec3(1.3, 1.2, -2.1);
    message.body.right_hand.rotation = yaw_degrees_to_quaternion(10.0);
    message.body.left_hand.position = Vec3(0.7, 1.2, -2.1);
    message.body.left_hand.rotation = yaw_degrees_to_quaternion(-10.0);
    PoseTransform virtual_transform;
    virtual_transform.position = Vec3(2.0, 0.5, -3.0);
    virtual_transform.rotation = yaw_degrees_to_quaternion(90.0);
    message.body.virtuals.push_back(virtual_transform);

    const std::vector<std::uint8_t> bytes = serialize_client_pose(message);
    ClientPoseMessage decoded;
    CHECK(deserialize_client_pose(bytes.data(), bytes.size(), decoded));
    CHECK_EQ(decoded.device_id, message.device_id);
    CHECK_EQ(decoded.body.pose_seq, message.body.pose_seq);
    CHECK_EQ(decoded.body.flags, message.body.flags);

    CHECK_NEAR(decoded.body.head.position.x, message.body.head.position.x, kAbsPosScale);
    CHECK_NEAR(decoded.body.head.position.y, message.body.head.position.y, kAbsPosScale);
    CHECK_NEAR(decoded.body.head.position.z, message.body.head.position.z, kAbsPosScale);

    // Hands are reconstructed as head + relative delta, so their error is the
    // sum of the two quantisation steps.
    const double hand_tolerance = kAbsPosScale + kRelPosScale;
    CHECK_NEAR(decoded.body.right_hand.position.x, message.body.right_hand.position.x,
               hand_tolerance);
    CHECK_NEAR(decoded.body.left_hand.position.z, message.body.left_hand.position.z,
               hand_tolerance);

    CHECK_EQ(decoded.body.virtuals.size(), static_cast<std::size_t>(1));
    CHECK_NEAR(decoded.body.virtuals[0].position.y, virtual_transform.position.y, hand_tolerance);

    CHECK_NEAR(decoded.body.xr_origin_delta_yaw, message.body.xr_origin_delta_yaw,
               kPhysicalYawScale);
    CHECK_NEAR(decoded.body.xr_origin_delta_position.x,
               message.body.xr_origin_delta_position.x, kLocoPosScale);
}

void test_virtual_transform_cap() {
    ClientPoseMessage message;
    message.device_id = "cap";
    message.body.flags = POSE_FLAG_HEAD_VALID | POSE_FLAG_VIRTUALS_VALID;
    for (int i = 0; i < 70; ++i) {
        PoseTransform entry;
        entry.position = Vec3(i * 0.1, 0.0, 0.0);
        message.body.virtuals.push_back(entry);
    }
    const std::vector<std::uint8_t> bytes = serialize_client_pose(message);
    ClientPoseMessage decoded;
    CHECK(deserialize_client_pose(bytes.data(), bytes.size(), decoded));
    CHECK_EQ(decoded.body.virtuals.size(), static_cast<std::size_t>(kMaxVirtualTransforms));
}

void test_physical_reconstruction() {
    // With no XR-origin locomotion the physical pose equals the head pose.
    ClientPoseMessage message;
    message.device_id = "phys";
    message.body.flags = POSE_FLAG_HEAD_VALID | POSE_FLAG_PHYSICAL_VALID;
    message.body.head.position = Vec3(2.0, 1.6, -3.0);
    message.body.head.rotation = yaw_degrees_to_quaternion(25.0);

    const std::vector<std::uint8_t> bytes = serialize_client_pose(message);
    ClientPoseMessage decoded;
    CHECK(deserialize_client_pose(bytes.data(), bytes.size(), decoded));
    CHECK_NEAR(decoded.body.physical.position.x, 2.0, kAbsPosScale);
    CHECK_NEAR(decoded.body.physical.position.z, -3.0, kAbsPosScale);
    CHECK_NEAR(quaternion_to_yaw_degrees(decoded.body.physical.rotation), 25.0, 0.5);

    // With locomotion applied, the physical pose is the head pose mapped back
    // through the XR-origin delta.
    ClientPoseMessage moved = message;
    moved.body.xr_origin_delta_position = Vec3(5.0, 0.0, 0.0);
    moved.body.xr_origin_delta_yaw = 90.0;
    const std::vector<std::uint8_t> moved_bytes = serialize_client_pose(moved);
    ClientPoseMessage moved_decoded;
    CHECK(deserialize_client_pose(moved_bytes.data(), moved_bytes.size(), moved_decoded));
    // translated = (2 - 5, 1.6, -3) = (-3, 1.6, -3); rotate by -90 degrees.
    const Vec3 expected = rotate_yaw_vector(Vec3(-3.0, 1.6, -3.0), -90.0);
    CHECK_NEAR(moved_decoded.body.physical.position.x, expected.x, 0.05);
    CHECK_NEAR(moved_decoded.body.physical.position.z, expected.z, 0.05);
    CHECK_NEAR(quaternion_to_yaw_degrees(moved_decoded.body.physical.rotation), 25.0 - 90.0, 0.5);
}

void test_moving_floor_local_encoding() {
    ClientPoseMessage message;
    message.device_id = "mf";
    message.body.flags =
        POSE_FLAG_HEAD_VALID | POSE_FLAG_PHYSICAL_VALID | POSE_FLAG_MOVING_FLOOR_LOCAL;
    message.body.physical.position = Vec3(1.5, 0.0, -2.5);
    message.body.physical.rotation = yaw_degrees_to_quaternion(60.0);
    message.body.head.position = Vec3(0.2, 1.6, 0.3);

    const std::vector<std::uint8_t> bytes = serialize_client_pose(message);
    // The encoding byte must drop the XR-origin-delta bit.
    CHECK_EQ(bytes[2 + 1 + message.device_id.size() + 2 + 1],
             static_cast<std::uint8_t>(ENCODING_FLAGS_DEFAULT &
                                       ~ENCODING_PHYSICAL_IS_XRORIGIN_DELTA));

    ClientPoseMessage decoded;
    CHECK(deserialize_client_pose(bytes.data(), bytes.size(), decoded));
    CHECK_NEAR(decoded.body.physical.position.x, 1.5, kLocoPosScale);
    CHECK_NEAR(decoded.body.physical.position.z, -2.5, kLocoPosScale);
    CHECK_NEAR(quaternion_to_yaw_degrees(decoded.body.physical.rotation), 60.0,
               kPhysicalYawScale);
    // The XR-origin delta is not carried in this mode.
    CHECK_EQ(decoded.body.xr_origin_delta_yaw, 0.0);
}

void test_missing_xrorigin_encoding_flag_is_rejected() {
    ClientPoseMessage message;
    message.device_id = "x";
    message.body.flags = POSE_FLAG_HEAD_VALID | POSE_FLAG_PHYSICAL_VALID;
    std::vector<std::uint8_t> bytes = serialize_client_pose(message);
    const std::size_t encoding_index = 2 + 1 + message.device_id.size() + 2 + 1;
    bytes[encoding_index] =
        static_cast<std::uint8_t>(bytes[encoding_index] & ~ENCODING_PHYSICAL_IS_XRORIGIN_DELTA);

    ClientPoseMessage decoded;
    CHECK(!deserialize_client_pose(bytes.data(), bytes.size(), decoded));
}

void test_stealth_handshake_matches_stealth_pose() {
    // The dedicated stealth handshake and a stealth-flagged pose body produce
    // the same bytes, which is what lets a stealth client use either path.
    ClientPoseMessage message;
    message.device_id = "stealth-dev";
    message.body.flags = POSE_FLAG_STEALTH;
    CHECK_BYTES(serialize_stealth_handshake_pose("stealth-dev"), serialize_client_pose(message));
}

}  // namespace

int main() {
    test_round_half_to_even();
    test_quantization_clamping();
    test_quantization_round_trip_accuracy();
    test_quaternion_normalization();
    test_quaternion_codec_accuracy();
    test_quaternion_sign_canonicalisation();
    test_quaternion_largest_index_layout();
    test_yaw_helpers();
    test_pose_flag_sanitisation();
    test_encoding_flag_derivation();
    test_pose_body_round_trip();
    test_virtual_transform_cap();
    test_physical_reconstruction();
    test_moving_floor_local_encoding();
    test_missing_xrorigin_encoding_flag_is_rejected();
    test_stealth_handshake_matches_stealth_pose();
    return summary("pose codec");
}
