/**
 * @file        tests/unit/graphics/fm4_wheel_motion_test.cpp
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <tuple>

#include <rex/graphics/fm4_wheel_motion.h>

using namespace rex::graphics;
using Catch::Matchers::WithinAbs;

namespace {

using Matrix = std::array<double, 16>;

Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix out = {};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      for (int k = 0; k < 4; ++k) out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
    }
  }
  return out;
}

// A D3D projection, 60 degrees high, near 0.1, far 7500; the camera at the origin looking
// down +z.
Matrix Projection() {
  const double s = 1.0 / std::tan(30.0 * 3.14159265358979 / 180.0);
  const double a = 7500.0 / (7500.0 - 0.1);
  return {s, 0, 0, 0, 0, s, 0, 0, 0, 0, a, -0.1 * a, 0, 0, 1, 0};
}

Matrix Translate(double x, double y, double z) {
  return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1};
}

// About the x axis: a wheel's axle.
Matrix RotateX(double angle) {
  const double c = std::cos(angle), s = std::sin(angle);
  return {1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0, 0, 0, 0, 1};
}

// About the y axis: a car's yaw.
Matrix RotateY(double angle) {
  const double c = std::cos(angle), s = std::sin(angle);
  return {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1};
}

std::array<float, 16> Floats(const Matrix& m) {
  std::array<float, 16> out;
  for (size_t i = 0; i < 16; ++i) out[i] = float(m[i]);
  return out;
}

// Normalised device x and y of an object-space point.
std::array<double, 2> Ndc(const Matrix& transform, double x, double y, double z) {
  double clip[4] = {};
  for (int r = 0; r < 4; ++r) {
    clip[r] = transform[r * 4] * x + transform[r * 4 + 1] * y + transform[r * 4 + 2] * z +
              transform[r * 4 + 3];
  }
  return {clip[0] / clip[3], clip[1] / clip[3]};
}

// A car's wheel: the car at (x, z) turned by yaw, the wheel at its front left corner, steered
// and turned about its axle.
Matrix Wheel(double x, double z, double yaw, double steer, double spin) {
  return Multiply(Multiply(Translate(x, -0.5, z), RotateY(yaw)),
                  Multiply(Multiply(Translate(-0.8, 0.0, 1.3), RotateY(steer)), RotateX(spin)));
}

// The camera, following the cars: it moves 0.2 m a frame.
Matrix ViewProjection(double z) { return Multiply(Projection(), Translate(0.0, 0.0, -z)); }

struct Frame {
  Matrix world, view_projection;
  std::array<float, 16> Wvp() const { return Floats(Multiply(view_projection, world)); }
  std::array<float, 16> World() const { return Floats(world); }
};

std::optional<std::array<double, 16>> SpinFree(const Frame& before, const Frame& now) {
  return Fm4SpinFreePreviousWvp(before.Wvp(), before.World(), now.World());
}

// Checks that every point of the wheel, its tread included, goes back to where the previous
// frame drew it turned by this frame's spin: the motion of its hub, its steering and the car's
// turn, without the spin.
void CheckSpinFree(const Frame& before, const Frame& now, double spin_before, double spin_now,
                   const Matrix& before_unspun) {
  const std::optional<std::array<double, 16>> previous = SpinFree(before, now);
  REQUIRE(previous.has_value());
  const Matrix target = Multiply(before.view_projection, before_unspun);
  for (const auto [x, y, z] : {std::tuple{0.0, 0.0, 0.0}, std::tuple{0.1, 0.33, 0.0},
                               std::tuple{-0.1, 0.0, -0.33}, std::tuple{0.1, -0.23, 0.23}}) {
    CAPTURE(x, y, z, spin_before, spin_now);
    const std::array<double, 2> moved = Ndc(*previous, x, y, z);
    const std::array<double, 2> expected = Ndc(target, x, y, z);
    CHECK_THAT(moved[0], WithinAbs(expected[0], 1e-4));
    CHECK_THAT(moved[1], WithinAbs(expected[1], 1e-4));
  }
}

}  // namespace

TEST_CASE("A spinning wheel moves as its hub, steering and car do", "[wheel_motion]") {
  // 100 km/h at 120 fps: 0.23 m and 0.7 rad a frame; the car turns half a degree a frame and
  // its driver steers.
  const double spin_before = 1.0, spin_now = 1.7;
  const Frame before = {Wheel(1.0, 8.0, 0.40, 0.10, spin_before), ViewProjection(0.0)};
  const Frame now = {Wheel(1.0, 8.23, 0.409, 0.12, spin_now), ViewProjection(0.2)};
  CheckSpinFree(before, now, spin_before, spin_now, Wheel(1.0, 8.0, 0.40, 0.10, spin_now));
}

TEST_CASE("A spinning wheel seen at an oblique angle is spin-free too", "[wheel_motion]") {
  // A distant car coming at the camera, its wheels seen almost edge-on: on the screen the
  // tread's points sweep a thin ellipse, where the spin used to go unnoticed every other frame.
  // 85 km/h at 120 fps: 0.6 rad a frame, at every phase of the turn.
  for (int phase = 0; phase < 16; ++phase) {
    const double spin_before = 0.2 * phase, spin_now = spin_before + 0.6;
    const Frame before = {Wheel(-12.0, 100.0, 0.0, 0.0, spin_before), ViewProjection(0.0)};
    const Frame now = {Wheel(-12.0, 99.8, 0.0, 0.0, spin_now), ViewProjection(0.2)};
    CheckSpinFree(before, now, spin_before, spin_now, Wheel(-12.0, 100.0, 0.0, 0.0, spin_now));
  }
}

TEST_CASE("Objects that do not spin keep their own reprojection", "[wheel_motion]") {
  // A car body turning into a corner: half a degree a frame.
  const Matrix body_before = Multiply(Translate(0.0, -0.8, 8.23), RotateY(0.5));
  const Matrix body_now = Multiply(Translate(0.0, -0.8, 8.0), RotateY(0.509));
  CHECK_FALSE(
      SpinFree({body_before, ViewProjection(0.0)}, {body_now, ViewProjection(0.0)}).has_value());
  // A wheel rolling slowly (3 degrees a frame): its own motion is followable.
  CHECK_FALSE(SpinFree({Wheel(1.0, 6.01, 0.0, 0.0, 0.35), ViewProjection(0.0)},
                        {Wheel(1.0, 6.0, 0.0, 0.0, 0.3), ViewProjection(0.0)})
                  .has_value());
  // A still prop.
  CHECK_FALSE(SpinFree({Translate(3.0, 0.0, 20.0), ViewProjection(0.0)},
                        {Translate(3.0, 0.0, 20.0), ViewProjection(0.0)})
                  .has_value());
  // A turn about another axis: not a wheel's spin.
  CHECK_FALSE(SpinFree({Multiply(Translate(3.0, 0.0, 20.0), RotateY(0.0)), ViewProjection(0.0)},
                        {Multiply(Translate(3.0, 0.0, 20.0), RotateY(1.5)), ViewProjection(0.0)})
                  .has_value());
}
