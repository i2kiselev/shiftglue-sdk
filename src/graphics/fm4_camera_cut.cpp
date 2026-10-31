#include <rex/graphics/fm4_camera_cut.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

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

float Fm4AspectScaleX(uint32_t output_width, uint32_t output_height) {
  if (!output_width || !output_height) return 0.0f;
  // (16 / 9) / (width / height): smaller clip x keeps vertical FOV and exposes
  // more world horizontally in a wider target.
  return float(double(output_height) * 16.0 / (double(output_width) * 9.0));
}

bool Fm4ScaleProjectionForAspect(std::array<float, 16>& registers, Fm4DrawPlacement placement,
                                 uint32_t output_width, uint32_t output_height) {
  const float scale = Fm4AspectScaleX(output_width, output_height);
  if (!scale) return false;
  if (placement == Fm4DrawPlacement::kCamera) {
    // c128-c131 hold rows for the column-vector projection (clip X is the first row).
    for (size_t i = 0; i < 4; ++i) registers[i] *= scale;
    return true;
  }
  if (placement == Fm4DrawPlacement::kObject) {
    // c0-c3 hold columns for x * c0 + y * c1 + z * c2 + c3; the x component of each
    // column is the matrix's clip-x row.
    registers[0] *= scale;
    registers[4] *= scale;
    registers[8] *= scale;
    registers[12] *= scale;
    return true;
  }
  return false;
}

bool Fm4ScaleVertexFloatConstantsForAspect(std::array<float, 256 * 4>& constants,
                                           Fm4DrawPlacement placement, uint32_t output_width,
                                           uint32_t output_height) {
  if (placement != Fm4DrawPlacement::kCamera && placement != Fm4DrawPlacement::kObject) {
    return false;
  }
  const size_t matrix_offset = placement == Fm4DrawPlacement::kCamera ? 128 * 4 : 0;
  std::array<float, 16> matrix;
  std::memcpy(matrix.data(), constants.data() + matrix_offset, sizeof(matrix));
  if (!Fm4ScaleProjectionForAspect(matrix, placement, output_width, output_height)) {
    return false;
  }
  std::memcpy(constants.data() + matrix_offset, matrix.data(), sizeof(matrix));
  return true;
}

bool Fm4ScaleVertexFloatConstantsForAspect(std::array<uint32_t, 256 * 4>& constants,
                                           Fm4DrawPlacement placement, uint32_t output_width,
                                           uint32_t output_height) {
  if (placement != Fm4DrawPlacement::kCamera && placement != Fm4DrawPlacement::kObject) {
    return false;
  }
  const size_t offset = placement == Fm4DrawPlacement::kCamera ? 128 * 4 : 0;
  std::array<float, 16> matrix;
  std::memcpy(matrix.data(), constants.data() + offset, sizeof(matrix));
  if (!Fm4ScaleProjectionForAspect(matrix, placement, output_width, output_height)) {
    return false;
  }
  std::memcpy(constants.data() + offset, matrix.data(), sizeof(matrix));
  return true;
}

std::optional<bool> Fm4SceneTurned(const std::array<float, 16>& rows) {
  // Clip y's row: its world-up part against its length.
  const double up = rows[5];
  const double length = std::sqrt(double(rows[4]) * rows[4] + up * up + double(rows[6]) * rows[6]);
  if (std::abs(up) < 0.5 * length) return std::nullopt;
  return up < 0.0;
}

uint32_t Fm4MatrixRegister(const std::array<uint64_t, 4>& float_bitmap, bool dynamic_addressing) {
  if ((float_bitmap[0] & 0xF) == 0xF && dynamic_addressing) return 0;
  if ((float_bitmap[2] & 0xF) == 0xF) return 128;
  if ((float_bitmap[0] & 0xF) == 0xF) return 0;
  return UINT32_MAX;
}

bool Fm4PlacedByObjectMatrix(const std::array<uint64_t, 4>& float_bitmap,
                             const std::array<float, 16>& camera_registers) {
  if ((float_bitmap[0] & 0xF) != 0xF || (float_bitmap[2] & 0xF) != 0xF) return false;
  // Gaussian elimination with partial pivoting; a vanishing pivot is a singular matrix, as
  // the object slots' inversion finds it.
  double a[4][4];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) a[r][c] = camera_registers[r * 4 + c];
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r) {
      if (std::abs(a[r][col]) > std::abs(a[pivot][col])) pivot = r;
    }
    if (std::abs(a[pivot][col]) < 1e-12) return true;
    std::swap(a[pivot], a[col]);
    for (int r = col + 1; r < 4; ++r) {
      const double f = a[r][col] / a[col][col];
      for (int c = col; c < 4; ++c) a[r][c] -= f * a[col][c];
    }
  }
  return false;
}

}  // namespace rex::graphics
