// SPDX-License-Identifier: Apache-2.0
// Quantisation and quaternion compression for STYLY NetSync protocol v8.
//
// Every routine here is a byte-exact port of the upstream reference
// implementation in STYLY-NetSync-Server/src/styly_netsync/binary_serializer.py,
// cross-checked against the Unity implementation in BinarySerializer.cs.
//
// Arithmetic is performed in `double` because the Python reference — the
// designated interoperability baseline — computes in double. See
// docs/UPSTREAM_COMPATIBILITY.md for the (degenerate-input only) consequences.
#pragma once

#include <cstdint>

namespace styly {
namespace netsync {

/// Pi. `M_PI` is a POSIX extension rather than standard C++: MSVC and MinGW
/// leave it undefined unless `_USE_MATH_DEFINES` is set before `<cmath>`, which
/// broke the Windows build. Naming it here gives every translation unit the
/// same value on every toolchain.
inline constexpr double kPi = 3.14159265358979323846;

/// Plain 3-vector in wire (NetSync/Unity) coordinates. Deliberately not a Godot type.
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
};

/// Plain quaternion in wire (NetSync/Unity) coordinates, xyzw ordering.
struct Quat {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;

    Quat() = default;
    Quat(double x_, double y_, double z_, double w_) : x(x_), y(y_), z(z_), w(w_) {}
};

// --- Scales -----------------------------------------------------------------

inline constexpr double kAbsPosScale = 0.01;
inline constexpr double kLocoPosScale = 0.01;
inline constexpr double kRelPosScale = 0.005;
inline constexpr double kPhysicalYawScale = 0.1;

inline constexpr std::int32_t kInt16Min = -32768;
inline constexpr std::int32_t kInt16Max = 32767;
inline constexpr std::int32_t kInt24Min = -(1 << 23);
inline constexpr std::int32_t kInt24Max = (1 << 23) - 1;

/// 1/sqrt(2) as spelled by upstream — the literal matters for byte equality.
inline constexpr double kQuatComponentMin = -0.70710677;
inline constexpr double kQuatComponentMax = 0.70710677;
inline constexpr double kQuatNormalizeEpsilon = 1e-12;

// --- Rounding ---------------------------------------------------------------

/// Round half to even ("banker's rounding").
///
/// This is what both upstream implementations do — Python's built-in `round()`
/// and .NET's `Math.Round(double)` (used by `Mathf.RoundToInt`). `std::round`
/// rounds half away from zero and would produce different bytes at exact `.5`
/// boundaries, so it must not be substituted here.
double round_half_to_even(double value);

// --- Quantisation -----------------------------------------------------------

/// Quantise to signed 16-bit with clamping. Returns 0 for a non-positive scale.
std::int16_t quantize_signed(double value, double scale);

/// Quantise to signed 24-bit with clamping. Returns 0 for a non-positive scale.
std::int32_t quantize_signed_int24(double value, double scale);

inline double dequantize_signed(std::int32_t value, double scale) {
    return static_cast<double>(value) * scale;
}

// --- Quaternion helpers -----------------------------------------------------

/// Normalise, mapping degenerate input (non-finite or magnitude² <= epsilon) to identity.
Quat normalize_quaternion(const Quat &q);

/// Inverse of a unit quaternion (normalises first).
Quat quaternion_inverse(const Quat &q);

/// Hamilton product `a * b`, matching Unity's `Quaternion` operator*.
Quat quaternion_multiply(const Quat &a, const Quat &b);

/// Yaw (rotation about +Y) in degrees, normalised to [-180, 180).
double quaternion_to_yaw_degrees(const Quat &q);

/// Normalise an angle in degrees to [-180, 180).
double normalize_yaw_degrees(double yaw);

/// Yaw-only quaternion from degrees.
Quat yaw_degrees_to_quaternion(double yaw_degrees);

/// Rotate a vector about +Y by `yaw_degrees`, using the upstream (Unity-handed)
/// form `(cos*x + sin*z, y, -sin*x + cos*z)`.
Vec3 rotate_yaw_vector(const Vec3 &v, double yaw_degrees);

// --- Smallest-three quaternion codec ---------------------------------------

/// Pack a quaternion into 32 bits: 2-bit largest index + three 10-bit components.
std::uint32_t compress_quaternion_smallest_three(const Quat &q);

/// Unpack a 32-bit smallest-three quaternion. Always returns a normalised value.
Quat decompress_quaternion_smallest_three(std::uint32_t packed);

}  // namespace netsync
}  // namespace styly
