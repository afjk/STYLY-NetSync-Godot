// SPDX-License-Identifier: Apache-2.0
#include "pose_codec.hpp"

#include <cmath>

namespace styly {
namespace netsync {

double round_half_to_even(double value) {
    if (!std::isfinite(value)) {
        return value;
    }
    const double lower = std::floor(value);
    const double fraction = value - lower;
    if (fraction > 0.5) {
        return lower + 1.0;
    }
    if (fraction < 0.5) {
        return lower;
    }
    // Exactly halfway: round towards the even neighbour.
    return std::fmod(lower, 2.0) == 0.0 ? lower : lower + 1.0;
}

namespace {

/// Shared clamp for the quantisers.
///
/// Upstream Python raises on a non-finite input (the message is then never sent);
/// this port instead saturates infinities and maps NaN to zero so a single bad
/// float cannot take the connection down. See docs/UPSTREAM_COMPATIBILITY.md.
std::int32_t clamp_rounded(double scaled, std::int32_t min_value, std::int32_t max_value) {
    if (std::isnan(scaled)) {
        return 0;
    }
    const double rounded = round_half_to_even(scaled);
    if (rounded < static_cast<double>(min_value)) {
        return min_value;
    }
    if (rounded > static_cast<double>(max_value)) {
        return max_value;
    }
    return static_cast<std::int32_t>(rounded);
}

}  // namespace

std::int16_t quantize_signed(double value, double scale) {
    if (!(scale > 0.0)) {
        return 0;
    }
    return static_cast<std::int16_t>(clamp_rounded(value / scale, kInt16Min, kInt16Max));
}

std::int32_t quantize_signed_int24(double value, double scale) {
    if (!(scale > 0.0)) {
        return 0;
    }
    return clamp_rounded(value / scale, kInt24Min, kInt24Max);
}

Quat normalize_quaternion(const Quat &q) {
    const double mag_sq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!std::isfinite(mag_sq) || mag_sq <= kQuatNormalizeEpsilon) {
        return Quat(0.0, 0.0, 0.0, 1.0);
    }
    const double inv_mag = 1.0 / std::sqrt(mag_sq);
    return Quat(q.x * inv_mag, q.y * inv_mag, q.z * inv_mag, q.w * inv_mag);
}

Quat quaternion_inverse(const Quat &q) {
    const Quat n = normalize_quaternion(q);
    return Quat(-n.x, -n.y, -n.z, n.w);
}

Quat quaternion_multiply(const Quat &a, const Quat &b) {
    // Term ordering follows the Python reference exactly so that floating-point
    // accumulation matches bit for bit.
    return Quat(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

double normalize_yaw_degrees(double yaw) {
    // Python: ((yaw + 180.0) % 360.0) - 180.0, where % is Python's floored modulo.
    double shifted = std::fmod(yaw + 180.0, 360.0);
    if (shifted < 0.0) {
        shifted += 360.0;
    }
    return shifted - 180.0;
}

double quaternion_to_yaw_degrees(const Quat &q) {
    const Quat n = normalize_quaternion(q);
    const double siny_cosp = 2.0 * (n.w * n.y + n.z * n.x);
    const double cosy_cosp = 1.0 - 2.0 * (n.y * n.y + n.z * n.z);
    const double yaw = std::atan2(siny_cosp, cosy_cosp) * (180.0 / M_PI);
    return normalize_yaw_degrees(yaw);
}

Quat yaw_degrees_to_quaternion(double yaw_degrees) {
    const double half = (yaw_degrees * (M_PI / 180.0)) * 0.5;
    return Quat(0.0, std::sin(half), 0.0, std::cos(half));
}

Vec3 rotate_yaw_vector(const Vec3 &v, double yaw_degrees) {
    const double yaw_rad = yaw_degrees * (M_PI / 180.0);
    const double cos_y = std::cos(yaw_rad);
    const double sin_y = std::sin(yaw_rad);
    return Vec3((cos_y * v.x) + (sin_y * v.z), v.y, (-sin_y * v.x) + (cos_y * v.z));
}

std::uint32_t compress_quaternion_smallest_three(const Quat &q) {
    const Quat n = normalize_quaternion(q);
    double values[4] = {n.x, n.y, n.z, n.w};
    const double abs_values[4] = {std::fabs(values[0]), std::fabs(values[1]),
                                  std::fabs(values[2]), std::fabs(values[3])};

    // First maximum wins, matching both Python's max() and Unity's strict-`>` scan.
    int largest_index = 0;
    for (int i = 1; i < 4; ++i) {
        if (abs_values[i] > abs_values[largest_index]) {
            largest_index = i;
        }
    }

    if (values[largest_index] < 0.0) {
        for (int i = 0; i < 4; ++i) {
            values[i] = -values[i];
        }
    }

    constexpr int kMax10Bit = 1023;
    std::uint32_t packed = static_cast<std::uint32_t>(largest_index) << 30;
    int write_index = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == largest_index) {
            continue;
        }
        double clamped = values[i];
        if (clamped < kQuatComponentMin) {
            clamped = kQuatComponentMin;
        }
        if (clamped > kQuatComponentMax) {
            clamped = kQuatComponentMax;
        }
        const double normalized =
            (clamped - kQuatComponentMin) / (kQuatComponentMax - kQuatComponentMin);
        double scaled_value = round_half_to_even(normalized * kMax10Bit);
        if (!(scaled_value >= 0.0)) {  // also catches NaN
            scaled_value = 0.0;
        }
        if (scaled_value > kMax10Bit) {
            scaled_value = kMax10Bit;
        }
        const std::uint32_t scaled = static_cast<std::uint32_t>(scaled_value);
        const int shift = 20 - (write_index * 10);
        packed |= scaled << shift;
        ++write_index;
    }
    return packed;
}

Quat decompress_quaternion_smallest_three(std::uint32_t packed) {
    const int largest_index = static_cast<int>((packed >> 30) & 0x3u);
    const std::uint32_t a = (packed >> 20) & 0x3FFu;
    const std::uint32_t b = (packed >> 10) & 0x3FFu;
    const std::uint32_t c = packed & 0x3FFu;

    const double inv = 1.0 / 1023.0;
    const auto decode = [inv](std::uint32_t v) {
        return kQuatComponentMin +
               ((kQuatComponentMax - kQuatComponentMin) * (static_cast<double>(v) * inv));
    };

    double values[4] = {0.0, 0.0, 0.0, 0.0};
    const std::uint32_t read_values[3] = {a, b, c};
    int read_index = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == largest_index) {
            continue;
        }
        values[i] = decode(read_values[read_index]);
        ++read_index;
    }

    double sum_sq = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (i == largest_index) {
            continue;
        }
        sum_sq += values[i] * values[i];
    }
    const double residual = 1.0 - sum_sq;
    values[largest_index] = std::sqrt(residual > 0.0 ? residual : 0.0);

    return normalize_quaternion(Quat(values[0], values[1], values[2], values[3]));
}

}  // namespace netsync
}  // namespace styly
