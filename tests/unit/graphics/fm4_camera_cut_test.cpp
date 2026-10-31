#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>

#include <rex/graphics/fm4_camera_cut.h>
#include <rex/graphics/fm4_frame_generation_camera.h>

using namespace rex::graphics;
using Catch::Matchers::WithinAbs;

namespace {

using Matrix = std::array<double, 16>;

constexpr Matrix kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// FM4's cameras as logged in a race at 1x (rows rounded to four decimals).
// The hood camera drifting sideways in a corner: the raw entries reach 0.62.
constexpr Matrix kHoodCameraCorner = {0.9997,  0.0141,  -0.6168, 0.6245,  -0.0446, 0.9997,
                                      0.1450,  -0.1367, -0.0101, -0.0036, 0.2074,  0.7926,
                                      -0.0101, -0.0036, -0.7926, 1.7926};
// The countdown's switch to the driving camera.
constexpr Matrix kRaceStartSwitch = {2.4398, 0.0011,  7.3498,   -7.3721, 0.0015, 2.4257,
                                     0.7230, -0.3175, 0.0122,   -0.0701, -10.6622, 11.6565,
                                     0.0122, -0.0701, -13.6617, 14.6560};

// A D3D projection (depth 0 at near, 1 at far) and a turn about the vertical axis, so a
// synthetic reprojection is P * R * P^-1.
Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix out = {};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      for (int k = 0; k < 4; ++k) out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
    }
  }
  return out;
}

Matrix TurnReprojection(double fov_y, double near_plane, double far_plane, double yaw) {
  const double s = 1.0 / std::tan(fov_y / 2.0);
  const double a = far_plane / (far_plane - near_plane);
  const double b = -near_plane * far_plane / (far_plane - near_plane);
  const Matrix projection = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, a, b, 0, 0, 1, 0};
  const Matrix inverse = {1 / s, 0, 0, 0, 0, 1 / s, 0, 0, 0, 0, 0, 1, 0, 0, 1 / b, -a / b};
  const Matrix turn = {std::cos(yaw), 0, std::sin(yaw), 0, 0, 1, 0, 0,
                       -std::sin(yaw), 0, std::cos(yaw), 0, 0, 0, 0, 1};
  return Multiply(Multiply(projection, turn), inverse);
}

}  // namespace

TEST_CASE("a still camera does not move the picture", "[graphics][camera]") {
  CHECK_THAT(Fm4ReprojectedScreenShift(kIdentity, 0.1, 7500.0), WithinAbs(0.0, 1e-12));
}

TEST_CASE("a hood camera drifting sideways in a corner is not a cut", "[graphics][camera]") {
  const double shift = Fm4ReprojectedScreenShift(kHoodCameraCorner, 0.1, 7500.0);
  CHECK(shift > 0.01);
  CHECK(shift < kFm4CameraCutScreenShift);
}

TEST_CASE("the race start's camera switch is a cut", "[graphics][camera]") {
  CHECK(Fm4ReprojectedScreenShift(kRaceStartSwitch, 0.1, 7500.0) >= kFm4CameraCutScreenShift);
}

TEST_CASE("a turn moves the picture by the turn, whatever the near plane",
          "[graphics][camera]") {
  const double fov_y = 60.0 * 3.14159265358979 / 180.0;
  for (const double near_plane : {0.1, 0.3, 1.7}) {
    CAPTURE(near_plane);
    // One degree in a frame, a fast pan: under an eighth of the screen.
    CHECK(Fm4ReprojectedScreenShift(TurnReprojection(fov_y, near_plane, 7500.0, 0.0174533),
                                    near_plane, 7500.0) < kFm4CameraCutScreenShift);
    // Twenty degrees: a new view.
    CHECK(Fm4ReprojectedScreenShift(TurnReprojection(fov_y, near_plane, 7500.0, 0.349066),
                                    near_plane, 7500.0) >= kFm4CameraCutScreenShift);
  }
}

TEST_CASE("a point behind the previous camera is a cut", "[graphics][camera]") {
  const Matrix flipped = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1};
  CHECK(Fm4ReprojectedScreenShift(flipped, 0.1, 7500.0) ==
        std::numeric_limits<double>::infinity());
}

