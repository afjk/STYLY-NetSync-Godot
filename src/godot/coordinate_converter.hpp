// SPDX-License-Identifier: Apache-2.0
// Coordinate conversion between Godot space and the STYLY NetSync wire space.
//
// This is the *only* place where the handedness difference is expressed. No
// scene, node or sample may compensate for coordinates on its own.
//
// Deliberately free of Godot headers so the conversion can be unit-tested
// without godot-cpp; src/godot/netsync_bridge.cpp adapts Transform3D/Basis to
// the plain types used here.
//
// Derivation
// ----------
// Godot's basis is {right = +X, up = +Y, backward = +Z}; NetSync (Unity) uses
// {right = +X, up = +Y, forward = +Z}. The two share right and up and differ
// only in the sign of the third axis, so a physical vector's components map
// through the reflection
//
//     M = diag(1, 1, -1),      M = M^-1
//
// and a rotation, being a linear operator, maps by conjugation:
//
//     B_netsync = M * B_godot * M
//
// Because M has determinant -1, this is exactly the handedness flip: the same
// physical rotation is described by a quaternion of the opposite sense in the
// other convention. Everything below follows from those two facts; nothing is
// asserted by inspection. tests/compatibility/test_coordinates.cpp proves the
// quaternion form and the basis form agree.
#pragma once

#include "protocol/pose_codec.hpp"
#include "protocol/protocol_v8.hpp"

namespace styly {
namespace netsync {

/// Row-major 3x3 matrix. `m[row][column]`, so `v' = M * v` reads
/// `v'[r] = sum_c m[r][c] * v[c]`.
struct Mat3 {
    double m[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
};

/// The axis-reflection that relates the two bases: `diag(1, 1, -1)`.
Mat3 reflection_matrix();

/// `M * basis * M`. Self-inverse, so this is both directions of the mapping.
Mat3 conjugate_with_reflection(const Mat3 &basis);

Mat3 matrix_multiply(const Mat3 &a, const Mat3 &b);
Vec3 matrix_transform(const Mat3 &m, const Vec3 &v);

/// Rotation matrix of a quaternion (normalised first).
Mat3 basis_from_quaternion(const Quat &q);

/// Quaternion of a rotation matrix, using Shepperd's largest-diagonal method.
/// The returned quaternion has a non-negative `w`.
Quat quaternion_from_basis(const Mat3 &basis);

namespace coordinates {

/// Position: `(x, y, z) -> (x, y, -z)`. Self-inverse.
Vec3 position_godot_to_netsync(const Vec3 &position);
Vec3 position_netsync_to_godot(const Vec3 &position);

/// Basis: `M * B * M`. Self-inverse.
Mat3 basis_godot_to_netsync(const Mat3 &basis);
Mat3 basis_netsync_to_godot(const Mat3 &basis);

/// Rotation, expressed on the quaternion directly.
///
/// Conjugating by the reflection through the XY plane leaves the rotation angle
/// and the Z component of the rotation generator alone and negates the X and Y
/// components, i.e. `(x, y, z, w) -> (-x, -y, z, w)`. This is the closed form of
/// `quaternion_from_basis(conjugate_with_reflection(basis_from_quaternion(q)))`,
/// without the matrix round trip's precision loss; the test suite asserts the
/// two agree.
Quat rotation_godot_to_netsync(const Quat &rotation);
Quat rotation_netsync_to_godot(const Quat &rotation);

/// Yaw about +Y in degrees. The reflection reverses the sense of a rotation
/// about the up axis, so the sign flips.
double yaw_degrees_godot_to_netsync(double yaw_degrees);
double yaw_degrees_netsync_to_godot(double yaw_degrees);

/// Whole pose (position + rotation).
PoseTransform transform_godot_to_netsync(const PoseTransform &pose);
PoseTransform transform_netsync_to_godot(const PoseTransform &pose);

}  // namespace coordinates
}  // namespace netsync
}  // namespace styly
