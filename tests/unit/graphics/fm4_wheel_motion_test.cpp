#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

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

std::array<float, 16> Wvp(const Matrix& world) {
  const Matrix wvp = Multiply(Projection(), world);
  std::array<float, 16> out;
  for (size_t i = 0; i < 16; ++i) out[i] = float(wvp[i]);
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

// A front wheel 6 m ahead of the camera, coming at it at 100 km/h and 120 fps (0.23 m and
// 0.7 rad a frame).
const Matrix kWheelNow = Multiply(Translate(1.0, -0.5, 6.0), RotateX(0.3));
const Matrix kWheelBefore = Multiply(Translate(1.0, -0.5, 6.23), RotateX(1.0));

}  // namespace

TEST_CASE("A spinning wheel moves with its hub", "[wheel_motion]") {
  const std::optional<std::array<double, 16>> reprojection =
      Fm4SpinFreeReprojection(Wvp(kWheelBefore), Wvp(kWheelNow));
  REQUIRE(reprojection.has_value());
  const Matrix now = Multiply(Projection(), kWheelNow);
  const Matrix before = Multiply(Projection(), kWheelBefore);
  const std::array<double, 2> hub_now = Ndc(now, 0, 0, 0);
  const std::array<double, 2> hub_before = Ndc(before, 0, 0, 0);
  // Every point of the wheel, its tread included, moves on the screen as its hub does.
  for (const auto [y, z] : {std::pair{0.0, 0.0}, std::pair{0.33, 0.0}, std::pair{0.0, -0.33}}) {
    CAPTURE(y, z);
    const std::array<double, 2> point = Ndc(now, 0.1, y, z);
    const std::array<double, 2> moved = Ndc(*reprojection, point[0], point[1], 0.5);
    CHECK_THAT(moved[0] - point[0], WithinAbs(hub_before[0] - hub_now[0], 1e-5));
    CHECK_THAT(moved[1] - point[1], WithinAbs(hub_before[1] - hub_now[1], 1e-5));
  }
}

TEST_CASE("Objects that do not spin keep their own reprojection", "[wheel_motion]") {
  // A car body turning into a corner: half a degree a frame.
  CHECK_FALSE(Fm4SpinFreeReprojection(Wvp(Multiply(Translate(0.0, -0.8, 8.23), RotateY(0.5))),
                                      Wvp(Multiply(Translate(0.0, -0.8, 8.0), RotateY(0.509))))
                  .has_value());
  // A wheel rolling slowly (3 degrees a frame): its own motion is followable.
  CHECK_FALSE(Fm4SpinFreeReprojection(Wvp(Multiply(Translate(1.0, -0.5, 6.01), RotateX(0.35))),
                                      Wvp(Multiply(Translate(1.0, -0.5, 6.0), RotateX(0.3))))
                  .has_value());
  // A still prop.
  CHECK_FALSE(
      Fm4SpinFreeReprojection(Wvp(Translate(3.0, 0.0, 20.0)), Wvp(Translate(3.0, 0.0, 20.0)))
          .has_value());
}