TEST_CASE("an unknown far plane measures far from the camera", "[graphics][camera]") {
  CHECK(Fm4ReprojectedScreenShift(kHoodCameraCorner, 0.1, 0.0) < kFm4CameraCutScreenShift);
  CHECK(Fm4ReprojectedScreenShift(kRaceStartSwitch, 0.1, 0.0) >= kFm4CameraCutScreenShift);
}

TEST_CASE("the camera's motion is the object reprojection most draws share",
          "[graphics][camera]") {
  // The showroom at 1x: 204 draws without a slot, 285 static meshes, a turntable car of 27.
  CHECK(Fm4CameraObjectSlot({204, 285, 3, 0, 27}, 16) == 1);
  CHECK(Fm4CameraObjectSlot({204, 12, 285}, 16) == 2);
  // Draws without a slot are no camera, and a few shared draws are not enough.
  CHECK(Fm4CameraObjectSlot({500, 10, 12}, 16) == 0);
  CHECK(Fm4CameraObjectSlot({}, 16) == 0);
}
TEST_CASE("the scene's depth is the widest the camera draws test, not the busiest",
          "[graphics][camera]") {
  // A replay's long-lens camera at 1x: the reflection cube (280 wide) has 228 camera draws, the
  // scene (1280) 108, a shadow map (320) 9.
  CHECK(Fm4SceneDepth({{0x00203800, 560, 2},
                       {0x00306800, 520, 1},
                       {0x00602040, 320, 9},
                       {0x007038E0, 280, 228},
                       {0x006082D0, 1280, 108}}) == 0x006082D0);
  // Equal widths: the most draws.
  CHECK(Fm4SceneDepth({{1, 1280, 3}, {2, 1280, 40}, {3, 320, 90}}) == 2);
  CHECK(Fm4SceneDepth({}) == 0);
}

TEST_CASE("an object's matrix registers are the columns of its world-view-projection",
          "[graphics][camera]") {
  // c0-c3 of an AI car 33 m ahead, logged in a race at 1x: c3 is where the car's origin
  // lands in clip space.
  const std::array<float, 16> registers = {-1.1040f, -0.0015f, 0.0045f,  0.0045f,
                                           0.0000f,  -1.9387f, -0.1557f, -0.1557f,
                                           -0.0051f, 0.3056f,  -0.9878f, -0.9878f,
                                           5.2096f,  -5.3839f, 33.0749f, 33.3736f};
  const std::array<float, 16> rows = Fm4ObjectMatrix(registers);
  const float point[4] = {1.5f, -0.5f, 2.0f, 1.0f};
  for (int r = 0; r < 4; ++r) {
    float by_rows = 0.0f;
    float by_columns = 0.0f;
    for (int c = 0; c < 4; ++c) {
      by_rows += rows[r * 4 + c] * point[c];
      by_columns += registers[c * 4 + r] * point[c];
    }
    CHECK_THAT(by_rows, WithinAbs(by_columns, 1.0e-5));
  }
  // The origin: clip w is the car's distance.
  CHECK_THAT(rows[15], WithinAbs(33.3736, 1.0e-4));
}

