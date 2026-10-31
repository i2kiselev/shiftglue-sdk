/**
 * @file        graphics/fm4_wheel_motion.cpp
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/fm4_wheel_motion.h>

#include <cmath>

namespace rex::graphics {

namespace {

// More than 5 degrees a frame about the axle: a wheel rolling at about 12 km/h at 120 fps.
// Below it the spin is slow enough for an upscaler to follow; a car body pitches far less.
constexpr double kMinSpin = 0.087;
// The axle may swing by up to about 11 degrees a frame (steering, a car's turn); a turn about
// another axis is not a wheel's spin.
constexpr double kMinAxleCosine = 0.98;

using Matrix3 = std::array<double, 9>;

Matrix3 Upper(const std::array<float, 16>& m) {
  return {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
}

bool Invert(const Matrix3& m, Matrix3& out) {
  const double c0 = m[4] * m[8] - m[5] * m[7];
  const double c1 = m[5] * m[6] - m[3] * m[8];
  const double c2 = m[3] * m[7] - m[4] * m[6];
  const double determinant = m[0] * c0 + m[1] * c1 + m[2] * c2;
  if (std::abs(determinant) < 1e-12) return false;
  const double inverse = 1.0 / determinant;
  out = {c0 * inverse,
         (m[2] * m[7] - m[1] * m[8]) * inverse,
         (m[1] * m[5] - m[2] * m[4]) * inverse,
         c1 * inverse,
         (m[0] * m[8] - m[2] * m[6]) * inverse,
         (m[2] * m[3] - m[0] * m[5]) * inverse,
         c2 * inverse,
         (m[1] * m[6] - m[0] * m[7]) * inverse,
         (m[0] * m[4] - m[1] * m[3]) * inverse};
  return true;
}

Matrix3 Multiply(const Matrix3& a, const Matrix3& b) {
  Matrix3 out = {};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      for (int k = 0; k < 3; ++k) out[r * 3 + c] += a[r * 3 + k] * b[k * 3 + c];
    }
  }
  return out;
}

}  // namespace

std::optional<std::array<double, 16>> Fm4SpinFreePreviousWvp(
    const std::array<float, 16>& previous_wvp, const std::array<float, 16>& previous_world,
    const std::array<float, 16>& current_world) {
  // The turn from the previous frame's object space to this frame's.
  Matrix3 previous_inverse;
  if (!Invert(Upper(previous_world), previous_inverse)) return std::nullopt;
  const Matrix3 turn = Multiply(previous_inverse, Upper(current_world));
  if (turn[0] < kMinAxleCosine) return std::nullopt;
  const double spin = std::atan2(turn[7] - turn[5], turn[4] + turn[8]);
  if (std::abs(spin) <= kMinSpin) return std::nullopt;
  // The previous frame's matrix with this frame's spin: x unchanged, y and z turned.
  const double c = std::cos(spin), s = std::sin(spin);
  std::array<double, 16> out;
  for (int r = 0; r < 4; ++r) {
    const double y = previous_wvp[r * 4 + 1], z = previous_wvp[r * 4 + 2];
    out[r * 4] = previous_wvp[r * 4];
    out[r * 4 + 1] = y * c + z * s;
    out[r * 4 + 2] = z * c - y * s;
    out[r * 4 + 3] = previous_wvp[r * 4 + 3];
  }
  return out;
}

}  // namespace rex::graphics
