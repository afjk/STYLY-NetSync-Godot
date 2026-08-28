// SPDX-License-Identifier: Apache-2.0
//
// Coordinate conversion between Godot space and the NetSync wire space.
//
// The central claim is that the quaternion shortcut
// `(x, y, z, w) -> (-x, -y, z, w)` really is the conjugation
// `M * B * M` with `M = diag(1, 1, -1)` — that is what makes it a derivation
// rather than a guessed sign flip. That equivalence is asserted directly, over
// random rotations, and the named axis/angle cases the brief calls for are
// checked against independently reasoned expected values.

#include <algorithm>
#include <cmath>
#include <random>
#include <string>

#include "../test_support.hpp"
#include "godot/coordinate_converter.hpp"

using namespace styly::netsync;
using namespace styly::netsync::coordinates;
using namespace styly::netsync::test;

namespace {

constexpr double kEpsilon = 1e-12;

/// Rotation about an arbitrary unit axis, right-handed, in Godot's convention.
Quat axis_angle(double ax, double ay, double az, double degrees) {
    const double length = std::sqrt(ax * ax + ay * ay + az * az);
    if (length <= 0.0) {
        return Quat(0, 0, 0, 1);
    }
    const double half = degrees * (kPi / 180.0) * 0.5;
    const double s = std::sin(half) / length;
    return Quat(ax * s, ay * s, az * s, std::cos(half));
}

/// Angular distance between two rotations, in degrees. Sign-insensitive: q and
/// -q are the same rotation.
///
/// Uses `2*atan2(|v|, |w|)` on the relative quaternion rather than `2*acos(dot)`.
/// Near zero, acos(1-e) grows like sqrt(e), so double-precision noise in the
/// dot product would show up as ~1e-6 degrees of phantom error and swamp the
/// tolerances these tests are trying to assert. The atan2 form is well
/// conditioned there.
double rotation_error_degrees(const Quat &a, const Quat &b) {
    const Quat na = normalize_quaternion(a);
    const Quat nb = normalize_quaternion(b);
    const Quat relative = quaternion_multiply(quaternion_inverse(na), nb);
    const double vector_length = std::sqrt(relative.x * relative.x + relative.y * relative.y +
                                           relative.z * relative.z);
    return 2.0 * std::atan2(vector_length, std::fabs(relative.w)) * (180.0 / kPi);
}

void check_vec3(const Vec3 &actual, const Vec3 &expected, const char *what) {
    CHECK_MSG(std::fabs(actual.x - expected.x) <= kEpsilon, what);
    CHECK_MSG(std::fabs(actual.y - expected.y) <= kEpsilon, what);
    CHECK_MSG(std::fabs(actual.z - expected.z) <= kEpsilon, what);
}

void test_reflection_is_its_own_inverse() {
    const Mat3 reflection = reflection_matrix();
    const Mat3 squared = matrix_multiply(reflection, reflection);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            CHECK_NEAR(squared.m[r][c], r == c ? 1.0 : 0.0, kEpsilon);
        }
    }
    // Determinant -1: this is the handedness flip, not a rotation.
    const double determinant =
        reflection.m[0][0] * reflection.m[1][1] * reflection.m[2][2];
    CHECK_NEAR(determinant, -1.0, kEpsilon);
}

void test_named_positions() {
    // Identity.
    check_vec3(position_godot_to_netsync(Vec3(0, 0, 0)), Vec3(0, 0, 0), "origin");

    // +X (right) is shared by both conventions.
    check_vec3(position_godot_to_netsync(Vec3(1, 0, 0)), Vec3(1, 0, 0), "+X translation");

    // +Y (up) is shared.
    check_vec3(position_godot_to_netsync(Vec3(0, 1, 0)), Vec3(0, 1, 0), "+Y translation");

    // Godot's forward is -Z; on the wire (Unity) forward is +Z.
    check_vec3(position_godot_to_netsync(Vec3(0, 0, -1)), Vec3(0, 0, 1), "forward translation");
    check_vec3(position_netsync_to_godot(Vec3(0, 0, 1)), Vec3(0, 0, -1), "forward translation back");

    // A general point.
    check_vec3(position_godot_to_netsync(Vec3(1.5, -2.25, 3.75)), Vec3(1.5, -2.25, -3.75),
               "general point");
}

