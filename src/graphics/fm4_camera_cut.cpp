#include <rex/graphics/fm4_camera_cut.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace rex::graphics {

double Fm4ReprojectedScreenShift(const std::array<double, 16>& reprojection, double near_plane,
                                 double far_plane) {
  // Depth 0 at the near plane and 1 at the far one; without a far plane, as if it were at
  // infinity.
  const bool has_far = far_plane > near_plane;
  const double depth = has_far ? std::sqrt(near_plane * far_plane) : 100.0 * near_plane;
  const double z = has_far ? far_plane * (depth - near_plane) / (depth * (far_plane - near_plane))
                           : 1.0 - near_plane / depth;
  double shift = 0.0;
  for (const auto [x, y] : {std::pair{0.0, 0.0}, std::pair{-1.0, -1.0}, std::pair{1.0, -1.0},
                            std::pair{-1.0, 1.0}, std::pair{1.0, 1.0}}) {
    const double point[4] = {x, y, z, 1.0};
    double moved[4] = {};
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) moved[r] += reprojection[r * 4 + c] * point[c];
    }
    if (moved[3] <= 0.0) return std::numeric_limits<double>::infinity();
    shift = std::max({shift, std::abs(moved[0] / moved[3] - x), std::abs(moved[1] / moved[3] - y)});
  }
  return shift;
}

uint32_t Fm4CameraObjectSlot(const std::vector<uint32_t>& slot_draws, uint32_t min_draws) {
  uint32_t best = 0;
  for (uint32_t slot = 1; slot < slot_draws.size(); ++slot) {
    if (slot_draws[slot] >= min_draws && (!best || slot_draws[slot] > slot_draws[best])) {
      best = slot;
    }
  }
  return best;
}

uint32_t Fm4SceneDepth(const std::vector<Fm4DepthCandidate>& candidates) {
  const Fm4DepthCandidate* best = nullptr;
  for (const Fm4DepthCandidate& candidate : candidates) {
    if (!best || candidate.width > best->width ||
        (candidate.width == best->width && candidate.draws > best->draws)) {
      best = &candidate;
    }
  }
  return best ? best->key : 0;
}

std::array<float, 16> Fm4ObjectMatrix(const std::array<float, 16>& registers) {
  std::array<float, 16> rows;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) rows[r * 4 + c] = registers[c * 4 + r];
  }
  return rows;
}

std::optional<bool> Fm4SceneTurned(const std::array<float, 16>& rows) {
  // Clip y's row: its world-up part against its length.
  const double up = rows[5];
  const double length = std::sqrt(double(rows[4]) * rows[4] + up * up + double(rows[6]) * rows[6]);
  if (std::abs(up) < 0.5 * length) return std::nullopt;
  return up < 0.0;
}

}  // namespace rex::graphics
