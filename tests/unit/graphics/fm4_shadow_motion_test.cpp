#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include <rex/graphics/fm4_shadow_motion.h>

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

// A D3D projection, 60 degrees high, near 0.3, far 7500, looking down +z.
Matrix Projection() {
  const double s = 1.0 / std::tan(30.0 * 3.14159265358979 / 180.0);
  const double a = 7500.0 / (7500.0 - 0.3);
  return {s, 0, 0, 0, 0, s, 0, 0, 0, 0, a, -0.3 * a, 0, 0, 1, 0};
}

Matrix Translate(double x, double y, double z) {
  return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1};
}

// About the y axis (up): a car's yaw.
Matrix RotateY(double angle) {
  const double c = std::cos(angle), s = std::sin(angle);
  return {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1};
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

std::array<double, 2> Ndc(const Matrix& transform, double x, double y, double z) {
  double clip[4] = {};
  for (int r = 0; r < 4; ++r) {
    clip[r] = transform[r * 4] * x + transform[r * 4 + 1] * y + transform[r * 4 + 2] * z +
              transform[r * 4 + 3];
  }
  return {clip[0] / clip[3], clip[1] / clip[3]};
}

// The car's parts in the scene and in the shadow map, the car car_z ahead of the camera.
std::vector<std::pair<std::array<float, 16>, std::array<float, 16>>> Parts(const Matrix& view,
                                                                           double car_z) {
  const Matrix car = Multiply(Translate(0.0, 0.0, car_z), RotateY(0.1));
  std::vector<std::pair<std::array<float, 16>, std::array<float, 16>>> parts;
  for (const Matrix& part : {Translate(0, 0.5, 0), Translate(0.8, 0.3, 1.4),
                             Translate(-0.8, 0.3, -1.4), Translate(0, 1.2, -0.3)}) {
    const Matrix world = Multiply(car, part);
    parts.emplace_back(Floats(Multiply(Projection(), Multiply(view, world))),
                       Floats(Multiply(Sun(0.0, car_z), world)));
  }
  return parts;
}

}  // namespace

TEST_CASE("The car's shadow map pass is orthographic, the scene is not", "[shadow_motion]") {
  CHECK(Fm4OrthographicMatrix(Floats(Sun(3.0, 40.0))));
  CHECK_FALSE(Fm4OrthographicMatrix(Floats(Multiply(Projection(), Translate(0, -1.5, 0)))));
}

TEST_CASE("The ground shadow moves with the car, not with the ground under it",
          "[shadow_motion]") {
  const Matrix view = Translate(0.0, -1.5, 0.0);
  // The car pulls away from the camera at 100 km/h and 120 fps.
  const std::optional<Matrix> before = Fm4ShadowToScene(Parts(view, 8.0));
  const std::optional<Matrix> now = Fm4ShadowToScene(Parts(view, 8.23));
  REQUIRE(before.has_value());
  REQUIRE(now.has_value());
  const std::optional<Matrix> reprojection = Fm4ShadowReprojection(*before, *now);
  REQUIRE(reprojection.has_value());
  // A ground point at the shadow's edge, 1.6 m behind the car's centre.
  const Matrix view_projection = Multiply(Projection(), view);
  const std::array<double, 2> edge_before = Ndc(view_projection, 0.4, 0.0, 8.0 - 1.6);
  const std::array<double, 2> asphalt_before = Ndc(view_projection, 0.4, 0.0, 8.23 - 1.6);
  // The pixel's previous place: its current clip position reprojected.
  double clip[4] = {};
  const double world[4] = {0.4, 0.0, 8.23 - 1.6, 1.0};
  for (int r = 0; r < 4; ++r) {
    for (int k = 0; k < 4; ++k) clip[r] += view_projection[r * 4 + k] * world[k];
  }
  double previous[4] = {};
  for (int r = 0; r < 4; ++r) {
    for (int k = 0; k < 4; ++k) previous[r] += (*reprojection)[r * 4 + k] * clip[k];
  }
  const double x = previous[0] / previous[3], y = previous[1] / previous[3];
  CHECK_THAT(x, WithinAbs(edge_before[0], 1e-4));
  CHECK_THAT(y, WithinAbs(edge_before[1], 1e-4));
  // Not where the asphalt was (it did not move, the camera is still).
  CHECK(std::hypot(x - asphalt_before[0], y - asphalt_before[1]) > 1e-3);
}

TEST_CASE("A part drawn with another matrix in the shadow map is outvoted", "[shadow_motion]") {
  const Matrix view = Translate(0.0, -1.5, 0.0);
  auto parts = Parts(view, 8.0);
  const std::optional<Matrix> clean = Fm4ShadowToScene(parts);
  REQUIRE(clean.has_value());
  // A wheel matched to another corner's shadow draw.
  parts.emplace_back(parts[1].first, parts[2].second);
  const std::optional<Matrix> voted = Fm4ShadowToScene(parts);
  REQUIRE(voted.has_value());
  for (size_t i = 0; i < 16; ++i) CHECK_THAT((*voted)[i], WithinAbs((*clean)[i], 1e-3));
}

TEST_CASE("No pairs give no shadow motion", "[shadow_motion]") {
  CHECK_FALSE(Fm4ShadowToScene({}).has_value());
}