void test_position_round_trip() {
    std::mt19937 engine(7);
    std::uniform_real_distribution<double> component(-1000.0, 1000.0);
    for (int i = 0; i < 10000; ++i) {
        const Vec3 original(component(engine), component(engine), component(engine));
        const Vec3 restored = position_netsync_to_godot(position_godot_to_netsync(original));
        // The mapping is exact (sign flip only), so this must be bit-exact.
        CHECK_EQ(restored.x, original.x);
        CHECK_EQ(restored.y, original.y);
        CHECK_EQ(restored.z, original.z);
    }
}

/// The core claim: the quaternion form equals the basis conjugation.
void test_quaternion_form_equals_basis_conjugation() {
    std::mt19937 engine(20240816);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    double worst = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const double u1 = unit(engine);
        const double u2 = unit(engine);
        const double u3 = unit(engine);
        const double s1 = std::sqrt(1.0 - u1);
        const double s2 = std::sqrt(u1);
        const double t1 = 2.0 * kPi * u2;
        const double t2 = 2.0 * kPi * u3;
        const Quat godot(s1 * std::sin(t1), s1 * std::cos(t1), s2 * std::sin(t2),
                         s2 * std::cos(t2));

        // Path A: the closed form used at runtime.
        const Quat via_quaternion = rotation_godot_to_netsync(godot);

        // Path B: the definition — conjugate the rotation matrix, then recover
        // the quaternion.
        const Mat3 conjugated = basis_godot_to_netsync(basis_from_quaternion(godot));
        const Quat via_basis = quaternion_from_basis(conjugated);

        const double error = rotation_error_degrees(via_quaternion, via_basis);
        if (error > worst) {
            worst = error;
        }
    }
    CHECK_MSG(worst < 1e-6, "quaternion form diverges from basis conjugation by " +
                                std::to_string(worst) + " degrees");

    // And the matrices themselves agree, not just the rotations they encode.
    const Quat sample = axis_angle(0.3, -0.5, 0.81, 47.0);
    const Mat3 from_converted_quaternion = basis_from_quaternion(rotation_godot_to_netsync(sample));
    const Mat3 from_conjugation = basis_godot_to_netsync(basis_from_quaternion(sample));
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            CHECK_NEAR(from_converted_quaternion.m[r][c], from_conjugation.m[r][c], 1e-12);
        }
    }
}

void test_named_rotations() {
    // Yaw ±90 about +Y. Godot is right-handed, the wire convention is
    // left-handed, so the same physical turn is the opposite signed yaw there.
    {
        const Quat godot_yaw = axis_angle(0, 1, 0, 90.0);
        const Quat wire = rotation_godot_to_netsync(godot_yaw);
        CHECK_NEAR(rotation_error_degrees(wire, axis_angle(0, 1, 0, -90.0)), 0.0, 1e-9);
        // quaternion_to_yaw_degrees reads the wire convention.
        CHECK_NEAR(quaternion_to_yaw_degrees(wire), -90.0, 1e-9);
    }
    {
        const Quat godot_yaw = axis_angle(0, 1, 0, -90.0);
        const Quat wire = rotation_godot_to_netsync(godot_yaw);
        CHECK_NEAR(quaternion_to_yaw_degrees(wire), 90.0, 1e-9);
    }

    // Pitch ±90 about +X: also reversed, X being in the reflection plane.
    {
        const Quat wire = rotation_godot_to_netsync(axis_angle(1, 0, 0, 90.0));
        CHECK_NEAR(rotation_error_degrees(wire, axis_angle(1, 0, 0, -90.0)), 0.0, 1e-9);
    }
    {
        const Quat wire = rotation_godot_to_netsync(axis_angle(1, 0, 0, -90.0));
        CHECK_NEAR(rotation_error_degrees(wire, axis_angle(1, 0, 0, 90.0)), 0.0, 1e-9);
    }

    // Roll ±90 about Z: the axis itself flips as well as the sense, so the
    // signed angle about +Z is preserved.
    {
        const Quat wire = rotation_godot_to_netsync(axis_angle(0, 0, 1, 90.0));
        CHECK_NEAR(rotation_error_degrees(wire, axis_angle(0, 0, 1, 90.0)), 0.0, 1e-9);
    }
    {
        const Quat wire = rotation_godot_to_netsync(axis_angle(0, 0, 1, -90.0));
        CHECK_NEAR(rotation_error_degrees(wire, axis_angle(0, 0, 1, -90.0)), 0.0, 1e-9);
    }

    // Identity maps to identity.
    {
        const Quat wire = rotation_godot_to_netsync(Quat(0, 0, 0, 1));
        CHECK_NEAR(rotation_error_degrees(wire, Quat(0, 0, 0, 1)), 0.0, 1e-12);
    }

    // An arbitrary rotation.
    {
        const Quat godot = axis_angle(0.37, 0.82, -0.44, 123.4);
        const Quat wire = rotation_godot_to_netsync(godot);
        const Quat expected =
            quaternion_from_basis(conjugate_with_reflection(basis_from_quaternion(godot)));
        CHECK_NEAR(rotation_error_degrees(wire, expected), 0.0, 1e-9);
    }
}