TEST_CASE("frame-generation camera factors P times a translated rotated view",
          "[graphics][camera]") {
  constexpr double kPi = 3.14159265358979323846;
  const double fov_y = 60.0 * kPi / 180.0;
  const double near_plane = 0.1;
  const double far_plane = 7500.0;
  const std::array<double, 3> expected_position = {3.0, -2.0, 17.0};

  const double yaw = 37.0 * kPi / 180.0;
  const double pitch = 12.0 * kPi / 180.0;
  const std::array<double, 3> forward = {std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                         std::cos(pitch) * std::cos(yaw)};
  const auto cross = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return std::array<double, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                 a[0] * b[1] - a[1] * b[0]};
  };
  const auto dot = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  const auto normalized = [](const std::array<double, 3>& value) {
    const double length =
        std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    return std::array<double, 3>{value[0] / length, value[1] / length, value[2] / length};
  };
  const std::array<double, 3> right = normalized(cross({0.0, 1.0, 0.0}, forward));
  const std::array<double, 3> up = normalized(cross(forward, right));
  const Matrix view = {
      right[0],   right[1],   right[2],   -dot(right, expected_position),
      up[0],      up[1],      up[2],      -dot(up, expected_position),
      forward[0], forward[1], forward[2], -dot(forward, expected_position),
      0.0,        0.0,        0.0,        1.0,
  };

  const double scale = 1.0 / std::tan(fov_y / 2.0);
  const double depth_scale = far_plane / (far_plane - near_plane);
  const double depth_bias = -near_plane * far_plane / (far_plane - near_plane);
  for (const double projection_y_sign : {1.0, -1.0}) {
    CAPTURE(projection_y_sign);
    for (const double aspect : {16.0 / 9.0, 9.0 / 16.0}) {
      CAPTURE(aspect);
      const Matrix expected_projection = {
          scale / aspect,
          0.0,
          0.0,
          0.0,
          0.0,
          projection_y_sign * scale,
          0.0,
          0.0,
          0.0,
          0.0,
          depth_scale,
          depth_bias,
          0.0,
          0.0,
          1.0,
          0.0,
      };
      const Matrix world_view_projection = Multiply(expected_projection, view);
      std::array<float, 16> world_view_projection_f32;
      for (size_t i = 0; i < world_view_projection.size(); ++i) {
        world_view_projection_f32[i] = float(world_view_projection[i]);
      }

      const auto camera = Fm4FactorFrameGenerationCamera(world_view_projection_f32, float(fov_y),
                                                         float(near_plane), float(far_plane));
      REQUIRE(camera);

      Matrix view_to_clip;
      Matrix clip_to_view;
      for (size_t i = 0; i < 16; ++i) {
        view_to_clip[i] = camera->view_to_clip[i];
        clip_to_view[i] = camera->clip_to_view[i];
        CHECK_THAT(camera->view_to_clip[i], WithinAbs(expected_projection[i], 2.0e-5));
      }
      const Matrix identity = Multiply(view_to_clip, clip_to_view);
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          CHECK_THAT(identity[r * 4 + c], WithinAbs(r == c ? 1.0 : 0.0, 2.0e-5));
        }
      }

      for (size_t i = 0; i < 3; ++i) {
        CHECK_THAT(camera->position[i], WithinAbs(expected_position[i], 2.0e-4));
        CHECK_THAT(camera->right[i], WithinAbs(right[i], 2.0e-5));
        CHECK_THAT(camera->up[i], WithinAbs(up[i], 2.0e-5));
        CHECK_THAT(camera->forward[i], WithinAbs(forward[i], 2.0e-5));
      }
      const auto basis_length = [](const std::array<float, 3>& basis) {
        return std::sqrt(basis[0] * basis[0] + basis[1] * basis[1] + basis[2] * basis[2]);
      };
      CHECK_THAT(basis_length(camera->right), WithinAbs(1.0, 2.0e-6));
      CHECK_THAT(basis_length(camera->up), WithinAbs(1.0, 2.0e-6));
      CHECK_THAT(basis_length(camera->forward), WithinAbs(1.0, 2.0e-6));
    }
  }
}

TEST_CASE("frame-generation camera rejects a non-projective c128 transform", "[graphics][camera]") {
  std::array<float, 16> identity{};
  for (int i = 0; i < 4; ++i)
    identity[i * 4 + i] = 1.0f;
  CHECK_FALSE(Fm4FactorFrameGenerationCamera(identity, 1.0472f, 0.1f, 1000.0f));

  const std::array<float, 16> world_transform = {1.5f, 0.0f, 0.0f, 10.0f, 0.0f, -2.0f, 0.0f, 20.0f,
                                                 0.0f, 0.0f, 1.0f, 30.0f, 0.0f, 0.0f,  0.0f, 1.0f};
  CHECK_FALSE(Fm4FactorFrameGenerationCamera(world_transform, 0.9272952f, 0.3f, 0.0f));
}

TEST_CASE("frame-generation camera preserves the observed turned c128 signs", "[graphics][camera]") {
  const std::array<float, 16> camera_registers = {
      1.5f, 0.0f, 0.0f, 0.0f, 0.0f, -2.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, -0.3f, 0.0f, 0.0f, 1.0f, 0.0f};
  const auto camera = Fm4FactorFrameGenerationCamera(camera_registers, 0.9272952f, 0.3f, 0.0f);
  REQUIRE(camera);
  CHECK_THAT(camera->view_to_clip[0], WithinAbs(1.5f, 1.0e-6));
  CHECK_THAT(camera->view_to_clip[5], WithinAbs(-2.0f, 1.0e-6));
  CHECK_THAT(camera->position[0], WithinAbs(0.0f, 1.0e-6));
  CHECK_THAT(camera->position[1], WithinAbs(0.0f, 1.0e-6));
  CHECK_THAT(camera->position[2], WithinAbs(0.0f, 1.0e-6));
}

