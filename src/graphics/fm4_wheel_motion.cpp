#include <rex/graphics/fm4_wheel_motion.h>

#include <algorithm>
#include <cmath>

namespace rex::graphics {

namespace {

// A turn of more than about 14 degrees a frame on the screen: a car body turns under a degree,
// a wheel rolling at 30 km/h at 120 fps about 12.
constexpr double kSpinRatio = 0.25;
// Object-space probes around the origin, in metres: a wheel's size.
constexpr double kProbeRadius = 0.25;

struct Point {
  double x, y;
  bool valid;
};

Point Project(const std::array<float, 16>& m, double x, double y, double z) {
  double clip[4];
  for (int r = 0; r < 4; ++r) {
    clip[r] = m[r * 4] * x + m[r * 4 + 1] * y + m[r * 4 + 2] * z + m[r * 4 + 3];
  }
  if (clip[3] <= 1.0e-6) return {0.0, 0.0, false};
  return {clip[0] / clip[3], clip[1] / clip[3], true};
}

}  // namespace

std::optional<std::array<double, 16>> Fm4SpinFreeReprojection(
    const std::array<float, 16>& previous_wvp, const std::array<float, 16>& current_wvp) {
  const Point origin_now = Project(current_wvp, 0.0, 0.0, 0.0);
  const Point origin_before = Project(previous_wvp, 0.0, 0.0, 0.0);
  if (!origin_now.valid || !origin_before.valid) return std::nullopt;
  // How far each axis probe turns about the origin on the screen, against its distance from
  // it; an axis seen end-on says nothing.
  double offsets[3] = {}, turns[3] = {};
  for (int axis = 0; axis < 3; ++axis) {
    const double probe[3] = {axis == 0 ? kProbeRadius : 0.0, axis == 1 ? kProbeRadius : 0.0,
                             axis == 2 ? kProbeRadius : 0.0};
    const Point now = Project(current_wvp, probe[0], probe[1], probe[2]);
    const Point before = Project(previous_wvp, probe[0], probe[1], probe[2]);
    if (!now.valid || !before.valid) return std::nullopt;
    const double now_x = now.x - origin_now.x, now_y = now.y - origin_now.y;
    offsets[axis] = std::hypot(now_x, now_y);
    turns[axis] =
        std::hypot(before.x - origin_before.x - now_x, before.y - origin_before.y - now_y);
  }
  const double longest = std::max({offsets[0], offsets[1], offsets[2]});
  if (longest <= 0.0) return std::nullopt;
  bool spins = false;
  for (int axis = 0; axis < 3; ++axis) {
    if (offsets[axis] >= 0.25 * longest && turns[axis] > kSpinRatio * offsets[axis]) {
      spins = true;
    }
  }
  if (!spins) return std::nullopt;
  return std::array<double, 16>{1.0, 0.0, 0.0, origin_before.x - origin_now.x,
                                0.0, 1.0, 0.0, origin_before.y - origin_now.y,
                                0.0, 0.0, 1.0, 0.0,
                                0.0, 0.0, 0.0, 1.0};
}

}  // namespace rex::graphics