void test_rotation_round_trip() {
    std::mt19937 engine(31337);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    double worst = 0.0;
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

        const Quat restored = rotation_netsync_to_godot(rotation_godot_to_netsync(original));
        // Two sign flips of the same components: exactly the original.
        CHECK_EQ(restored.x, original.x);
        CHECK_EQ(restored.y, original.y);
        CHECK_EQ(restored.z, original.z);
        CHECK_EQ(restored.w, original.w);

        const double error = rotation_error_degrees(restored, original);
        if (error > worst) {
            worst = error;
        }
    }
    CHECK_MSG(worst < 1e-9, "round-trip rotation error " + std::to_string(worst) + " degrees");
}

void test_basis_round_trip() {
    std::mt19937 engine(555);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int i = 0; i < 2000; ++i) {
        const double u1 = unit(engine);
        const double u2 = unit(engine);
        const double u3 = unit(engine);
        const double s1 = std::sqrt(1.0 - u1);
        const double s2 = std::sqrt(u1);
        const double t1 = 2.0 * kPi * u2;
        const double t2 = 2.0 * kPi * u3;
        const Mat3 original = basis_from_quaternion(Quat(
            s1 * std::sin(t1), s1 * std::cos(t1), s2 * std::sin(t2), s2 * std::cos(t2)));
        const Mat3 restored = basis_netsync_to_godot(basis_godot_to_netsync(original));
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                CHECK_NEAR(restored.m[r][c], original.m[r][c], 1e-14);
            }
        }
    }
}

void test_yaw_conversion() {
    // Consistency with the rotation conversion: converting a yaw-only rotation
    // and reading its yaw back must equal the scalar helper.
    for (double yaw = -175.0; yaw < 180.0; yaw += 5.0) {
        const Quat wire = rotation_godot_to_netsync(axis_angle(0, 1, 0, yaw));
        CHECK_NEAR(quaternion_to_yaw_degrees(wire), yaw_degrees_godot_to_netsync(yaw), 1e-9);
    }
    CHECK_EQ(yaw_degrees_netsync_to_godot(yaw_degrees_godot_to_netsync(37.5)), 37.5);
}

void test_transform_conversion() {
    PoseTransform godot;
    godot.position = Vec3(1.0, 2.0, 3.0);
    godot.rotation = axis_angle(0, 1, 0, 45.0);

    const PoseTransform wire = transform_godot_to_netsync(godot);
    CHECK_NEAR(wire.position.z, -3.0, kEpsilon);
    CHECK_NEAR(quaternion_to_yaw_degrees(wire.rotation), -45.0, 1e-9);

    const PoseTransform back = transform_netsync_to_godot(wire);
    CHECK_EQ(back.position.x, godot.position.x);
    CHECK_EQ(back.position.z, godot.position.z);
    CHECK_NEAR(rotation_error_degrees(back.rotation, godot.rotation), 0.0, 1e-12);
}