TEST_CASE("a race draws the scene turned 180 degrees, the showroom upright",
          "[graphics][camera]") {
  // A race's camera (c128-c131, logged on the grid at 1x): world up lands on clip -y.
  const std::array<float, 16> race_camera = {-0.339f, -0.000f, -0.797f, -0.003f,
                                             -0.169f, -1.529f, 0.072f,  -137.604f,
                                             0.914f,  -0.119f, -0.389f, -10.806f,
                                             0.914f,  -0.119f, -0.389f, -10.706f};
  CHECK(Fm4SceneTurned(race_camera) == std::optional<bool>(true));
  // The AI car's c0-c3 above: its up axis too.
  const std::array<float, 16> ai_car = {-1.1040f, -0.0015f, 0.0045f,  0.0045f,
                                        0.0000f,  -1.9387f, -0.1557f, -0.1557f,
                                        -0.0051f, 0.3056f,  -0.9878f, -0.9878f,
                                        5.2096f,  -5.3839f, 33.0749f, 33.3736f};
  CHECK(Fm4SceneTurned(Fm4ObjectMatrix(ai_car)) == std::optional<bool>(true));
  // The showroom's car body (c0-c3, logged at 1x): up lands on clip +y.
  const std::array<float, 16> showroom_car = {2.086f,  1.131f,  0.655f,  0.655f,
                                              -0.703f, 4.970f,  -0.057f, -0.057f,
                                              1.868f,  0.607f,  -0.753f, -0.753f,
                                              -1.961f, -3.296f, 3.179f,  3.479f};
  CHECK(Fm4SceneTurned(Fm4ObjectMatrix(showroom_car)) == std::optional<bool>(false));
}

TEST_CASE("a camera rolled on its side or looking straight down does not say",
          "[graphics][camera]") {
  // Rolled 90 degrees: world x is the screen's vertical.
  const std::array<float, 16> rolled = {0, -1.5f, 0, 0, 1.5f, 0, 0, 0, 0, 0, 1, -0.1f,
                                        0, 0, 1, 0};
  CHECK_FALSE(Fm4SceneTurned(rolled).has_value());
  // Looking straight down: world up is the view direction.
  const std::array<float, 16> down = {1.5f, 0, 0, 0, 0, 0, 1.5f, 0, 0, -1, 0, -0.1f,
                                      0, -1, 0, 0};
  CHECK_FALSE(Fm4SceneTurned(down).has_value());
}

TEST_CASE("a shader indexing its constants is placed by c0-c3, not the camera's registers",
          "[graphics][camera]") {
  // The track (c0-c4 and c128-c131 read), a car (c0-c3 and c128-c130), and the driver,
  // whose bone palette is indexed: every register counts as read.
  const std::array<uint64_t, 4> world = {0x1F, 0, 0xF, 0};
  const std::array<uint64_t, 4> car = {0xF, 0, 0x7, 0};
  const std::array<uint64_t, 4> all = {~0ull, ~0ull, ~0ull, ~0ull};
  CHECK(Fm4MatrixRegister(world, false) == 128);
  CHECK(Fm4MatrixRegister(car, false) == 0);
  CHECK(Fm4MatrixRegister(all, true) == 0);
  CHECK(Fm4MatrixRegister({0x3, 0, 0, 0}, false) == UINT32_MAX);
}

TEST_CASE("a part reading the camera's registers as plain constants is placed by c0-c3",
          "[graphics][camera]") {
  // The 599 GTO's exhaust tips: c128-c131 hold constants and the tip's offset, no matrix.
  const std::array<float, 16> constants = {0,     0,    0,     1, 0.77f, 0,     0,      1,
                                           0.5f,  0,    0,     1, -0.9f, 0.24f, 0.04f, 1.07f};
  const std::array<float, 16> camera = {1.5f, 0, 0, 0, 0, -2, 0, 0, 0, 0, 1, -0.3f, 0, 0, 1, 0};
  const std::array<uint64_t, 4> both = {0xF, 0, 0xF, 0};
  const std::array<uint64_t, 4> world = {0, 0, 0xF, 0};
  CHECK(Fm4PlacedByObjectMatrix(both, constants));
  CHECK_FALSE(Fm4PlacedByObjectMatrix(both, camera));
  CHECK_FALSE(Fm4PlacedByObjectMatrix(world, constants));
}
