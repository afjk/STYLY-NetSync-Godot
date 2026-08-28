// SPDX-License-Identifier: Apache-2.0
#include "coordinate_converter.hpp"

#include <cmath>

namespace styly {
namespace netsync {

namespace {
/// Diagonal of the reflection relating the two bases. Right and up are shared;
/// the third axis points the opposite way.
constexpr double kReflectionDiagonal[3] = {1.0, 1.0, -1.0};
}  // namespace

Mat3 reflection_matrix() {
    Mat3 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out.m[r][c] = (r == c) ? kReflectionDiagonal[r] : 0.0;
        }
    }
    return out;
}

Mat3 matrix_multiply(const Mat3 &a, const Mat3 &b) {
    Mat3 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += a.m[r][k] * b.m[k][c];
            }
            out.m[r][c] = sum;
        }
    }
    return out;
}

Vec3 matrix_transform(const Mat3 &m, const Vec3 &v) {
    return Vec3(m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
                m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
                m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z);
}

Mat3 conjugate_with_reflection(const Mat3 &basis) {
    // Because the reflection is diagonal, M * B * M scales entry (r, c) by
    // d[r] * d[c]. The general product is written out anyway so the identity is
    // visible rather than folded into sign bookkeeping.
    const Mat3 reflection = reflection_matrix();
    return matrix_multiply(matrix_multiply(reflection, basis), reflection);
}

Mat3 basis_from_quaternion(const Quat &q) {
    const Quat n = normalize_quaternion(q);
    const double xx = n.x * n.x;
    const double yy = n.y * n.y;
    const double zz = n.z * n.z;
    const double xy = n.x * n.y;
    const double xz = n.x * n.z;
    const double yz = n.y * n.z;
    const double wx = n.w * n.x;
    const double wy = n.w * n.y;
    const double wz = n.w * n.z;

    Mat3 out;
    out.m[0][0] = 1.0 - 2.0 * (yy + zz);
    out.m[0][1] = 2.0 * (xy - wz);
    out.m[0][2] = 2.0 * (xz + wy);
    out.m[1][0] = 2.0 * (xy + wz);
    out.m[1][1] = 1.0 - 2.0 * (xx + zz);
    out.m[1][2] = 2.0 * (yz - wx);
    out.m[2][0] = 2.0 * (xz - wy);
    out.m[2][1] = 2.0 * (yz + wx);
    out.m[2][2] = 1.0 - 2.0 * (xx + yy);
    return out;
}

Quat quaternion_from_basis(const Mat3 &basis) {
    const double trace = basis.m[0][0] + basis.m[1][1] + basis.m[2][2];
    Quat out;
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        out.w = 0.25 * s;
        out.x = (basis.m[2][1] - basis.m[1][2]) / s;
        out.y = (basis.m[0][2] - basis.m[2][0]) / s;
        out.z = (basis.m[1][0] - basis.m[0][1]) / s;
    } else if (basis.m[0][0] > basis.m[1][1] && basis.m[0][0] > basis.m[2][2]) {
        const double s = std::sqrt(1.0 + basis.m[0][0] - basis.m[1][1] - basis.m[2][2]) * 2.0;
        out.w = (basis.m[2][1] - basis.m[1][2]) / s;
        out.x = 0.25 * s;
        out.y = (basis.m[0][1] + basis.m[1][0]) / s;
        out.z = (basis.m[0][2] + basis.m[2][0]) / s;
    } else if (basis.m[1][1] > basis.m[2][2]) {
        const double s = std::sqrt(1.0 + basis.m[1][1] - basis.m[0][0] - basis.m[2][2]) * 2.0;
        out.w = (basis.m[0][2] - basis.m[2][0]) / s;
        out.x = (basis.m[0][1] + basis.m[1][0]) / s;
        out.y = 0.25 * s;
        out.z = (basis.m[1][2] + basis.m[2][1]) / s;
    } else {
        const double s = std::sqrt(1.0 + basis.m[2][2] - basis.m[0][0] - basis.m[1][1]) * 2.0;
        out.w = (basis.m[1][0] - basis.m[0][1]) / s;
        out.x = (basis.m[0][2] + basis.m[2][0]) / s;
        out.y = (basis.m[1][2] + basis.m[2][1]) / s;
        out.z = 0.25 * s;
    }
    if (out.w < 0.0) {
        out = Quat(-out.x, -out.y, -out.z, -out.w);
    }
    return normalize_quaternion(out);
}

namespace coordinates {

Vec3 position_godot_to_netsync(const Vec3 &position) {
    return matrix_transform(reflection_matrix(), position);
}

Vec3 position_netsync_to_godot(const Vec3 &position) {
    // The reflection is its own inverse.
    return matrix_transform(reflection_matrix(), position);
}

Mat3 basis_godot_to_netsync(const Mat3 &basis) { return conjugate_with_reflection(basis); }

Mat3 basis_netsync_to_godot(const Mat3 &basis) { return conjugate_with_reflection(basis); }

Quat rotation_godot_to_netsync(const Quat &rotation) {
    return Quat(-rotation.x, -rotation.y, rotation.z, rotation.w);
}

Quat rotation_netsync_to_godot(const Quat &rotation) {
    return Quat(-rotation.x, -rotation.y, rotation.z, rotation.w);
}

double yaw_degrees_godot_to_netsync(double yaw_degrees) { return -yaw_degrees; }

double yaw_degrees_netsync_to_godot(double yaw_degrees) { return -yaw_degrees; }

PoseTransform transform_godot_to_netsync(const PoseTransform &pose) {
    PoseTransform out;
    out.position = position_godot_to_netsync(pose.position);
    out.rotation = rotation_godot_to_netsync(pose.rotation);
    return out;
}

PoseTransform transform_netsync_to_godot(const PoseTransform &pose) {
    PoseTransform out;
    out.position = position_netsync_to_godot(pose.position);
    out.rotation = rotation_netsync_to_godot(pose.rotation);
    return out;
}

}  // namespace coordinates
}  // namespace netsync
}  // namespace styly