/// A "looking at" sanity check: a Godot node facing its own forward (-Z) must
/// come out on the wire facing Unity's forward (+Z).
void test_forward_direction_is_preserved() {
    // A Godot rotation of yaw θ maps the local -Z axis into world space.
    for (double yaw = -180.0; yaw < 180.0; yaw += 15.0) {
        const Quat godot_rotation = axis_angle(0, 1, 0, yaw);
        const Mat3 godot_basis = basis_from_quaternion(godot_rotation);
        // Godot forward is the third basis column negated.
        const Vec3 godot_forward(-godot_basis.m[0][2], -godot_basis.m[1][2], -godot_basis.m[2][2]);

        const Quat wire_rotation = rotation_godot_to_netsync(godot_rotation);
        const Mat3 wire_basis = basis_from_quaternion(wire_rotation);
        // Wire (Unity) forward is the third basis column as-is.
        const Vec3 wire_forward(wire_basis.m[0][2], wire_basis.m[1][2], wire_basis.m[2][2]);

        // The same physical direction, expressed in the two conventions.
        const Vec3 expected = position_godot_to_netsync(godot_forward);
        check_vec3(wire_forward, expected, "forward direction");
    }
}

/// End-to-end: Godot pose -> wire -> quantised -> wire -> Godot must land close
/// to the original, with the error bounded by the protocol's quantisation.
void test_end_to_end_through_the_wire_codec() {
    std::mt19937 engine(24680);
    std::uniform_real_distribution<double> position(-50.0, 50.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    double worst_position = 0.0;
    double worst_rotation = 0.0;
    for (int i = 0; i < 2000; ++i) {
        PoseTransform godot;
        godot.position = Vec3(position(engine), position(engine), position(engine));
        const double u1 = unit(engine);
        const double u2 = unit(engine);
        const double u3 = unit(engine);
        const double s1 = std::sqrt(1.0 - u1);
        const double s2 = std::sqrt(u1);
        godot.rotation = Quat(s1 * std::sin(2.0 * kPi * u2), s1 * std::cos(2.0 * kPi * u2),
                              s2 * std::sin(2.0 * kPi * u3), s2 * std::cos(2.0 * kPi * u3));

        // To the wire and through the object-pose codec.
        const PoseTransform wire = transform_godot_to_netsync(godot);
        ObjectPoseMessage message;
        message.device_id = "coordinate-test";
        message.object_id = 1;
        message.position = wire.position;
        message.rotation = wire.rotation;
        const std::vector<std::uint8_t> bytes = serialize_object_pose(message);

        ObjectPoseMessage decoded;
        CHECK(deserialize_object_pose(bytes.data(), bytes.size(), decoded));

        PoseTransform decoded_wire;
        decoded_wire.position = decoded.position;
        decoded_wire.rotation = decoded.rotation;
        const PoseTransform back = transform_netsync_to_godot(decoded_wire);

        const double position_error =
            std::max(std::fabs(back.position.x - godot.position.x),
                     std::max(std::fabs(back.position.y - godot.position.y),
                              std::fabs(back.position.z - godot.position.z)));
        worst_position = std::max(worst_position, position_error);
        worst_rotation = std::max(worst_rotation, rotation_error_degrees(back.rotation, godot.rotation));
    }

    // Half a 0.01 m quantisation step, and the codec's rotation resolution.
    CHECK_MSG(worst_position <= kAbsPosScale * 0.5 + 1e-9,
              "worst position error " + std::to_string(worst_position) + " m");
    CHECK_MSG(worst_rotation < 0.25,
              "worst rotation error " + std::to_string(worst_rotation) + " degrees");
}

}  // namespace

int main() {
    test_reflection_is_its_own_inverse();
    test_named_positions();
    test_position_round_trip();
    test_quaternion_form_equals_basis_conjugation();
    test_named_rotations();
    test_rotation_round_trip();
    test_basis_round_trip();
    test_yaw_conversion();
    test_transform_conversion();
    test_forward_direction_is_preserved();
    test_end_to_end_through_the_wire_codec();
    return summary("coordinate conversion");
}
