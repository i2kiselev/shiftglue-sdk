/**
 * @file        tests/unit/graphics/fm4_shadow_motion_test.cpp
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <rex/graphics/fm4_shadow_motion.h>

using namespace rex::graphics;

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

// A D3D projection, 60 degrees high, near 0.3, far 7500, looking down +z.
Matrix Projection() {
  const double s = 1.0 / std::tan(30.0 * 3.14159265358979 / 180.0);
  const double a = 7500.0 / (7500.0 - 0.3);
  return {s, 0, 0, 0, 0, s, 0, 0, 0, 0, a, -0.3 * a, 0, 0, 1, 0};
}

Matrix Translate(double x, double y, double z) {
  return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1};
}

// About the x axis: the sun's height.
Matrix RotateX(double angle) {
  const double c = std::cos(angle), s = std::sin(angle);
  return {1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0, 0, 0, 0, 1};
}

// The sun's orthographic view of a 6 m square around the car at (x, 0, z), looking down at 50
// degrees: what FM4's car shadow map pass draws with.
Matrix Sun(double car_x, double car_z) {
  const Matrix ortho = {1.0 / 3.0, 0, 0, 0, 0, 1.0 / 3.0, 0, 0, 0, 0, 1.0 / 20.0, 0.5, 0, 0, 0, 1};
  return Multiply(ortho, Multiply(RotateX(0.87), Translate(-car_x, 0.0, -car_z)));
}

std::array<float, 16> Floats(const Matrix& m) {
  std::array<float, 16> out;
  for (size_t i = 0; i < 16; ++i) out[i] = float(m[i]);
  return out;
}

}  // namespace

TEST_CASE("The car's shadow map pass is orthographic, the scene is not", "[shadow_motion]") {
  CHECK(Fm4OrthographicMatrix(Floats(Sun(3.0, 40.0))));
  CHECK_FALSE(Fm4OrthographicMatrix(Floats(Multiply(Projection(), Translate(0, -1.5, 0)))));
}

TEST_CASE("The ground shadow takes the slot most of the car's meshes share", "[shadow_motion]") {
  // The body's panels share the car's motion; the driver and a mirror got slots of their own.
  CHECK(Fm4ShadowSlot({7, 7, 9, 7, 12, 7}) == 7);
}

TEST_CASE("Meshes new this frame do not vote for the ground shadow's slot", "[shadow_motion]") {
  // Slot 0 is the camera's motion, which the shadow does not have.
  CHECK(Fm4ShadowSlot({0, 0, 0, 4, 4}) == 4);
}

TEST_CASE("No slotted mesh gives the ground shadow no slot", "[shadow_motion]") {
  CHECK(Fm4ShadowSlot({}) == 0);
  CHECK(Fm4ShadowSlot({0, 0}) == 0);
}
